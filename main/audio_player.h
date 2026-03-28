// Audio Player - MP3 decode and I2S output (chunk-based streaming)
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Initialize audio subsystem
esp_err_t audio_player_init(void);

// Start audio playback
// Creates audio task and queue for receiving chunks
esp_err_t audio_player_start(void);

// Push audio chunk to playback queue (called from main loop)
// Data is copied, caller can reuse buffer after return
// Returns ESP_OK on success, ESP_ERR_TIMEOUT if queue is full
esp_err_t audio_player_push_chunk(const uint8_t* data, size_t size);

// Signal end of audio stream (no more chunks will be pushed)
void audio_player_end_stream(void);

// Stop audio playback
void audio_player_stop(void);

// Pause/resume audio playback (flushes I2S with silence on pause)
void audio_player_pause(bool pause);

// Check if audio is currently playing
bool audio_player_is_playing(void);

// Get current playback position in milliseconds (for A/V sync)
uint32_t audio_player_get_position_ms(void);

// Set volume (0-100)
void audio_player_set_volume(int volume);

// Deinitialize audio subsystem
void audio_player_deinit(void);
