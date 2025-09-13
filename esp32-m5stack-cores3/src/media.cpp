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
#define OPUS_BUFFER_SIZE 1276  // 1276 bytes is recommended by opus_encode
#define PCM_BUFFER_SIZE 640
#define OPUS_ENCODER_BITRATE 96000
#define OPUS_ENCODER_COMPLEXITY 0

std::atomic<bool> is_playing = false;
unsigned int silence_count = 0;

void set_is_playing(int16_t *in_buf, size_t in_samples) {
  // Ultra-fast silence detection - check only first, middle, and last samples
  bool any_set = (abs(in_buf[0]) > 1) || 
                 (abs(in_buf[in_samples/2]) > 1) || 
                 (abs(in_buf[in_samples-1]) > 1);
  
  if (any_set) {
    silence_count = 0;
  } else {
    silence_count++;
  }
  
  if (silence_count >= 20 && is_playing) {
    M5.Speaker.end();
    M5.Mic.begin();
    is_playing = false;
  } else if (any_set && !is_playing) {
    M5.Mic.end();
    M5.Speaker.begin();
    is_playing = true;
  }
}

void pipecat_init_audio_capture() {
  M5.Speaker.setVolume(200);
}

opus_int16 *decoder_buffer = NULL;
OpusDecoder *opus_decoder = NULL;

void pipecat_init_audio_decoder() {
  int decoder_error = 0;
  opus_decoder = opus_decoder_create(SAMPLE_RATE, 1, &decoder_error);
  if (decoder_error != OPUS_OK) {
    printf("Failed to create OPUS decoder");
    return;
  }
  decoder_buffer = (opus_int16 *)heap_caps_malloc(PCM_BUFFER_SIZE * sizeof(opus_int16), MALLOC_CAP_DMA);
}

// === New noise-gated gain filter ===
void process_audio(int16_t *samples, size_t num_samples) {
    int64_t energy = 0;

    // Compute frame energy
    for (size_t i = 0; i < num_samples; i++) {
        energy += (int32_t)samples[i] * samples[i];
    }

    // Noise gate threshold (adjust to taste)
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
    set_is_playing(decoder_buffer, decoded_size);
    if (is_playing) {
      process_audio(decoder_buffer, decoded_size);
      M5.Speaker.playRaw(decoder_buffer, decoded_size, SAMPLE_RATE);
    }
  }
}

OpusEncoder *opus_encoder = NULL;
uint8_t *encoder_output_buffer = NULL;
int16_t *read_buffer = NULL;

void pipecat_init_audio_encoder() {
  int encoder_error;
  opus_encoder = opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &encoder_error);
  if (encoder_error != OPUS_OK) {
    printf("Failed to create OPUS encoder");
    return;
  }
  
  // Optimize for low latency and CPU usage
  opus_encoder_ctl(opus_encoder, OPUS_SET_BITRATE(24000)); // Lower bitrate for better throughput
  opus_encoder_ctl(opus_encoder, OPUS_SET_COMPLEXITY(0));  // Minimum complexity
  opus_encoder_ctl(opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  opus_encoder_ctl(opus_encoder, OPUS_SET_VBR(0)); // Disable VBR for consistent packet sizes
  opus_encoder_ctl(opus_encoder, OPUS_SET_DTX(1)); // Enable discontinuous transmission
  
  // Use DMA-capable memory for better performance
  read_buffer = (int16_t *)heap_caps_malloc(PCM_BUFFER_SIZE * sizeof(int16_t), MALLOC_CAP_DMA);
  encoder_output_buffer = (uint8_t *)heap_caps_malloc(OPUS_BUFFER_SIZE, MALLOC_CAP_DMA);
}

void pipecat_send_audio(PeerConnection *peer_connection) {
  if (is_playing) {
    // If playing, feed silence to encoder
    __builtin_memset(read_buffer, 0, PCM_BUFFER_SIZE * sizeof(int16_t));
    vTaskDelay(pdMS_TO_TICKS(20));
  } else {
    M5.Mic.record(read_buffer, PCM_BUFFER_SIZE / sizeof(uint16_t), SAMPLE_RATE);
  }
  
  int encoded_size = opus_encode(opus_encoder, (const opus_int16 *)read_buffer,
                                 PCM_BUFFER_SIZE / sizeof(uint16_t),
                                 encoder_output_buffer, OPUS_BUFFER_SIZE);
  
  // Only send if encoding was successful and not silence
  if (encoded_size > 2) { // Opus silence packets are typically 1–2 bytes
    peer_connection_send_audio(peer_connection, encoder_output_buffer, encoded_size);
  }
}

// Cleanup function
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
