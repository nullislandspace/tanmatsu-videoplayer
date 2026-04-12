#include "splash.h"

#include <stdlib.h>
#include <string.h>

#include "bsp/display.h"
#include "driver/jpeg_decode.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fastopen.h"

static const char *TAG = "splash";

// Splash screen JPEG — installed as an app asset by the launcher.
// Try both SD and internal flash install locations.
#define SPLASH_PATH_SD  "/sd/apps/at.cavac.zoetrope/zotoropu.jpg"
#define SPLASH_PATH_INT "/int/apps/at.cavac.zoetrope/zotoropu.jpg"
#define SPLASH_DURATION_MS 4000

void splash_show(void *fb_pixels, size_t h_res, size_t v_res, int bpp) {
    if (fb_pixels == NULL || h_res == 0 || v_res == 0) return;

    // Try both install locations.
    FILE *f = fastopen(SPLASH_PATH_SD, "rb");
    if (!f) f = fastopen(SPLASH_PATH_INT, "rb");
    if (!f) {
        ESP_LOGW(TAG, "splash not found at %s or %s",
                 SPLASH_PATH_SD, SPLASH_PATH_INT);
        return;
    }

    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz <= 0 || fsz > 2 * 1024 * 1024) {
        ESP_LOGE(TAG, "bad file size %ld", fsz);
        fastclose(f);
        return;
    }

    // Allocate JPEG input buffer via the decoder's alignment helper.
    jpeg_decode_memory_alloc_cfg_t in_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    size_t   in_alloc = 0;
    uint8_t *in_buf   = jpeg_alloc_decoder_mem((size_t)fsz, &in_cfg, &in_alloc);
    if (!in_buf) {
        ESP_LOGE(TAG, "alloc input (%ld B) failed", fsz);
        fastclose(f);
        return;
    }
    size_t got = fread(in_buf, 1, (size_t)fsz, f);
    fastclose(f);
    if (got != (size_t)fsz) {
        ESP_LOGE(TAG, "short read %zu != %ld", got, fsz);
        free(in_buf);
        return;
    }

    // Create a temporary decoder engine.
    jpeg_decoder_handle_t dec = NULL;
    jpeg_decode_engine_cfg_t dec_eng = { .timeout_ms = 3000 };
    esp_err_t err = jpeg_new_decoder_engine(&dec_eng, &dec);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_new_decoder_engine: %d", err);
        free(in_buf);
        return;
    }

    // Get image dimensions from the header.
    jpeg_decode_picture_info_t info = {0};
    err = jpeg_decoder_get_info(in_buf, (uint32_t)fsz, &info);
    if (err != ESP_OK || info.width == 0 || info.height == 0) {
        ESP_LOGE(TAG, "bad JPEG header");
        free(in_buf);
        jpeg_del_decoder_engine(dec);
        return;
    }

    // HW decoder emits full MCU rows -> pad to multiple of 16.
    uint32_t padded_w = (info.width  + 15u) & ~15u;
    uint32_t padded_h = (info.height + 15u) & ~15u;
    size_t   out_sz   = (size_t)padded_w * padded_h * (size_t)bpp;

    jpeg_decode_memory_alloc_cfg_t out_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t   out_alloc = 0;
    uint8_t *out_buf   = jpeg_alloc_decoder_mem(out_sz, &out_cfg, &out_alloc);
    if (!out_buf) {
        ESP_LOGE(TAG, "alloc output (%zu B) failed", out_sz);
        free(in_buf);
        jpeg_del_decoder_engine(dec);
        return;
    }

    jpeg_decode_cfg_t decode_cfg = {
        .output_format = (bpp == 2) ? JPEG_DECODE_OUT_FORMAT_RGB565
                                    : JPEG_DECODE_OUT_FORMAT_RGB888,
        .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };
    uint32_t dec_out_sz = 0;
    err = jpeg_decoder_process(dec, &decode_cfg, in_buf, (uint32_t)fsz,
                               out_buf, out_alloc, &dec_out_sz);
    free(in_buf);
    jpeg_del_decoder_engine(dec);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "jpeg decode failed: %d", err);
        free(out_buf);
        return;
    }

    // Clear the framebuffer.
    memset(fb_pixels, 0, h_res * v_res * (size_t)bpp);

    // The decoded image is landscape (800x480). The panel framebuffer is
    // portrait (h_res=480 x v_res=800). Apply 270° rotation to map
    // user-landscape coordinates to panel-native coordinates:
    //   panel_x = h_res - 1 - user_y
    //   panel_y = user_x
    // This matches the PAX_O_ROT_CW / BSP_DISPLAY_ROTATION_270 transform.
    int display_w = (int)v_res;  // user-landscape width  = panel v_res (800)
    int display_h = (int)h_res;  // user-landscape height = panel h_res (480)
    int img_w = (int)info.width;
    int img_h = (int)info.height;
    int src_stride = (int)padded_w;

    // Centre the image in user-landscape space.
    int offset_x = (display_w - img_w) / 2;
    int offset_y = (display_h - img_h) / 2;
    if (offset_x < 0) offset_x = 0;
    if (offset_y < 0) offset_y = 0;
    int copy_w = img_w; if (copy_w > display_w) copy_w = display_w;
    int copy_h = img_h; if (copy_h > display_h) copy_h = display_h;

    uint8_t *dst = (uint8_t *)fb_pixels;
    int fb_row_bytes = (int)h_res * bpp;  // panel row stride in bytes

    for (int sy = 0; sy < copy_h; sy++) {
        uint8_t *src_row = out_buf + sy * src_stride * bpp;
        int buf_x = (int)h_res - 1 - (offset_y + sy);
        for (int sx = 0; sx < copy_w; sx++) {
            int buf_y = offset_x + sx;
            uint8_t *d = dst + buf_y * fb_row_bytes + buf_x * bpp;
            uint8_t *s = src_row + sx * bpp;
            for (int b = 0; b < bpp; b++) d[b] = s[b];
        }
    }
    free(out_buf);

    // Blit and hold for minimum duration.
    bsp_display_blit(0, 0, h_res, v_res, fb_pixels);
    vTaskDelay(pdMS_TO_TICKS(SPLASH_DURATION_MS));
}
