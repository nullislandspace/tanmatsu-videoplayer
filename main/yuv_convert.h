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

// Request a one-shot dump of the YUV input and PPA output to SD card
void yuv_convert_request_dump(void);

// Convert pre-formatted PPA-packed YUV420 to framebuffer (skips I420→PPA reformat)
// coded_*: the decoder's macroblock-aligned buffer geometry (ppa_stride belongs to it)
// visible_*: how much of that is picture; the remainder is encoder padding
// Returns ESP_ERR_NOT_SUPPORTED when built without PPA — use yuv_to_framebuffer.
esp_err_t ppa_to_framebuffer(uint8_t* ppa_in, int ppa_stride,
                              uint8_t* fb_out,
                              int coded_width, int coded_height,
                              int visible_width, int visible_height,
                              int display_width, int display_height);

// Get last frame's reformat time (I420 → PPA-packed) in microseconds
uint32_t yuv_convert_get_reformat_us(void);

// Get last frame's PPA hardware time in microseconds
uint32_t yuv_convert_get_ppa_us(void);

// Deinitialize the converter (unregisters PPA client)
void yuv_convert_deinit(void);
