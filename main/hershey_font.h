// Hershey Vector Font - Direct framebuffer rendering
// Based on the Hershey Simplex font from paulbourke.net/dataformats/hershey/
// Public domain

#ifndef HERSHEY_FONT_H
#define HERSHEY_FONT_H

#include <stdint.h>
#include <stdlib.h>
#include "hershey.h"

// Font metrics
#define HERSHEY_BASE_HEIGHT 21  // Capital letter height in font units

// Set a single pixel in the framebuffer (BGR888 format)
// Takes SCREEN coordinates (as user sees them) and converts to buffer coords
// for 270° rotated display.
// Screen: 800 wide (x) × 480 tall (y) as user sees it
// Buffer: 800 wide × 480 tall in memory
static inline void hershey_set_pixel(uint8_t* fb, int fb_stride, int fb_height,
                                     int screen_x, int screen_y,
                                     uint8_t r, uint8_t g, uint8_t b) {
    // Convert screen coords to buffer coords for 270° display
    // Screen X (0-799) maps to buffer Y (0-479)
    // Screen Y (0-479) maps to buffer X (0-799), inverted
    int buf_x = fb_stride - 1 - screen_y;
    int buf_y = screen_x;

    // Bounds check in buffer space (buffer is 800 wide × 480 tall)
    if (buf_x < 0 || buf_x >= fb_stride || buf_y < 0 || buf_y >= fb_height) {
        return;
    }

    int idx = buf_y * fb_stride * 3 + buf_x * 3;
    fb[idx + 0] = b;  // BGR order
    fb[idx + 1] = g;
    fb[idx + 2] = r;
}

// Draw a line using Bresenham's algorithm
// All coordinates are in SCREEN space (hershey_set_pixel does the rotation)
// thickness: 1 = normal, 2+ = bold (draws parallel lines)
static inline void hershey_draw_line_thick(uint8_t* fb, int fb_stride, int fb_height,
                                           int x0, int y0, int x1, int y1,
                                           uint8_t r, uint8_t g, uint8_t b, int thickness) {
    // Draw multiple parallel lines for thickness
    for (int t = 0; t < thickness; t++) {
        int ox = t;  // Offset in X
        int oy = 0;

        int lx0 = x0 + ox, ly0 = y0 + oy;
        int lx1 = x1 + ox, ly1 = y1 + oy;

        int dx = abs(lx1 - lx0);
        int dy = abs(ly1 - ly0);
        int sx = (lx0 < lx1) ? 1 : -1;
        int sy = (ly0 < ly1) ? 1 : -1;
        int err = dx - dy;

        while (1) {
            hershey_set_pixel(fb, fb_stride, fb_height, lx0, ly0, r, g, b);

            if (lx0 == lx1 && ly0 == ly1) break;

            int e2 = 2 * err;
            if (e2 > -dy) {
                err -= dy;
                lx0 += sx;
            }
            if (e2 < dx) {
                err += dx;
                ly0 += sy;
            }
        }
    }
}

// Normal thickness line (backwards compatible)
static inline void hershey_draw_line(uint8_t* fb, int fb_stride, int fb_height,
                                     int x0, int y0, int x1, int y1,
                                     uint8_t r, uint8_t g, uint8_t b) {
    hershey_draw_line_thick(fb, fb_stride, fb_height, x0, y0, x1, y1, r, g, b, 1);
}

// Draw a single character from the Hershey font
// screen_x, screen_y: SCREEN coordinates (as user sees them, y=0 at top)
// font_height: desired font height in pixels
// thickness: line thickness (1 = normal, 2+ = bold)
// Returns: scaled character width for horizontal advance
static inline int hershey_draw_char_ex(uint8_t* fb, int fb_stride, int fb_height,
                                       int screen_x, int screen_y, char c, float font_height,
                                       uint8_t r, uint8_t g, uint8_t b, int thickness) {
    float scale = font_height / HERSHEY_BASE_HEIGHT;
    // Map ASCII to array index (ASCII 32-126 -> index 0-94)
    int idx = (int)c - 32;
    if (idx < 0 || idx >= 95) {
        return (int)(16 * scale);  // Default width for unsupported chars
    }

    const int* glyph = simplex[idx];
    int num_vertices = glyph[0];
    int char_width = glyph[1];

    // No vertices means space or empty character
    if (num_vertices == 0) {
        return (int)(char_width * scale);
    }

    int start_y = screen_y;

    // Process vertex pairs
    int pen_down = 0;
    int prev_sx = 0, prev_sy = 0;

    for (int i = 0; i < num_vertices; i++) {
        int vx = glyph[2 + i * 2];
        int vy = glyph[2 + i * 2 + 1];

        // Check for pen-up marker
        if (vx == -1 && vy == -1) {
            pen_down = 0;
            continue;
        }

        // Scale glyph coordinates
        // Font Y goes up (0=bottom, 21=top), we need to flip it for screen Y (down)
        int gx = (int)(vx * scale);
        int gy = (int)((HERSHEY_BASE_HEIGHT - vy) * scale);

        // Apply coordinates
        int sx = screen_x + gx;
        int sy = start_y + gy;

        if (pen_down) {
            hershey_draw_line_thick(fb, fb_stride, fb_height,
                                    prev_sx, prev_sy, sx, sy,
                                    r, g, b, thickness);
        }

        prev_sx = sx;
        prev_sy = sy;
        pen_down = 1;
    }

    return (int)(char_width * scale);
}

// Normal weight character (backwards compatible)
static inline int hershey_draw_char(uint8_t* fb, int fb_stride, int fb_height,
                                    int screen_x, int screen_y, char c, float font_height,
                                    uint8_t r, uint8_t g, uint8_t b) {
    return hershey_draw_char_ex(fb, fb_stride, fb_height, screen_x, screen_y, c, font_height, r, g, b, 1);
}

// Draw a string using the Hershey font with configurable thickness
// screen_x, screen_y: screen position (top-left of first character)
// font_height: desired font height in pixels
// thickness: line thickness (1 = normal, 2+ = bold)
static inline void hershey_draw_string_ex(uint8_t* fb, int fb_stride, int fb_height,
                                          int screen_x, int screen_y, const char* str, float font_height,
                                          uint8_t r, uint8_t g, uint8_t b, int thickness) {
    while (*str) {
        screen_x += hershey_draw_char_ex(fb, fb_stride, fb_height, screen_x, screen_y, *str, font_height, r, g, b, thickness);
        str++;
    }
}

// Normal weight string (backwards compatible)
static inline void hershey_draw_string(uint8_t* fb, int fb_stride, int fb_height,
                                       int screen_x, int screen_y, const char* str, float font_height,
                                       uint8_t r, uint8_t g, uint8_t b) {
    hershey_draw_string_ex(fb, fb_stride, fb_height, screen_x, screen_y, str, font_height, r, g, b, 1);
}

// Bold string (thickness = 2)
static inline void hershey_draw_string_bold(uint8_t* fb, int fb_stride, int fb_height,
                                            int screen_x, int screen_y, const char* str, float font_height,
                                            uint8_t r, uint8_t g, uint8_t b) {
    hershey_draw_string_ex(fb, fb_stride, fb_height, screen_x, screen_y, str, font_height, r, g, b, 2);
}

// Calculate the width of a string without drawing it
// font_height: desired font height in pixels
static inline int hershey_string_width(const char* str, float font_height) {
    float scale = font_height / HERSHEY_BASE_HEIGHT;
    int width = 0;
    while (*str) {
        int idx = (int)*str - 32;
        if (idx >= 0 && idx < 95) {
            width += (int)(simplex[idx][1] * scale);
        } else {
            width += (int)(16 * scale);
        }
        str++;
    }
    return width;
}

#endif // HERSHEY_FONT_H
