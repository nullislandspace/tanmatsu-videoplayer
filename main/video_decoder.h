// Video Decoder - H.264 software decoding using esp_h264 component
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Initialize the H.264 decoder for given max dimensions
esp_err_t video_decoder_init(int max_width, int max_height);

// Decode a video chunk (H.264 NAL units) to YUV420 frame
// Returns ESP_OK on success with yuv_out/width/height set
// Returns ESP_ERR_NOT_FINISHED if decoder is buffering (needs more data)
esp_err_t video_decoder_decode(uint8_t* nal_data, size_t nal_size,
                               uint8_t** yuv_out, int* width, int* height);

// Deinitialize decoder and free resources
void video_decoder_deinit(void);
