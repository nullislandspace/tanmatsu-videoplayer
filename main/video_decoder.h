// Video Decoder - H.264 software decoding using esp_h264 component
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

// Initialize the H.264 decoder for given max dimensions
esp_err_t video_decoder_init(int max_width, int max_height);

// Decode a video chunk (H.264 NAL units) to YUV420 frame
// Returns ESP_OK on success with yuv_out/width/height set
// Returns ESP_ERR_NOT_FINISHED if decoder is buffering (needs more data)
// ppa_out/ppa_stride: if non-NULL, receives PPA-packed YUV420 buffer for direct PPA use
esp_err_t video_decoder_decode(uint8_t* nal_data, size_t nal_size,
                               uint8_t** yuv_out, int* width, int* height,
                               uint8_t** ppa_out, uint32_t* ppa_stride);

// Visible frame size, i.e. the coded size with the SPS frame-cropping
// window applied. h264bsdPicWidth/Height() -- and so the width/height
// video_decoder_decode() reports -- are whole macroblocks, so for any
// stream whose real height is not a multiple of 16 they include rows of
// encoder padding that must never reach the panel.
// Writes the coded size when the stream carries no cropping window.
// Returns false before the first SPS has been parsed.
bool video_decoder_get_visible_size(int* width, int* height);

// Deinitialize decoder and free resources
void video_decoder_deinit(void);
