// YUV420 to BGR888 conversion with 270-degree rotation and letterboxing
// Uses PPA hardware acceleration when available, software fallback otherwise
#pragma once

#include <stdint.h>
#include "esp_err.h"

// Initialize the YUV converter (registers PPA client)
esp_err_t yuv_convert_init(void);

// Convert YUV420 to framebuffer BGR888 with 270° rotation and centering
// yuv_in: I420 planar buffer (Y + U + V) from H.264 decoder
// fb_out: PAX framebuffer pixels (BGR888)
// src_width, src_height: source YUV dimensions (may be macroblock-aligned)
// visible_width, visible_height: actual visible content dimensions
// display_width, display_height: screen dimensions (800x480)
esp_err_t yuv_to_framebuffer(uint8_t* yuv_in, uint8_t* fb_out,
                              int src_width, int src_height,
                              int visible_width, int visible_height,
                              int display_width, int display_height);

// Get the PPA output buffer pointer (can be passed directly to bsp_display_blit)
// Returns NULL if PPA is not initialized or software fallback is active
uint8_t* yuv_convert_get_output_buffer(void);

// Deinitialize the converter (unregisters PPA client)
void yuv_convert_deinit(void);
