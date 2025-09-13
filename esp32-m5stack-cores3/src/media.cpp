#include <opus.h>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "main.h"

#define SAMPLE_RATE (16000)
#define OPUS_BUFFER_SIZE 1276
#define PCM_BUFFER_SIZE 640
#define OPUS_ENCODER_BITRATE 96000
#define OPUS_ENCODER_COMPLEXITY 0

// State management with hysteresis
std::atomic<bool> audio_playing = false;
std::atomic<bool> should_be_playing = false;
unsigned int silence_frames = 0;
const unsigned int SILENCE_THRESHOLD_OFF = 25;  // ~500ms of silence
const unsigned int ACTIVITY_THRESHOLD_ON = 3;   // ~60ms of activity

// Audio buffers
opus_int16 *decoder_buffer = NULL;
OpusDecoder *opus_decoder = NULL;
OpusEncoder *opus_encoder = NULL;
uint8_t *encoder_output_buffer = NULL;
int16_t *read_buffer = NULL;

void set_audio_state(bool play_audio) {
    static bool current_state = false;
    
    if (play_audio != current_state) {
        if (play_audio) {
            // Switch to speaker output
            M5.Mic.end();
            vTaskDelay(pdMS_TO_TICKS(10));  // Brief delay to ensure clean switch
            M5.Speaker.begin();
        } else {
            // Switch to microphone input
            M5.Speaker.end();
            vTaskDelay(pdMS_TO_TICKS(10));  // Brief delay to ensure clean switch
            M5.Mic.begin();
        }
        current_state = play_audio;
        audio_playing = play_audio;
    }
}

void update_audio_state(int16_t *audio_data, size_t sample_count) {
    // Check multiple points in the audio buffer for activity
    bool has_audio = false;
    const size_t check_points[] = {0, sample_count/4, sample_count/2, 3*sample_count/4, sample_count-1};
    
    for (size_t i = 0; i < sizeof(check_points)/sizeof(check_points[0]); i++) {
        if (abs(audio_data[check_points[i]]) > 100) {  // Increased threshold to avoid false positives
            has_audio = true;
            break;
        }
    }
    
    // Update state with hysteresis
    if (has_audio) {
        silence_frames = 0;
        should_be_playing = true;
    } else {
        silence_frames++;
        if (silence_frames >= SILENCE_THRESHOLD_OFF) {
            should_be_playing = false;
        }
    }
    
    // Apply state change with additional activity confirmation
    static unsigned int activity_confirmation = 0;
    if (should_be_playing && !audio_playing) {
        activity_confirmation++;
        if (activity_confirmation >= ACTIVITY_THRESHOLD_ON) {
            set_audio_state(true);
            activity_confirmation = 0;
        }
    } else if (!should_be_playing && audio_playing) {
        set_audio_state(false);
        activity_confirmation = 0;
    } else {
        activity_confirmation = 0;
    }
}

void pipecat_init_audio_capture() {
    M5.Speaker.setVolume(200);
    // Start with microphone active by default
    M5.Mic.begin();
    audio_playing = false;
}

void pipecat_init_audio_decoder() {
    int decoder_error = 0;
    opus_decoder = opus_decoder_create(SAMPLE_RATE, 1, &decoder_error);
    if (decoder_error != OPUS_OK) {
        printf("Failed to create OPUS decoder");
        return;
    }
    decoder_buffer = (opus_int16 *)heap_caps_malloc(PCM_BUFFER_SIZE * sizeof(opus_int16), MALLOC_CAP_DMA);
}

void process_audio(int16_t *samples, size_t num_samples) {
    int64_t energy = 0;

    // Compute frame energy
    for (size_t i = 0; i < num_samples; i++) {
        energy += (int32_t)samples[i] * samples[i];
    }

    // Noise gate threshold
    if (energy < 500000) {
        memset(samples, 0, num_samples * sizeof(int16_t));
        return;
    }

    // Apply 1.5x gain with clamping
    for (size_t i = 0; i < num_samples; i++) {
        int32_t s = (int32_t)samples[i] * 3 / 2;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        samples[i] = (int16_t)s;
    }
}

void pipecat_audio_decode(uint8_t *data, size_t size) {
    int decoded_size = opus_decode(opus_decoder, data, size, decoder_buffer, PCM_BUFFER_SIZE, 0);
    
    if (decoded_size > 0) {
        update_audio_state(decoder_buffer, decoded_size);
        if (audio_playing) {
            process_audio(decoder_buffer, decoded_size);
            M5.Speaker.playRaw(decoder_buffer, decoded_size, SAMPLE_RATE);
        }
    }
}

void pipecat_init_audio_encoder() {
    int encoder_error;
    opus_encoder = opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &encoder_error);
    if (encoder_error != OPUS_OK) {
        printf("Failed to create OPUS encoder");
        return;
    }
    
    // Optimize for low latency and CPU usage
    opus_encoder_ctl(opus_encoder, OPUS_SET_BITRATE(24000));
    opus_encoder_ctl(opus_encoder, OPUS_SET_COMPLEXITY(0));
    opus_encoder_ctl(opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(opus_encoder, OPUS_SET_VBR(0));
    opus_encoder_ctl(opus_encoder, OPUS_SET_DTX(1));
    
    // Use DMA-capable memory for better performance
    read_buffer = (int16_t *)heap_caps_malloc(PCM_BUFFER_SIZE * sizeof(int16_t), MALLOC_CAP_DMA);
    encoder_output_buffer = (uint8_t *)heap_caps_malloc(OPUS_BUFFER_SIZE, MALLOC_CAP_DMA);
}

void pipecat_send_audio(PeerConnection *peer_connection) {
    if (audio_playing) {
        // If playing, feed silence to encoder
        memset(read_buffer, 0, PCM_BUFFER_SIZE * sizeof(int16_t));
        vTaskDelay(pdMS_TO_TICKS(20));
    } else {
        M5.Mic.record(read_buffer, PCM_BUFFER_SIZE / sizeof(uint16_t), SAMPLE_RATE);
    }
    
    int encoded_size = opus_encode(opus_encoder, (const opus_int16 *)read_buffer,
                                    PCM_BUFFER_SIZE / sizeof(uint16_t),
                                    encoder_output_buffer, OPUS_BUFFER_SIZE);
    
    // Only send if encoding was successful and not silence
    if (encoded_size > 2) {
        peer_connection_send_audio(peer_connection, encoder_output_buffer, encoded_size);
    }
}

void pipecat_audio_cleanup() {
    if (decoder_buffer) {
        heap_caps_free(decoder_buffer);
        decoder_buffer = NULL;
    }
    
    if (encoder_output_buffer) {
        heap_caps_free(encoder_output_buffer);
        encoder_output_buffer = NULL;
    }
    
    if (read_buffer) {
        heap_caps_free(read_buffer);
        read_buffer = NULL;
    }
    
    ESP_LOGI(LOG_TAG, "Audio cleanup completed");
}