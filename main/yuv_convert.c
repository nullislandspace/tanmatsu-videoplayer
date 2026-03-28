// YUV420 to BGR888 conversion with 270-degree rotation and letterboxing
//
// Two implementation paths:
//   1. PPA hardware acceleration (preferred) - YUV420→RGB888 + rotation in one DMA op
//   2. Software fallback - CPU-based BT.601 conversion with lookup tables
//
// To switch: set USE_PPA_HARDWARE to 0 to force software path

#include "yuv_convert.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include <string.h>

static const char* TAG = "yuv_convert";

// ============================================================================
// CONFIGURATION: Set to 0 to disable PPA and use software fallback
// ============================================================================
#define USE_PPA_HARDWARE 0

#if USE_PPA_HARDWARE
// ============================================================================
// PPA HARDWARE PATH
// ============================================================================
#include "driver/ppa.h"

// Cache line size for PPA DMA alignment
#define PPA_CACHE_LINE 64

static ppa_client_handle_t ppa_client = NULL;

// Cache-aligned output buffer for PPA (framebuffer may not be aligned)
static uint8_t* ppa_out_buf = NULL;
static size_t ppa_out_buf_size = 0;
// Cache-aligned copy of YUV input (H.264 decoder buffer may not be aligned)
static uint8_t* ppa_in_buf = NULL;
static size_t ppa_in_buf_size = 0;

esp_err_t yuv_convert_init(void) {
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

    // Allocate cache-aligned output buffer for PPA (full display size RGB888)
    // Size must also be aligned to cache line
    ppa_out_buf_size = (800 * 480 * 3 + PPA_CACHE_LINE - 1) & ~(PPA_CACHE_LINE - 1);
    ppa_out_buf = heap_caps_aligned_alloc(PPA_CACHE_LINE, ppa_out_buf_size, MALLOC_CAP_SPIRAM);
    if (!ppa_out_buf) {
        ESP_LOGE(TAG, "Failed to allocate PPA output buffer (%zu bytes)", ppa_out_buf_size);
        ppa_unregister_client(ppa_client);
        ppa_client = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Allocate cache-aligned input buffer for YUV data
    // Max: 800x480 YUV420 = 800*480*1.5 = 576000, aligned up
    ppa_in_buf_size = (800 * 480 * 3 / 2 + PPA_CACHE_LINE - 1) & ~(PPA_CACHE_LINE - 1);
    ppa_in_buf = heap_caps_aligned_alloc(PPA_CACHE_LINE, ppa_in_buf_size, MALLOC_CAP_SPIRAM);
    if (!ppa_in_buf) {
        ESP_LOGE(TAG, "Failed to allocate PPA input buffer (%zu bytes)", ppa_in_buf_size);
        heap_caps_free(ppa_out_buf);
        ppa_out_buf = NULL;
        ppa_unregister_client(ppa_client);
        ppa_client = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "YUV converter initialized (PPA hardware, in=%zu out=%zu bytes)",
             ppa_in_buf_size, ppa_out_buf_size);
    return ESP_OK;
}

esp_err_t yuv_to_framebuffer(uint8_t* yuv_in, uint8_t* fb_out,
                              int src_width, int src_height,
                              int visible_width, int visible_height,
                              int display_width, int display_height) {
    if (!ppa_client || !yuv_in || !fb_out || !ppa_in_buf || !ppa_out_buf) {
        return ESP_ERR_INVALID_ARG;
    }

    // Copy YUV input to cache-aligned buffer (H.264 decoder buffer may not be aligned)
    size_t yuv_size = src_width * src_height * 3 / 2;
    if (yuv_size > ppa_in_buf_size) {
        ESP_LOGE(TAG, "YUV data too large: %zu > %zu", yuv_size, ppa_in_buf_size);
        return ESP_ERR_NO_MEM;
    }
    memcpy(ppa_in_buf, yuv_in, yuv_size);

    // Sync input cache before DMA
    esp_cache_msync(ppa_in_buf, ppa_in_buf_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    // Step 1: PPA converts YUV420→RGB888 (no rotation, no scaling)
    // Output is a flat visible_width × visible_height RGB888 buffer
    size_t rgb_size = visible_width * visible_height * 3;
    // Ensure output buffer size covers this (it's allocated for full display, so always enough)

    ppa_srm_oper_config_t srm_config = {
        .in = {
            .buffer = ppa_in_buf,
            .pic_w = src_width,
            .pic_h = src_height,
            .block_w = visible_width,
            .block_h = visible_height,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_YUV420,
            .yuv_range = PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer = ppa_out_buf,
            .buffer_size = ppa_out_buf_size,
            .pic_w = visible_width,
            .pic_h = visible_height,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mirror_x = false,
        .mirror_y = false,
        .rgb_swap = false,
        .byte_swap = false,
        .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    esp_err_t ret = ppa_do_scale_rotate_mirror(ppa_client, &srm_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA SRM operation failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Sync output cache after DMA
    esp_cache_msync(ppa_out_buf, (rgb_size + PPA_CACHE_LINE - 1) & ~(PPA_CACHE_LINE - 1),
                    ESP_CACHE_MSYNC_FLAG_DIR_M2C);

    // Step 2: Copy PPA RGB888 output to framebuffer with 270° rotation + centering
    // This reuses the same rotation logic as mjpeg_copy_to_framebuffer
    // PPA output is RGB order; display expects BGR — swap R and B during copy
    int offset_x = (display_width - visible_width) / 2;
    int offset_y = (display_height - visible_height) / 2;
    if (offset_x < 0) offset_x = 0;
    if (offset_y < 0) offset_y = 0;

    int fb_stride_bytes = display_height * 3;

    for (int sy = 0; sy < visible_height; sy++) {
        uint8_t* src_row = ppa_out_buf + sy * visible_width * 3;
        int buf_x = display_height - 1 - (offset_y + sy);

        for (int sx = 0; sx < visible_width; sx++) {
            int buf_y = offset_x + sx;
            uint8_t* dst = fb_out + buf_y * fb_stride_bytes + buf_x * 3;
            // PPA outputs RGB, display wants BGR — swap R and B
            dst[0] = src_row[sx * 3 + 2];  // B <- R
            dst[1] = src_row[sx * 3 + 1];  // G
            dst[2] = src_row[sx * 3 + 0];  // R <- B
        }
    }

    return ESP_OK;
}

uint8_t* yuv_convert_get_output_buffer(void) {
    return ppa_out_buf;
}

void yuv_convert_deinit(void) {
    if (ppa_client) {
        ppa_unregister_client(ppa_client);
        ppa_client = NULL;
    }
    if (ppa_out_buf) {
        heap_caps_free(ppa_out_buf);
        ppa_out_buf = NULL;
    }
    if (ppa_in_buf) {
        heap_caps_free(ppa_in_buf);
        ppa_in_buf = NULL;
    }
    ppa_out_buf_size = 0;
    ppa_in_buf_size = 0;
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

void yuv_convert_deinit(void) {
    ESP_LOGI(TAG, "YUV converter deinitialized");
}

#endif // USE_PPA_HARDWARE
