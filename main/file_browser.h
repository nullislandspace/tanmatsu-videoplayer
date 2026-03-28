#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define FB_MAX_ENTRIES 64
#define FB_MAX_PATH    256
#define FB_MAX_NAME    128

typedef enum {
    FB_ENTRY_FILE,
    FB_ENTRY_DIR,
    FB_ENTRY_PARENT,
} fb_entry_type_t;

typedef struct {
    char name[FB_MAX_NAME];
    fb_entry_type_t type;
} fb_entry_t;

typedef struct {
    char current_path[FB_MAX_PATH];
    fb_entry_t entries[FB_MAX_ENTRIES];
    int entry_count;
    int selected_index;
    int scroll_offset;
} file_browser_t;

// Initialize browser at /sd root
void fb_init(file_browser_t* browser);

// Scan current directory, populating entries with dirs + .avi files
void fb_scan(file_browser_t* browser);

// Navigation
void fb_move_up(file_browser_t* browser);
void fb_move_down(file_browser_t* browser);

// Enter selected dir or return selected file path.
// Returns true if a file was selected (path written to out_path).
// Returns false if navigated into a directory (browser state updated, re-scanned).
bool fb_select(file_browser_t* browser, char* out_path, size_t out_path_size);

// Draw the file browser UI using Hershey fonts
// fb_pixels: writable PAX framebuffer, stride/height in pixels
void fb_draw(const file_browser_t* browser, uint8_t* fb_pixels, int stride, int height);
