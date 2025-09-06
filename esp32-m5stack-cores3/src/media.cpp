#include "bsp/esp-bsp.h"
#include <atomic>
#include <opus.h>
#include <peer.h>
#include "esp_log.h"
#include "main.h"
#include <queue>
#include <vector>

#define CHANNELS 1
#define SAMPLE_RATE (16000)
#define BITS_PER_SAMPLE 16

#define PCM_BUFFER_SIZE_BYTES 640
#define PCM_BUFFER_SIZE_SAMPLES (PCM_BUFFER_SIZE_BYTES / sizeof(int16_t))

#define OPUS_BUFFER_SIZE 1276
#define OPUS_ENCODER_BITRATE 30000
#define OPUS_ENCODER_COMPLEXITY 0

// Simple packet queue - no complex threading
#define MAX_AUDIO_PACKETS 20
#define PLAYBACK_INTERVAL_MS 40  // Play audio every 40ms

static const char *TAG = "pipecat_audio";

// Simple audio packet structure
struct AudioPacket {
    std::vector<uint8_t> data;
    uint32_t timestamp;
};

esp_codec_dev_sample_info_t fs = {
    .bits_per_sample = 16,
    .channel = 1,
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

std::atomic<bool> is_playing = false;

// Simple packet queue - no complex synchronization needed
std::queue<AudioPacket> audio_packet_queue;
uint32_t last_playback_time = 0;

void set_is_playing(int16_t *in_buf) {
    bool any_set = false;
    for (size_t i = 0; i < PCM_BUFFER_SIZE_SAMPLES; i++) {
        if (in_buf[i] != -1 && in_buf[i] != 0 && in_buf[i] != 1) {
            any_set = true;
            break;
        }
    }
    is_playing = any_set;
}

void apply_gain(int16_t *samples) {
    return; // Disabled
}

void pipecat_init_audio_capture() {
    ESP_LOGI(TAG, ">>> BSP AUDIO INIT <<<");
    
    esp_err_t ret = bsp_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BSP board init failed: %s", esp_err_to_name(ret));
        return;
    }
    
    spk_codec_dev = bsp_audio_codec_speaker_init();
    if (spk_codec_dev == NULL) {
        ESP_LOGE(TAG, "Failed to initialize speaker codec");
        return;
    }
    
    ret = esp_codec_dev_open(spk_codec_dev, &fs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open speaker codec: %s", esp_err_to_name(ret));
        return;
    }
    
    ret = esp_codec_dev_set_out_vol(spk_codec_dev, 60);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set speaker volume: %s", esp_err_to_name(ret));
    }

    mic_codec_dev = bsp_audio_codec_microphone_init();
    if (mic_codec_dev == NULL) {
        ESP_LOGE(TAG, "Failed to initialize microphone codec");
        return;
    }
    
    ret = esp_codec_dev_set_in_gain(mic_codec_dev, 30.0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set microphone gain: %s", esp_err_to_name(ret));
    }
    
    ret = esp_codec_dev_open(mic_codec_dev, &fs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open microphone codec: %s", esp_err_to_name(ret));
        return;
    }
    
    ESP_LOGI(TAG, ">>> BSP AUDIO INIT COMPLETE <<<");
}

void pipecat_init_audio_decoder() {
    ESP_LOGI(TAG, ">>> BSP OPUS DECODER INIT <<<");
    
    int opus_error = 0;
    opus_decoder = opus_decoder_create(SAMPLE_RATE, 1, &opus_error);
    if (opus_error != OPUS_OK) {
        ESP_LOGE(TAG, "Failed to create OPUS decoder: %d", opus_error);
        return;
    }
    
    opus_decoder_ctl(opus_decoder, OPUS_RESET_STATE);
    opus_decoder_ctl(opus_decoder, OPUS_SET_GAIN(0));
    
    decoder_buffer = (opus_int16 *)malloc(PCM_BUFFER_SIZE_BYTES);
    if (!decoder_buffer) {
        ESP_LOGE(TAG, "Failed to allocate decoder buffer");
        return;
    }
    
    ESP_LOGI(TAG, ">>> BSP OPUS DECODER READY <<<");
}

void pipecat_init_audio_encoder() {
    ESP_LOGI(TAG, ">>> BSP OPUS ENCODER INIT <<<");
    
    int opus_error = 0;
    opus_encoder = opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &opus_error);
    if (opus_error != OPUS_OK) {
        ESP_LOGE(TAG, "Failed to create OPUS encoder: %d", opus_error);
        return;
    }
    
    opus_encoder_ctl(opus_encoder, OPUS_SET_BITRATE(OPUS_ENCODER_BITRATE));
    opus_encoder_ctl(opus_encoder, OPUS_SET_COMPLEXITY(OPUS_ENCODER_COMPLEXITY));
    opus_encoder_ctl(opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    read_buffer = (uint8_t *)heap_caps_malloc(PCM_BUFFER_SIZE_BYTES, MALLOC_CAP_DEFAULT);
    encoder_output_buffer = (uint8_t *)malloc(OPUS_BUFFER_SIZE);
    
    if (!read_buffer || !encoder_output_buffer) {
        ESP_LOGE(TAG, "Failed to allocate encoder buffers");
        return;
    }
    
    ESP_LOGI(TAG, ">>> BSP OPUS ENCODER READY <<<");
}

// Add packet to simple queue
void pipecat_audio_decode(uint8_t *data, size_t size) {
    // Skip DTX (silence) packets  
    if (size <= 3) {
        ESP_LOGD(TAG, "Skipping DTX packet (%d bytes)", size);
        return;
    }
    
    // Validate packet size
    if (size < 10 || size > 400) {
        ESP_LOGW(TAG, "Dropping suspicious packet: %d bytes", size);
        return;
    }
    
    // Drop packets if queue is full
    if (audio_packet_queue.size() >= MAX_AUDIO_PACKETS) {
        ESP_LOGW(TAG, "Audio queue full, dropping oldest packet");
        audio_packet_queue.pop();
    }
    
    // Add to queue
    AudioPacket packet;
    packet.data.assign(data, data + size);
    packet.timestamp = esp_log_timestamp();
    audio_packet_queue.push(packet);
    
    ESP_LOGD(TAG, "Queued packet: %d bytes (queue: %d)", size, audio_packet_queue.size());
}

// Process audio queue at regular intervals - focus on clean OPUS decoding
void pipecat_audio_process() {
    uint32_t current_time = esp_log_timestamp();
    
    // Only process audio at regular intervals
    if (current_time - last_playback_time < PLAYBACK_INTERVAL_MS) {
        return;
    }
    
    if (audio_packet_queue.empty()) {
        return;
    }
    
    last_playback_time = current_time;
    
    // Get next packet
    AudioPacket packet = audio_packet_queue.front();
    audio_packet_queue.pop();
    
    ESP_LOGD(TAG, "Processing packet: %d bytes", packet.data.size());
    
    // Clear the buffer first to avoid artifacts
    memset(decoder_buffer, 0, PCM_BUFFER_SIZE_BYTES);
    
    // Decode OPUS to PCM with Forward Error Correction
    auto decoded_size = opus_decode(opus_decoder, 
                                  packet.data.data(), 
                                  packet.data.size(),
                                  decoder_buffer, 
                                  PCM_BUFFER_SIZE_SAMPLES, 
                                  1);  // Enable FEC
    
    if (decoded_size <= 0) {
        ESP_LOGW(TAG, "OPUS decode failed: %d", decoded_size);
        return;
    }
    
    // Check for decode artifacts - skip frames with too many extreme values
    int extreme_count = 0;
    for (int i = 0; i < decoded_size; i++) {
        if (decoder_buffer[i] > 20000 || decoder_buffer[i] < -20000) {
            extreme_count++;
        }
    }
    
    if (extreme_count > (decoded_size / 4)) {  // More than 25% extreme values
        ESP_LOGW(TAG, "Skipping frame with %d extreme values", extreme_count);
        return;
    }
    
    // Light smoothing to reduce growling artifacts
    for (int i = 1; i < decoded_size - 1; i++) {
        int32_t smoothed = (decoder_buffer[i-1] + decoder_buffer[i] + decoder_buffer[i+1]) / 3;
        decoder_buffer[i] = (int16_t)smoothed;
    }
    
    ESP_LOGD(TAG, "Decoded %d samples (cleaned)", decoded_size);
    
    set_is_playing(decoder_buffer);
    apply_gain(decoder_buffer);
    
    // Play the audio
    size_t bytes_to_write = decoded_size * sizeof(int16_t);
    esp_err_t ret = esp_codec_dev_write(spk_codec_dev, decoder_buffer, bytes_to_write);
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Speaker write failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGD(TAG, "Played %d samples successfully", decoded_size);
    }
}

// Keep original microphone logic - this was working
void pipecat_send_audio(PeerConnection *peer_connection) {
    if (is_playing) {
        memset(read_buffer, 0, PCM_BUFFER_SIZE_BYTES);
    } else {
        esp_err_t ret = esp_codec_dev_read(mic_codec_dev, read_buffer, PCM_BUFFER_SIZE_BYTES);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Microphone read failed: %s", esp_err_to_name(ret));
            memset(read_buffer, 0, PCM_BUFFER_SIZE_BYTES);
        }
    }

    auto encoded_size = opus_encode(opus_encoder, 
                                  (const opus_int16 *)read_buffer,
                                  PCM_BUFFER_SIZE_SAMPLES,
                                  encoder_output_buffer, 
                                  OPUS_BUFFER_SIZE);
    
    if (encoded_size > 0) {
        peer_connection_send_audio(peer_connection, encoder_output_buffer, encoded_size);
        ESP_LOGD(TAG, "Sent %d bytes to peer", encoded_size);
    } else {
        ESP_LOGW(TAG, "OPUS encode failed: %d", encoded_size);
    }
}