// YUV420 to display conversion with 270-degree rotation, scaling, and letterboxing
//
// Pipeline:
//   1. Reformat I420 planar → PPA-packed YUV420 (byte shuffling, no color math)
//   2. PPA hardware (DMA) - YUV→RGB color conversion + 270° rotation + scaling
//
// The PPA's YUV420 packed format (from ESP32-P4 TRM Table 37.5-1):
//   Odd rows (1,3,5... i.e. 0-indexed even): U interleaved with Y
//     Bytes: U0 Y0 Y1 U2 | Y2 U4 Y3 Y4 | ...  (little-endian 32-bit words)
//   Even rows (2,4,6... i.e. 0-indexed odd): V interleaved with Y
//     Bytes: V0 Y0 Y1 V2 | Y2 V4 Y3 Y4 | ...
//
// To switch: set USE_PPA_HARDWARE to 0 to force pure software path

#include "yuv_convert.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_timer.h"
#include <string.h>

static const char* TAG = "yuv_convert";

// ============================================================================
// CONFIGURATION: Set to 0 to disable PPA and use software fallback
// ============================================================================
#define USE_PPA_HARDWARE 1

#if USE_PPA_HARDWARE
// ============================================================================
// PPA HARDWARE PATH: I420 reformat + PPA YUV→RGB + rotation + scaling
// ============================================================================
#include "driver/ppa.h"

#define PPA_CACHE_LINE 64

static ppa_client_handle_t ppa_client = NULL;
static bool dump_requested = false;
static uint32_t last_reformat_us = 0;
static uint32_t last_ppa_us = 0;

// PPA-packed YUV420 buffer (output of reformat, input to PPA)
static uint8_t* ppa_yuv_buf = NULL;
static size_t ppa_yuv_buf_size = 0;

// Convert I420 planar to PPA-packed YUV420 format.
// PPA YUV420 (from TRM): odd rows interleave Y+U, even rows interleave Y+V.
// Each 4-byte group covers 2 luma pixels plus 1 chroma sample.
// TRM byte layout per 32-bit word (little-endian, StartAddr+0 is lowest):
//   Odd rows:  [StartAddr+0]=U0 [+1]=Y0 [+2]=Y1 [+3]=U2
//   Even rows: [StartAddr+0]=V0 [+1]=Y0 [+2]=Y1 [+3]=V2
//
// For 8 pixels (4 bytes x 2 words), the pattern from the TRM is:
//   Odd:  U0 Y0 Y1 U2 | Y2 U4 Y3 Y4
//   Even: V0 Y0 Y1 V2 | Y2 V4 Y3 Y4
//
// Note: "odd/even rows" in the TRM use 1-based indexing.
// Row 1 (0-indexed row 0) = "odd" = U row.
// Row 2 (0-indexed row 1) = "even" = V row.
// Convert I420 planar to PPA-packed YUV420.
//
// PPA YUV420 memory layout per the ESP32-P4 TRM Table 37.5-1:
// (table shows StartAddr+3 | +2 | +1 | +0, so actual byte order is reversed)
//
// TRM "odd" rows (1,3,5... = 0-indexed 0,2,4...) carry U chroma:
//   Bytes: [U0] [Y0] [Y1] [U2] | [Y2] [Y3] [U4] [Y4] | [Y5] [U6] [Y6] [Y7]
// TRM "even" rows (2,4,6... = 0-indexed 1,3,5...) carry V chroma:
//   Bytes: [V0] [Y0] [Y1] [V2] | [Y2] [Y3] [V4] [Y4] | [Y5] [V6] [Y6] [Y7]
//
// Pattern: 12 bytes per 8 luma pixels (1.5 bytes/pixel).
// Chroma sample indices: C0=c[0], C2=c[1], C4=c[2], C6=c[3] for 8 pixels.
static void i420_to_ppa_yuv420(const uint8_t* y_plane, const uint8_t* u_plane,
                                const uint8_t* v_plane,
                                int y_stride, int uv_stride,
                                int width, int height,
                                uint8_t* out, int out_stride) {
    for (int row = 0; row < height; row++) {
        const uint8_t* y_row = y_plane + row * y_stride;
        // TRM 1-based odd rows (0-indexed even) = U, even rows (0-indexed odd) = V
        const uint8_t* c_row = (row & 1)
            ? v_plane + (row >> 1) * uv_stride
            : u_plane + (row >> 1) * uv_stride;
        uint8_t* dst = out + row * out_stride;

        int x;
        for (x = 0; x + 7 < width; x += 8) {
            int cx = x >> 1;  // chroma index for pixel x
            // Word 0: C0 Y0 Y1 C2
            dst[0]  = c_row[cx + 0];   // C0
            dst[1]  = y_row[x + 0];    // Y0
            dst[2]  = y_row[x + 1];    // Y1
            dst[3]  = c_row[cx + 1];   // C2
            // Word 1: Y2 Y3 C4 Y4
            dst[4]  = y_row[x + 2];    // Y2
            dst[5]  = y_row[x + 3];    // Y3
            dst[6]  = c_row[cx + 2];   // C4
            dst[7]  = y_row[x + 4];    // Y4
            // Word 2: Y5 C6 Y6 Y7
            dst[8]  = y_row[x + 5];    // Y5
            dst[9]  = c_row[cx + 3];   // C6
            dst[10] = y_row[x + 6];    // Y6
            dst[11] = y_row[x + 7];    // Y7
            dst += 12;
        }
        // Remaining pixels (width must be multiple of 2 for YUV420)
        for (; x + 1 < width; x += 2) {
            *dst++ = c_row[x >> 1];
            *dst++ = y_row[x + 0];
            *dst++ = y_row[x + 1];
        }
    }
}

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

    // PPA YUV420 buffer: 1.5 bytes per pixel (same as I420, different layout)
    // Max size for 800x480 source
    ppa_yuv_buf_size = (800 * 480 * 3 / 2 + PPA_CACHE_LINE - 1) & ~(PPA_CACHE_LINE - 1);
    ppa_yuv_buf = heap_caps_aligned_alloc(PPA_CACHE_LINE, ppa_yuv_buf_size, MALLOC_CAP_SPIRAM);
    if (!ppa_yuv_buf) {
        ESP_LOGE(TAG, "Failed to allocate PPA YUV buffer (%zu bytes)", ppa_yuv_buf_size);
        ppa_unregister_client(ppa_client);
        ppa_client = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "YUV converter initialized (h264bsd + PPA, yuv_buf=%zu bytes)",
             ppa_yuv_buf_size);
    return ESP_OK;
}

esp_err_t yuv_to_framebuffer(uint8_t* yuv_in, uint8_t* fb_out,
                              int src_width, int src_height,
                              int visible_width, int visible_height,
                              int display_width, int display_height) {
    if (!ppa_client || !yuv_in || !fb_out || !ppa_yuv_buf) {
        return ESP_ERR_INVALID_ARG;
    }

    // --- Step 1: Reformat I420 planar → PPA-packed YUV420 ---
    uint8_t* y_plane = yuv_in;
    uint8_t* u_plane = yuv_in + src_width * src_height;
    uint8_t* v_plane = u_plane + (src_width * src_height / 4);

    // PPA YUV420 stride: 1.5 bytes per pixel, but must be even width
    // The actual stride in bytes for PPA packed format
    int ppa_stride = visible_width * 3 / 2;  // 1.5 bytes per pixel

    int64_t t_reformat_start = esp_timer_get_time();

    i420_to_ppa_yuv420(y_plane, u_plane, v_plane,
                       src_width, src_width / 2,
                       visible_width, visible_height,
                       ppa_yuv_buf, ppa_stride);

    // Flush to PSRAM for PPA DMA
    size_t yuv_packed_size = (size_t)ppa_stride * visible_height;
    size_t yuv_aligned = (yuv_packed_size + PPA_CACHE_LINE - 1) & ~(PPA_CACHE_LINE - 1);
    esp_cache_msync(ppa_yuv_buf, yuv_aligned, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    last_reformat_us = (uint32_t)(esp_timer_get_time() - t_reformat_start);

    // --- Step 2: PPA YUV→RGB + 270° rotation + scaling ---
    int fb_w = display_height;  // 480 (physical framebuffer width)
    int fb_h = display_width;   // 800 (physical framebuffer height)

    // Scale to fit display while maintaining aspect ratio
    float scale_x = (float)fb_h / visible_width;
    float scale_y = (float)fb_w / visible_height;
    float scale = (scale_x < scale_y) ? scale_x : scale_y;
    if (scale > 1.0f && visible_width >= fb_h && visible_height >= fb_w) {
        scale = 1.0f;
    }

    int out_block_w = (int)(visible_height * scale);
    int out_block_h = (int)(visible_width * scale);

    int offset_x = (fb_w - out_block_w) / 2;
    int offset_y = (fb_h - out_block_h) / 2;
    if (offset_x < 0) offset_x = 0;
    if (offset_y < 0) offset_y = 0;

    // Ensure even offsets for YUV420
    offset_x &= ~1;
    offset_y &= ~1;

    size_t fb_size = (size_t)fb_w * fb_h * 3;
    size_t fb_size_aligned = (fb_size + PPA_CACHE_LINE - 1) & ~(PPA_CACHE_LINE - 1);
    esp_cache_msync(fb_out, fb_size_aligned, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    ppa_srm_oper_config_t srm = {
        .in = {
            .buffer = ppa_yuv_buf,
            .pic_w = visible_width,
            .pic_h = visible_height,
            .block_w = visible_width,
            .block_h = visible_height,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_YUV420,
            .yuv_range = PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
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

    int64_t t_ppa_start = esp_timer_get_time();

    esp_err_t ret = ppa_do_scale_rotate_mirror(ppa_client, &srm);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA failed: %s (scale=%.2f, in=%dx%d, block=%dx%d, off=%d,%d)",
                 esp_err_to_name(ret), scale, visible_width, visible_height,
                 out_block_w, out_block_h, offset_x, offset_y);
        return ret;
    }

    esp_cache_msync(fb_out, fb_size_aligned, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

    last_ppa_us = (uint32_t)(esp_timer_get_time() - t_ppa_start);

    // Dump buffers to SD card if requested
    if (dump_requested) {
        dump_requested = false;
        ESP_LOGI(TAG, "Dumping buffers to SD card...");

        FILE* f = fopen("/sd/ppa_yuv_packed.bin", "wb");
        if (f) {
            uint32_t hdr[4] = { visible_width, visible_height, ppa_stride, 0 };
            fwrite(hdr, sizeof(uint32_t), 4, f);
            fwrite(ppa_yuv_buf, 1, yuv_packed_size, f);
            fclose(f);
            ESP_LOGI(TAG, "Wrote /sd/ppa_yuv_packed.bin");
        }

        f = fopen("/sd/ppa_output.bin", "wb");
        if (f) {
            uint32_t hdr[4] = { fb_w, fb_h, out_block_w, out_block_h };
            fwrite(hdr, sizeof(uint32_t), 4, f);
            fwrite(fb_out, 1, fb_size, f);
            fclose(f);
            ESP_LOGI(TAG, "Wrote /sd/ppa_output.bin");
        }
    }

    return ESP_OK;
}

void yuv_convert_request_dump(void) {
    dump_requested = true;
    ESP_LOGI(TAG, "Buffer dump requested");
}

uint8_t* yuv_convert_get_output_buffer(void) {
    return NULL;  // PPA writes directly to framebuffer
}

uint32_t yuv_convert_get_reformat_us(void) {
    return last_reformat_us;
}

uint32_t yuv_convert_get_ppa_us(void) {
    return last_ppa_us;
}

void yuv_convert_deinit(void) {
    if (ppa_client) {
        ppa_unregister_client(ppa_client);
        ppa_client = NULL;
    }
    if (ppa_yuv_buf) {
        heap_caps_free(ppa_yuv_buf);
        ppa_yuv_buf = NULL;
    }
    ppa_yuv_buf_size = 0;
    ESP_LOGI(TAG, "YUV converter deinitialized");
}

#else
// ============================================================================
// SOFTWARE FALLBACK PATH
// ============================================================================

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

    int offset_x = (display_width - visible_width) / 2;
    int offset_y = (display_height - visible_height) / 2;
    if (offset_x < 0) offset_x = 0;
    if (offset_y < 0) offset_y = 0;

    uint8_t* y_plane = yuv_in;
    uint8_t* u_plane = yuv_in + src_width * src_height;
    uint8_t* v_plane = u_plane + (src_width * src_height / 4);
    int uv_stride = src_width >> 1;
    int fb_stride_bytes = display_height * 3;

    for (int sy = 0; sy < visible_height; sy++) {
        uint8_t* y_row = y_plane + sy * src_width;
        uint8_t* u_row = u_plane + (sy >> 1) * uv_stride;
        uint8_t* v_row = v_plane + (sy >> 1) * uv_stride;
        int buf_x = display_height - 1 - (offset_y + sy);

        for (int sx = 0; sx < visible_width; sx++) {
            int y_val = y_row[sx];
            int u_val = u_row[sx >> 1] - 128;
            int v_val = v_row[sx >> 1] - 128;
            int c = yuv_y_table[y_val];
            int r = c + ((v_val * 359) >> 8);
            int g = c - ((u_val * 88 + v_val * 183) >> 8);
            int b = c + ((u_val * 454) >> 8);
            int buf_y = offset_x + sx;
            uint8_t* dst = fb_out + buf_y * fb_stride_bytes + buf_x * 3;
            dst[0] = clamp_table[b + 256];
            dst[1] = clamp_table[g + 256];
            dst[2] = clamp_table[r + 256];
        }
    }

    return ESP_OK;
}

uint8_t* yuv_convert_get_output_buffer(void) {
    return NULL;
}

void yuv_convert_request_dump(void) {
    // No-op in software path
}

void yuv_convert_deinit(void) {
    ESP_LOGI(TAG, "YUV converter deinitialized");
}

#endif // USE_PPA_HARDWARE
