// MJPEG Decoder - Hardware JPEG decoding using ESP32-P4 JPEG peripheral
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Initialize the hardware JPEG decoder
// Allocates output buffer for decoded BGR frames
esp_err_t mjpeg_decoder_init(int max_width, int max_height);

// Decode a JPEG frame to BGR888
// jpeg_data: pointer to JPEG data (from AVI chunk)
// jpeg_size: size of JPEG data
// bgr_out: pointer to decoded BGR888 data
// width, height: output frame dimensions (from JPEG header)
// Returns ESP_OK on success
esp_err_t mjpeg_decoder_decode(uint8_t* jpeg_data, size_t jpeg_size,
                                uint8_t** bgr_out, int* width, int* height);

// Copy decoded BGR to framebuffer with 270-degree rotation and letterboxing
// bgr_in: decoded BGR888 from mjpeg_decoder_decode
// fb_out: framebuffer pointer
// src_width, src_height: decoded frame dimensions
// display_width, display_height: total display dimensions for centering
esp_err_t mjpeg_copy_to_framebuffer(uint8_t* bgr_in, uint8_t* fb_out,
                                     int src_width, int src_height,
                                     int display_width, int display_height);

// Deinitialize decoder and free resources
void mjpeg_decoder_deinit(void);

// Get output buffer pointer (for direct access)
uint8_t* mjpeg_decoder_get_output_buffer(void);

// Get output buffer size
size_t mjpeg_decoder_get_output_buffer_size(void);
