#pragma once

#include <stddef.h>

// Try to load and display the splash screen JPEG for 2 seconds.
// Looks for zotoropu.jpg in the app asset directories on SD and
// internal flash. If the file can't be found or decoded, logs a
// warning and returns immediately — not fatal.
//
// fb_pixels: raw pixel buffer (RGB888 or RGB565, matching display)
// h_res/v_res: physical panel dimensions
// bpp: bytes per pixel (3 for RGB888, 2 for RGB565)
void splash_show(void *fb_pixels, size_t h_res, size_t v_res, int bpp);
