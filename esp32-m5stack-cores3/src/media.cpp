#include "main.h"
#include "bsp/esp-bsp.h"
#include <atomic>
#include <opus.h>
#include <peer.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#define GAIN 10.0
#define CHANNELS 1
#define SAMPLE_RATE (16000)
#define BITS_PER_SAMPLE 16
#define PCM_BUFFER_SIZE 640
#define OPUS_BUFFER_SIZE 1276
#define OPUS_ENCODER_BITRATE 30000
#define OPUS_ENCODER_COMPLEXITY 0

// Audio buffer settings
#define AUDIO_BUFFER_SIZE (PCM_BUFFER_SIZE * 8)  // Buffer for multiple audio chunks
#define AUDIO_CHUNK_SIZE (PCM_BUFFER_SIZE / 2)   // Size per audio chunk (320 samples)

// Audio Volume settings
#define SPEAKER_SET_OUT_VOLUME 255

// Global variables
esp_codec_dev_sample_info_t fs = {
    .bits_per_sample = BITS_PER_SAMPLE,
    .channel = CHANNELS,
    .channel_mask = 0,
    .sample_rate = SAMPLE_RATE,
    .mclk_multiple = 0,
};

esp_codec_dev_handle_t mic_codec_dev;
esp_codec_dev_handle_t spk_codec_dev;

OpusDecoder *opus_decoder = NULL;
opus_int16 *decoder_buffer = NULL;

OpusEncoder *opus_encoder = NULL;
uint8_t *encoder_output_buffer = NULL;
uint8_t *read_buffer = NULL;

// Audio buffer and task handles
QueueHandle_t audio_queue;
TaskHandle_t audio_playback_task_handle = NULL;
SemaphoreHandle_t audio_mutex;

std::atomic<bool> is_playing = false;
std::atomic<bool> audio_system_running = true;

// Audio chunk structure for queue
typedef struct {
    int16_t data[AUDIO_CHUNK_SIZE];
    size_t size;
    bool is_silence;
} audio_chunk_t;

// Helper function to detect if audio is playing
void set_is_playing(int16_t *in_buf) {
    bool any_set = false;
    for (size_t i = 0; i < (PCM_BUFFER_SIZE / 2); i++) {
        if (in_buf[i] != -1 && in_buf[i] != 0 && in_buf[i] != 1) {
            any_set = true;
        }
    }
    is_playing = any_set;
}

// Helper function to apply gain to audio samples
void apply_gain(int16_t *samples) {
    for (size_t i = 0; i < (PCM_BUFFER_SIZE / 2); i++) {
        float scaled = (float)samples[i] * GAIN;

        // Clamp to 16-bit range
        if (scaled > 32767.0f)
            scaled = 32767.0f;
        if (scaled < -32768.0f)
            scaled = -32768.0f;

        samples[i] = (int16_t)scaled;
    }
}

// Audio playback task - continuously streams audio to prevent gaps
void pipecat_audio_playback_task(void *parameter) {
    audio_chunk_t chunk;
    int16_t silence_buffer[AUDIO_CHUNK_SIZE];
    memset(silence_buffer, 0, sizeof(silence_buffer));
    
    TickType_t last_audio_time = xTaskGetTickCount();
    const TickType_t silence_timeout = pdMS_TO_TICKS(100); // 100ms timeout
    
    ESP_LOGI(LOG_TAG, "Audio playback task started");
    
    while (audio_system_running) {
        // Try to get audio from queue with short timeout
        if (xQueueReceive(audio_queue, &chunk, pdMS_TO_TICKS(20)) == pdTRUE) {
            // Got real audio data
            set_is_playing(chunk.data);
            esp_codec_dev_write(spk_codec_dev, chunk.data, chunk.size * sizeof(int16_t));
            last_audio_time = xTaskGetTickCount();
        } else {
            // No audio data - check if we should play silence to prevent clicks
            TickType_t current_time = xTaskGetTickCount();
            if ((current_time - last_audio_time) < silence_timeout) {
                // Play silence to maintain audio stream continuity
                esp_codec_dev_write(spk_codec_dev, silence_buffer, sizeof(silence_buffer));
            }
            // After timeout, stop playing silence to save power
        }
        
        vTaskDelay(pdMS_TO_TICKS(1)); // Small delay
    }
    
    vTaskDelete(NULL);
}

// Initialize audio capture (microphone)
void pipecat_init_audio_capture() {
    // Microphone
    mic_codec_dev = bsp_audio_codec_microphone_init();
    esp_codec_dev_set_in_gain(mic_codec_dev, 42.0);
    esp_codec_dev_open(mic_codec_dev, &fs);
    
    // Allocate read buffer for microphone
    read_buffer = (uint8_t *)heap_caps_malloc(PCM_BUFFER_SIZE, MALLOC_CAP_DEFAULT);
    
    ESP_LOGI(LOG_TAG, "Audio capture initialized");
}

// Initialize audio decoder (speaker/playback)
void pipecat_init_audio_decoder() {
    // Speaker
    spk_codec_dev = bsp_audio_codec_speaker_init();
    assert(spk_codec_dev);
    esp_codec_dev_open(spk_codec_dev, &fs);
    esp_codec_dev_set_out_vol(spk_codec_dev, SPEAKER_SET_OUT_VOLUME);

    // Opus Decoder
    int opus_error = 0;
    opus_decoder = opus_decoder_create(SAMPLE_RATE, 1, &opus_error);
    assert(opus_error == OPUS_OK);
    decoder_buffer = (opus_int16 *)malloc(PCM_BUFFER_SIZE);
    
    // Create audio queue and mutex
    audio_queue = xQueueCreate(8, sizeof(audio_chunk_t)); // Buffer 8 chunks
    audio_mutex = xSemaphoreCreateMutex();
    
    // Create audio playback task
    xTaskCreate(pipecat_audio_playback_task, "audio_playback", 4096, NULL, 5, &audio_playback_task_handle);
    
    ESP_LOGI(LOG_TAG, "Audio decoder initialized");
}

// Initialize audio encoder
void pipecat_init_audio_encoder() {
    // Opus Encoder
    int opus_error = 0;
    opus_encoder = opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &opus_error);
    assert(opus_error == OPUS_OK);
    assert(opus_encoder_init(opus_encoder, SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP) == OPUS_OK);

    opus_encoder_ctl(opus_encoder, OPUS_SET_BITRATE(OPUS_ENCODER_BITRATE));
    opus_encoder_ctl(opus_encoder, OPUS_SET_COMPLEXITY(OPUS_ENCODER_COMPLEXITY));
    opus_encoder_ctl(opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    encoder_output_buffer = (uint8_t *)malloc(OPUS_BUFFER_SIZE);
    
    ESP_LOGI(LOG_TAG, "Audio encoder initialized");
}

// Send audio data (encode and send via WebRTC)
void pipecat_send_audio(PeerConnection *peer_connection) {
    if (is_playing) {
        // If audio is playing, send silence to avoid echo
        memset(read_buffer, 0, PCM_BUFFER_SIZE);
    } else {
        // Read from microphone
        ESP_ERROR_CHECK(esp_codec_dev_read(mic_codec_dev, read_buffer, PCM_BUFFER_SIZE));
    }

    // Encode audio with Opus
    auto encoded_size = opus_encode(opus_encoder, (const opus_int16 *)read_buffer,
                                    PCM_BUFFER_SIZE / sizeof(uint16_t),
                                    encoder_output_buffer, OPUS_BUFFER_SIZE);
    
    // Send encoded audio via WebRTC
    peer_connection_send_audio(peer_connection, encoder_output_buffer, encoded_size);
}

// Decode and queue received audio - now non-blocking
void pipecat_audio_decode(uint8_t *data, size_t size) {
    auto decoded_size = opus_decode(opus_decoder, data, size, decoder_buffer, PCM_BUFFER_SIZE / 2, 0);

    if (decoded_size > 0) {
        apply_gain((int16_t *)decoder_buffer);
        
        // Create audio chunk
        audio_chunk_t chunk;
        chunk.size = decoded_size;
        chunk.is_silence = false;
        
        // Copy decoded audio to chunk
        memcpy(chunk.data, decoder_buffer, decoded_size * sizeof(int16_t));
        
        // Try to queue the audio (non-blocking)
        if (xQueueSend(audio_queue, &chunk, 0) != pdTRUE) {
            // Queue full - drop oldest and try again
            audio_chunk_t dummy;
            xQueueReceive(audio_queue, &dummy, 0);
            xQueueSend(audio_queue, &chunk, 0);
            ESP_LOGW(LOG_TAG, "Audio queue full, dropped frame");
        }
    }
}

// Cleanup function
void pipecat_audio_cleanup() {
    audio_system_running = false;
    
    if (audio_playback_task_handle) {
        vTaskDelete(audio_playback_task_handle);
        audio_playback_task_handle = NULL;
    }
    
    if (audio_queue) {
        vQueueDelete(audio_queue);
        audio_queue = NULL;
    }
    
    if (audio_mutex) {
        vSemaphoreDelete(audio_mutex);
        audio_mutex = NULL;
    }
    
    if (decoder_buffer) {
        free(decoder_buffer);
        decoder_buffer = NULL;
    }
    
    if (encoder_output_buffer) {
        free(encoder_output_buffer);
        encoder_output_buffer = NULL;
    }
    
    if (read_buffer) {
        heap_caps_free(read_buffer);
        read_buffer = NULL;
    }
}
