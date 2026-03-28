// Video Decoder - H.264 software decoding using esp_h264 component
// Supports arbitrary resolutions (buffer sized at init time)

#include "video_decoder.h"
#include "esp_h264_dec.h"
#include "esp_h264_dec_sw.h"
#include "esp_h264_dec_param.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>

static const char* TAG = "video_decoder";

static esp_h264_dec_handle_t decoder = NULL;
static esp_h264_dec_param_handle_t param_handle = NULL;
static uint8_t* yuv_buffer = NULL;
static size_t yuv_buffer_size = 0;
static int configured_width = 0;
static int configured_height = 0;

esp_err_t video_decoder_init(int max_width, int max_height) {
    ESP_LOGI(TAG, "Initializing H.264 software decoder for %dx%d", max_width, max_height);

    configured_width = max_width;
    configured_height = max_height;

    // H.264 uses 16x16 macroblocks, decoder outputs padded dimensions
    int aligned_w = (max_width + 15) & ~15;
    int aligned_h = (max_height + 15) & ~15;

    // YUV420: Y = w*h, U = w*h/4, V = w*h/4 = w*h*3/2
    yuv_buffer_size = aligned_w * aligned_h * 3 / 2;

    ESP_LOGI(TAG, "Aligned: %dx%d, YUV buffer: %zu bytes", aligned_w, aligned_h, yuv_buffer_size);

    // Allocate in PSRAM (too large for internal SRAM at display resolutions)
    yuv_buffer = heap_caps_malloc(yuv_buffer_size, MALLOC_CAP_SPIRAM);
    if (!yuv_buffer) {
        ESP_LOGE(TAG, "Failed to allocate YUV buffer (%zu bytes)", yuv_buffer_size);
        return ESP_ERR_NO_MEM;
    }
    memset(yuv_buffer, 0, yuv_buffer_size);

    // Create software decoder
    esp_h264_dec_cfg_t cfg = {
        .pic_type = ESP_H264_RAW_FMT_I420,
    };

    esp_h264_err_t ret = esp_h264_dec_sw_new(&cfg, &decoder);
    if (ret != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "Failed to create H.264 decoder: %d", ret);
        heap_caps_free(yuv_buffer);
        yuv_buffer = NULL;
        return ESP_FAIL;
    }

    ret = esp_h264_dec_sw_get_param_hd(decoder, &param_handle);
    if (ret != ESP_H264_ERR_OK) {
        ESP_LOGW(TAG, "Failed to get param handle: %d", ret);
        param_handle = NULL;
    }

    ret = esp_h264_dec_open(decoder);
    if (ret != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open H.264 decoder: %d", ret);
        esp_h264_dec_del(decoder);
        decoder = NULL;
        heap_caps_free(yuv_buffer);
        yuv_buffer = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "H.264 decoder initialized (%zu bytes YUV buffer)", yuv_buffer_size);
    return ESP_OK;
}

esp_err_t video_decoder_decode(uint8_t* nal_data, size_t nal_size,
                               uint8_t** yuv_out, int* width, int* height) {
    if (!decoder || !nal_data || nal_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_h264_dec_in_frame_t in_frame = {
        .pts = 0,
        .raw_data = {
            .buffer = nal_data,
            .len = nal_size,
        },
        .consume = 0,
    };

    esp_h264_dec_out_frame_t out_frame = {0};

    esp_h264_err_t ret = esp_h264_dec_process(decoder, &in_frame, &out_frame);

    if (ret == ESP_H264_ERR_OK && out_frame.outbuf && out_frame.out_size > 0) {
        size_t copy_size = out_frame.out_size;
        if (copy_size > yuv_buffer_size) {
            ESP_LOGW(TAG, "Decoded frame larger than buffer: %zu > %zu", copy_size, yuv_buffer_size);
            copy_size = yuv_buffer_size;
        }
        memcpy(yuv_buffer, out_frame.outbuf, copy_size);

        // Get actual resolution from decoder
        if (param_handle) {
            esp_h264_resolution_t res = {0};
            if (esp_h264_dec_get_resolution(param_handle, &res) == ESP_H264_ERR_OK) {
                *width = res.width;
                *height = res.height;
            } else {
                *width = configured_width;
                *height = configured_height;
            }
        } else {
            *width = configured_width;
            *height = configured_height;
        }

        static bool first_frame = true;
        if (first_frame) {
            ESP_LOGI(TAG, "First H.264 frame: %dx%d, %zu bytes", *width, *height, copy_size);
            first_frame = false;
        }

        *yuv_out = yuv_buffer;
        return ESP_OK;
    } else if (ret == ESP_H264_ERR_OK) {
        return ESP_ERR_NOT_FINISHED;
    } else {
        ESP_LOGW(TAG, "H.264 decode error: %d", ret);
        return ESP_FAIL;
    }
}

void video_decoder_deinit(void) {
    if (decoder) {
        esp_h264_dec_close(decoder);
        esp_h264_dec_del(decoder);
        decoder = NULL;
        param_handle = NULL;
    }

    if (yuv_buffer) {
        heap_caps_free(yuv_buffer);
        yuv_buffer = NULL;
    }

    yuv_buffer_size = 0;
    configured_width = 0;
    configured_height = 0;

    ESP_LOGI(TAG, "H.264 decoder deinitialized");
}
