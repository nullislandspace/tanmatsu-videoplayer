// YUV420 to BGR888 conversion with 270-degree rotation, scaling, and letterboxing
//
// Pipeline:
//   1. esp_image_effects (SIMD) - I420 → BGR888 color conversion
//   2. PPA hardware (DMA)       - 270° rotation + optional scaling
//
// To switch: set USE_PPA_HARDWARE to 0 to force pure software path

#include "yuv_convert.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include <string.h>

static const char* TAG = "yuv_convert";

// ============================================================================
// CONFIGURATION: Set to 0 to disable PPA and use software fallback
// ============================================================================
#define USE_PPA_HARDWARE 1

#if USE_PPA_HARDWARE
// ============================================================================
// esp_image_effects + PPA HARDWARE PATH
// ============================================================================
#include "driver/ppa.h"
#include "esp_imgfx_color_convert.h"

// Cache line size for PPA DMA alignment
#define PPA_CACHE_LINE 64

static ppa_client_handle_t ppa_client = NULL;
static bool dump_requested = false;

// Color converter handle (esp_image_effects)
static esp_imgfx_color_convert_handle_t color_converter = NULL;

// Intermediate BGR888 buffer (output of color conversion, input to PPA rotation)
static uint8_t* ppa_mid_buf = NULL;
static size_t ppa_mid_buf_size = 0;

esp_err_t yuv_convert_init(void) {
    // Register PPA SRM client for rotation/scaling
    ppa_client_config_t ppa_cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };

    esp_err_t ret = ppa_register_client(&ppa_cfg, &ppa_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register PPA client: %s", esp_err_to_name(ret));
        ppa_client = NULL;
        return ret;
    }

    // Allocate intermediate buffer for color conversion output (max source size BGR888)
    ppa_mid_buf_size = (800 * 480 * 3 + PPA_CACHE_LINE - 1) & ~(PPA_CACHE_LINE - 1);
    ppa_mid_buf = heap_caps_aligned_alloc(PPA_CACHE_LINE, ppa_mid_buf_size, MALLOC_CAP_SPIRAM);
    if (!ppa_mid_buf) {
        ESP_LOGE(TAG, "Failed to allocate mid buffer (%zu bytes)", ppa_mid_buf_size);
        ppa_unregister_client(ppa_client);
        ppa_client = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "YUV converter initialized (esp_image_effects + PPA, mid=%zu bytes)",
             ppa_mid_buf_size);
    return ESP_OK;
}

esp_err_t yuv_to_framebuffer(uint8_t* yuv_in, uint8_t* fb_out,
                              int src_width, int src_height,
                              int visible_width, int visible_height,
                              int display_width, int display_height) {
    if (!ppa_client || !yuv_in || !fb_out || !ppa_mid_buf) {
        return ESP_ERR_INVALID_ARG;
    }

    // --- Pass 1: I420 → BGR888 via esp_image_effects (SIMD-accelerated) ---

    // Create or reconfigure color converter for current dimensions
    // (dimensions may change between videos)
    if (!color_converter) {
        esp_imgfx_color_convert_cfg_t cc_cfg = {
            .in_res = { .width = src_width, .height = src_height },
            .in_pixel_fmt = ESP_IMGFX_PIXEL_FMT_I420,
            .out_pixel_fmt = ESP_IMGFX_PIXEL_FMT_BGR888,
            .color_space_std = ESP_IMGFX_COLOR_SPACE_STD_BT601,
        };
        esp_imgfx_err_t ie_ret = esp_imgfx_color_convert_open(&cc_cfg, &color_converter);
        if (ie_ret != ESP_IMGFX_ERR_OK) {
            ESP_LOGE(TAG, "Failed to open color converter: %d", ie_ret);
            return ESP_FAIL;
        }
    }

    size_t yuv_size = src_width * src_height * 3 / 2;
    size_t bgr_size = (size_t)visible_width * visible_height * 3;

    esp_imgfx_data_t in_img = { .data = yuv_in, .data_len = yuv_size };
    esp_imgfx_data_t out_img = { .data = ppa_mid_buf, .data_len = bgr_size };

    esp_imgfx_err_t ie_ret = esp_imgfx_color_convert_process(color_converter, &in_img, &out_img);
    if (ie_ret != ESP_IMGFX_ERR_OK) {
        ESP_LOGE(TAG, "Color conversion failed: %d", ie_ret);
        return ESP_FAIL;
    }

    // Flush color conversion output to PSRAM for PPA DMA
    size_t mid_size_aligned = (bgr_size + PPA_CACHE_LINE - 1) & ~(PPA_CACHE_LINE - 1);
    esp_cache_msync(ppa_mid_buf, mid_size_aligned, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    // --- Pass 2: BGR888 → BGR888 + 270° rotation (+ optional scaling) via PPA ---

    // Physical framebuffer dimensions (portrait: 480w x 800h)
    int fb_w = display_height;  // 480
    int fb_h = display_width;   // 800

    // After 270° rotation: input W x H -> output H x W
    // With scaling: output becomes (H * scale_y) x (W * scale_x)
    // We scale to fit the display while maintaining aspect ratio
    float scale_x = (float)fb_h / visible_width;   // scale to fill height
    float scale_y = (float)fb_w / visible_height;   // scale to fill width
    // Use the smaller scale to maintain aspect ratio (letterbox/pillarbox)
    float scale = (scale_x < scale_y) ? scale_x : scale_y;
    // Clamp to max PPA supports and avoid upscaling beyond 1:1 if video is larger
    if (scale > 1.0f && visible_width >= fb_h && visible_height >= fb_w) {
        scale = 1.0f;
    }

    int out_block_w = (int)(visible_height * scale);
    int out_block_h = (int)(visible_width * scale);

    int offset_x = (fb_w - out_block_w) / 2;
    int offset_y = (fb_h - out_block_h) / 2;
    if (offset_x < 0) offset_x = 0;
    if (offset_y < 0) offset_y = 0;

    // PPA writes directly to the framebuffer (must be 64-byte aligned)
    size_t fb_size = (size_t)fb_w * fb_h * 3;
    size_t fb_size_aligned = (fb_size + PPA_CACHE_LINE - 1) & ~(PPA_CACHE_LINE - 1);

    // Flush framebuffer cache before PPA DMA writes to it
    esp_cache_msync(fb_out, fb_size_aligned, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    ppa_srm_oper_config_t srm_rotate = {
        .in = {
            .buffer = ppa_mid_buf,
            .pic_w = visible_width,
            .pic_h = visible_height,
            .block_w = visible_width,
            .block_h = visible_height,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
        },
        .out = {
            .buffer = fb_out,
            .buffer_size = fb_size_aligned,
            .pic_w = fb_w,
            .pic_h = fb_h,
            .block_offset_x = offset_x,
            .block_offset_y = offset_y,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_270,
        .scale_x = scale,
        .scale_y = scale,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    esp_err_t ret = ppa_do_scale_rotate_mirror(ppa_client, &srm_rotate);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA rotate failed: %s (scale=%.2f, in=%dx%d, out_block=%dx%d, offset=%d,%d)",
                 esp_err_to_name(ret), scale, visible_width, visible_height,
                 out_block_w, out_block_h, offset_x, offset_y);
        return ret;
    }

    // Invalidate framebuffer cache after PPA DMA wrote to it
    esp_cache_msync(fb_out, fb_size_aligned, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

    // Dump buffers to SD card if requested
    if (dump_requested) {
        dump_requested = false;
        ESP_LOGI(TAG, "Dumping buffers to SD card...");

        FILE* f = fopen("/sd/ppa_converted.bin", "wb");
        if (f) {
            uint32_t hdr[4] = { visible_width, visible_height, 0, 0 };
            fwrite(hdr, sizeof(uint32_t), 4, f);
            fwrite(ppa_mid_buf, 1, bgr_size, f);
            fclose(f);
            ESP_LOGI(TAG, "Wrote /sd/ppa_converted.bin");
        }

        f = fopen("/sd/ppa_rotated.bin", "wb");
        if (f) {
            uint32_t hdr[4] = { fb_w, fb_h, out_block_w, out_block_h };
            fwrite(hdr, sizeof(uint32_t), 4, f);
            fwrite(fb_out, 1, fb_size, f);
            fclose(f);
            ESP_LOGI(TAG, "Wrote /sd/ppa_rotated.bin");
        }
    }

    return ESP_OK;
}

void yuv_convert_request_dump(void) {
    dump_requested = true;
    ESP_LOGI(TAG, "Buffer dump requested");
}

uint8_t* yuv_convert_get_output_buffer(void) {
    return NULL;  // PPA writes directly to framebuffer now
}

void yuv_convert_deinit(void) {
    if (color_converter) {
        esp_imgfx_color_convert_close(color_converter);
        color_converter = NULL;
    }
    if (ppa_client) {
        ppa_unregister_client(ppa_client);
        ppa_client = NULL;
    }
    if (ppa_mid_buf) {
        heap_caps_free(ppa_mid_buf);
        ppa_mid_buf = NULL;
    }
    ppa_mid_buf_size = 0;
    ESP_LOGI(TAG, "YUV converter deinitialized");
}

#else
// ============================================================================
// SOFTWARE FALLBACK PATH
// Replace USE_PPA_HARDWARE with 0 above to use this path
// ============================================================================

// BT.601 Y lookup table: (Y - 16) * 1.164, clamped to [0..255]
static DRAM_ATTR const int16_t yuv_y_table[256] = {
    -19, -18, -16, -15, -14, -13, -12, -10, -9, -8, -7, -6, -4, -3, -2, -1,
    0, 1, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 15, 16, 17, 18,
    19, 21, 22, 23, 24, 25, 27, 28, 29, 30, 31, 33, 34, 35, 36, 37,
    39, 40, 41, 42, 43, 45, 46, 47, 48, 49, 51, 52, 53, 54, 55, 57,
    58, 59, 60, 61, 63, 64, 65, 66, 67, 69, 70, 71, 72, 73, 75, 76,
    77, 78, 79, 81, 82, 83, 84, 85, 87, 88, 89, 90, 91, 93, 94, 95,
    96, 97, 99, 100, 101, 102, 103, 105, 106, 107, 108, 109, 111, 112, 113, 114,
    115, 117, 118, 119, 120, 121, 123, 124, 125, 126, 127, 129, 130, 131, 132, 133,
    135, 136, 137, 138, 139, 141, 142, 143, 144, 145, 147, 148, 149, 150, 151, 153,
    154, 155, 156, 157, 159, 160, 161, 162, 163, 165, 166, 167, 168, 169, 171, 172,
    173, 174, 175, 177, 178, 179, 180, 181, 183, 184, 185, 186, 187, 189, 190, 191,
    192, 193, 195, 196, 197, 198, 199, 201, 202, 203, 204, 205, 207, 208, 209, 210,
    211, 213, 214, 215, 216, 217, 219, 220, 221, 222, 223, 225, 226, 227, 228, 229,
    231, 232, 233, 234, 235, 237, 238, 239, 240, 241, 243, 244, 245, 246, 247, 249,
    250, 251, 252, 253, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255
};

// Clamp table: maps [-256..511] to [0..255]
static DRAM_ATTR uint8_t clamp_table[768];
static bool clamp_inited = false;

static void init_clamp_table(void) {
    if (clamp_inited) return;
    for (int i = 0; i < 768; i++) {
        int val = i - 256;
        if (val < 0) clamp_table[i] = 0;
        else if (val > 255) clamp_table[i] = 255;
        else clamp_table[i] = val;
    }
    clamp_inited = true;
}

esp_err_t yuv_convert_init(void) {
    init_clamp_table();
    ESP_LOGI(TAG, "YUV converter initialized (software fallback)");
    return ESP_OK;
}

esp_err_t yuv_to_framebuffer(uint8_t* yuv_in, uint8_t* fb_out,
                              int src_width, int src_height,
                              int visible_width, int visible_height,
                              int display_width, int display_height) {
    if (!yuv_in || !fb_out) {
        return ESP_ERR_INVALID_ARG;
    }

    // Centering offsets in screen coordinates
    int offset_x = (display_width - visible_width) / 2;
    int offset_y = (display_height - visible_height) / 2;
    if (offset_x < 0) offset_x = 0;
    if (offset_y < 0) offset_y = 0;

    // YUV420 plane pointers (I420: Y, then U, then V)
    uint8_t* y_plane = yuv_in;
    uint8_t* u_plane = yuv_in + src_width * src_height;
    uint8_t* v_plane = u_plane + (src_width * src_height / 4);
    int uv_stride = src_width >> 1;

    // Framebuffer stride after 270° rotation
    int fb_stride_bytes = display_height * 3;

    for (int sy = 0; sy < visible_height; sy++) {
        uint8_t* y_row = y_plane + sy * src_width;
        uint8_t* u_row = u_plane + (sy >> 1) * uv_stride;
        uint8_t* v_row = v_plane + (sy >> 1) * uv_stride;

        // Screen Y → buffer X after 270° rotation
        int buf_x = display_height - 1 - (offset_y + sy);

        for (int sx = 0; sx < visible_width; sx++) {
            int y_val = y_row[sx];
            int u_val = u_row[sx >> 1] - 128;
            int v_val = v_row[sx >> 1] - 128;

            // BT.601 YUV→RGB
            int c = yuv_y_table[y_val];
            int r = c + ((v_val * 359) >> 8);
            int g = c - ((u_val * 88 + v_val * 183) >> 8);
            int b = c + ((u_val * 454) >> 8);

            // Screen X → buffer Y after 270° rotation
            int buf_y = offset_x + sx;

            uint8_t* dst = fb_out + buf_y * fb_stride_bytes + buf_x * 3;
            dst[0] = clamp_table[b + 256];  // BGR order
            dst[1] = clamp_table[g + 256];
            dst[2] = clamp_table[r + 256];
        }
    }

    return ESP_OK;
}

uint8_t* yuv_convert_get_output_buffer(void) {
    return NULL;  // Software path writes directly to fb_out
}

void yuv_convert_request_dump(void) {
    // No-op in software path
}

void yuv_convert_deinit(void) {
    ESP_LOGI(TAG, "YUV converter deinitialized");
}

#endif // USE_PPA_HARDWARE
