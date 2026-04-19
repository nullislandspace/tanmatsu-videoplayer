// File Browser - SD card directory listing with Hershey font UI

#include "file_browser.h"
#include "hershey_font.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>

// UI constants (screen coordinates: 800x480)
#define HEADER_HEIGHT   30
#define ITEM_HEIGHT     26
#define ITEM_PADDING    4
#define LEFT_MARGIN     10
#define FONT_SIZE       20
#define HEADER_FONT     16
#define VISIBLE_ITEMS   ((480 - HEADER_HEIGHT - ITEM_PADDING) / ITEM_HEIGHT)

// Colors (BGR888)
#define COLOR_BG_R       0
#define COLOR_BG_G       0
#define COLOR_BG_B       0
#define COLOR_TEXT_R    255
#define COLOR_TEXT_G    255
#define COLOR_TEXT_B    255
#define COLOR_DIR_R    100
#define COLOR_DIR_G    200
#define COLOR_DIR_B    255
#define COLOR_SEL_R     40
#define COLOR_SEL_G     60
#define COLOR_SEL_B    120
#define COLOR_HEADER_R 180
#define COLOR_HEADER_G 180
#define COLOR_HEADER_B 180

// Check if filename has .avi extension (case-insensitive)
static bool has_avi_extension(const char* name) {
    size_t len = strlen(name);
    if (len < 4) return false;
    const char* ext = name + len - 4;
    return (tolower(ext[0]) == '.' &&
            tolower(ext[1]) == 'a' &&
            tolower(ext[2]) == 'v' &&
            tolower(ext[3]) == 'i');
}

// Compare entries for sorting: directories first, then alphabetically
static int entry_compare(const void* a, const void* b) {
    const fb_entry_t* ea = (const fb_entry_t*)a;
    const fb_entry_t* eb = (const fb_entry_t*)b;

    // Parent always first
    if (ea->type == FB_ENTRY_PARENT) return -1;
    if (eb->type == FB_ENTRY_PARENT) return 1;

    // Directories before files
    if (ea->type == FB_ENTRY_DIR && eb->type == FB_ENTRY_FILE) return -1;
    if (ea->type == FB_ENTRY_FILE && eb->type == FB_ENTRY_DIR) return 1;

    // Alphabetical (case-insensitive)
    return strcasecmp(ea->name, eb->name);
}

void fb_init(file_browser_t* browser) {
    memset(browser, 0, sizeof(file_browser_t));
    strncpy(browser->current_path, "/sd", FB_MAX_PATH - 1);
    fb_scan(browser);
}

void fb_scan(file_browser_t* browser) {
    browser->entry_count = 0;
    browser->selected_index = 0;
    browser->scroll_offset = 0;

    // Add ".." entry unless at /sd root
    if (strcmp(browser->current_path, "/sd") != 0) {
        fb_entry_t* entry = &browser->entries[browser->entry_count++];
        strncpy(entry->name, "..", FB_MAX_NAME - 1);
        entry->type = FB_ENTRY_PARENT;
    }

    DIR* dir = opendir(browser->current_path);
    if (!dir) return;

    struct dirent* de;
    while ((de = readdir(dir)) != NULL && browser->entry_count < FB_MAX_ENTRIES) {
        // Skip hidden files
        if (de->d_name[0] == '.') continue;

        // Check if directory
        char fullpath[512];
        int n = snprintf(fullpath, sizeof(fullpath), "%s/%s", browser->current_path, de->d_name);
        if (n < 0 || (size_t)n >= sizeof(fullpath)) continue;

        struct stat st;
        if (stat(fullpath, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            fb_entry_t* entry = &browser->entries[browser->entry_count++];
            snprintf(entry->name, FB_MAX_NAME, "%s/", de->d_name);
            entry->type = FB_ENTRY_DIR;
        } else if (has_avi_extension(de->d_name)) {
            fb_entry_t* entry = &browser->entries[browser->entry_count++];
            snprintf(entry->name, FB_MAX_NAME, "%s", de->d_name);
            entry->type = FB_ENTRY_FILE;
        }
    }

    closedir(dir);

    // Sort entries
    if (browser->entry_count > 1) {
        int start = (browser->entries[0].type == FB_ENTRY_PARENT) ? 1 : 0;
        qsort(&browser->entries[start], browser->entry_count - start,
              sizeof(fb_entry_t), entry_compare);
    }
}

void fb_move_up(file_browser_t* browser) {
    if (browser->entry_count == 0) return;
    if (browser->selected_index > 0) {
        browser->selected_index--;
    } else {
        browser->selected_index = browser->entry_count - 1;
    }
    if (browser->selected_index < browser->scroll_offset) {
        browser->scroll_offset = browser->selected_index;
    } else if (browser->selected_index >= browser->scroll_offset + VISIBLE_ITEMS) {
        browser->scroll_offset = browser->selected_index - VISIBLE_ITEMS + 1;
    }
}

void fb_move_down(file_browser_t* browser) {
    if (browser->entry_count == 0) return;
    if (browser->selected_index < browser->entry_count - 1) {
        browser->selected_index++;
    } else {
        browser->selected_index = 0;
    }
    if (browser->selected_index >= browser->scroll_offset + VISIBLE_ITEMS) {
        browser->scroll_offset = browser->selected_index - VISIBLE_ITEMS + 1;
    } else if (browser->selected_index < browser->scroll_offset) {
        browser->scroll_offset = browser->selected_index;
    }
}

bool fb_select(file_browser_t* browser, char* out_path, size_t out_path_size) {
    if (browser->entry_count == 0) return false;

    fb_entry_t* selected = &browser->entries[browser->selected_index];

    if (selected->type == FB_ENTRY_PARENT) {
        // Go up one directory
        char* last_slash = strrchr(browser->current_path, '/');
        if (last_slash && last_slash != browser->current_path) {
            *last_slash = '\0';
        }
        // Don't go above /sd
        if (strlen(browser->current_path) < 3) {
            strncpy(browser->current_path, "/sd", FB_MAX_PATH - 1);
        }
        fb_scan(browser);
        return false;
    }

    if (selected->type == FB_ENTRY_DIR) {
        // Enter directory (strip trailing slash from name for path building)
        char dirname[FB_MAX_NAME];
        strncpy(dirname, selected->name, FB_MAX_NAME - 1);
        dirname[FB_MAX_NAME - 1] = '\0';
        size_t len = strlen(dirname);
        if (len > 0 && dirname[len - 1] == '/') {
            dirname[len - 1] = '\0';
        }

        size_t cur_len = strlen(browser->current_path);
        size_t dir_len = strlen(dirname);
        if (cur_len + 1 + dir_len < FB_MAX_PATH) {
            browser->current_path[cur_len] = '/';
            memcpy(browser->current_path + cur_len + 1, dirname, dir_len + 1);
        }
        fb_scan(browser);
        return false;
    }

    // File selected - build full path
    snprintf(out_path, out_path_size, "%s/%s", browser->current_path, selected->name);
    return true;
}

// Fill a rectangle in screen coordinates with a solid color
static void fill_rect(uint8_t* fb, int fb_stride, int fb_height,
                      int sx, int sy, int sw, int sh,
                      uint8_t r, uint8_t g, uint8_t b) {
    for (int y = sy; y < sy + sh && y < 480; y++) {
        for (int x = sx; x < sx + sw && x < 800; x++) {
            hershey_set_pixel(fb, fb_stride, fb_height, x, y, r, g, b);
        }
    }
}

void fb_draw(const file_browser_t* browser, uint8_t* fb_pixels, int stride, int height) {
    // Clear screen to black
    memset(fb_pixels, 0, stride * height * 3);

    // Draw header with current path
    hershey_draw_string(fb_pixels, stride, height,
                        LEFT_MARGIN, 5, browser->current_path, HEADER_FONT,
                        COLOR_HEADER_R, COLOR_HEADER_G, COLOR_HEADER_B);

    // Draw horizontal separator
    fill_rect(fb_pixels, stride, height,
              0, HEADER_HEIGHT - 2, 800, 1, 80, 80, 80);

    if (browser->entry_count == 0) {
        hershey_draw_string(fb_pixels, stride, height,
                            LEFT_MARGIN, HEADER_HEIGHT + 20, "No .avi files found", FONT_SIZE,
                            COLOR_HEADER_R, COLOR_HEADER_G, COLOR_HEADER_B);
        return;
    }

    // Draw visible entries
    int visible_count = VISIBLE_ITEMS;
    if (visible_count > browser->entry_count - browser->scroll_offset) {
        visible_count = browser->entry_count - browser->scroll_offset;
    }

    for (int i = 0; i < visible_count; i++) {
        int entry_idx = browser->scroll_offset + i;
        const fb_entry_t* entry = &browser->entries[entry_idx];
        int y = HEADER_HEIGHT + ITEM_PADDING + i * ITEM_HEIGHT;

        // Draw selection highlight
        if (entry_idx == browser->selected_index) {
            fill_rect(fb_pixels, stride, height,
                      0, y - 2, 800, ITEM_HEIGHT, COLOR_SEL_R, COLOR_SEL_G, COLOR_SEL_B);
        }

        // Choose color based on entry type
        uint8_t tr, tg, tb;
        if (entry->type == FB_ENTRY_DIR || entry->type == FB_ENTRY_PARENT) {
            tr = COLOR_DIR_R;
            tg = COLOR_DIR_G;
            tb = COLOR_DIR_B;
        } else {
            tr = COLOR_TEXT_R;
            tg = COLOR_TEXT_G;
            tb = COLOR_TEXT_B;
        }

        hershey_draw_string(fb_pixels, stride, height,
                            LEFT_MARGIN, y, entry->name, FONT_SIZE, tr, tg, tb);
    }

    // Draw scroll indicator if needed
    if (browser->entry_count > VISIBLE_ITEMS) {
        char indicator[32];
        snprintf(indicator, sizeof(indicator), "%d/%d",
                 browser->selected_index + 1, browser->entry_count);
        int tw = hershey_string_width(indicator, HEADER_FONT);
        hershey_draw_string(fb_pixels, stride, height,
                            800 - tw - LEFT_MARGIN, 5, indicator, HEADER_FONT,
                            COLOR_HEADER_R, COLOR_HEADER_G, COLOR_HEADER_B);
    }
}
