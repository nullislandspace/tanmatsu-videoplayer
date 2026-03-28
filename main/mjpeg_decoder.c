// MJPEG Decoder - Hardware JPEG decoding using ESP32-P4 JPEG peripheral

#include "mjpeg_decoder.h"
#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>

static const char* TAG = "mjpeg_decoder";

static jpeg_decoder_handle_t decoder = NULL;
static uint8_t* output_buffer = NULL;
static size_t output_buffer_size = 0;
static int max_frame_width = 0;
static int max_frame_height = 0;
static int aligned_frame_width = 0;  // 16-pixel aligned width for hardware decoder

esp_err_t mjpeg_decoder_init(int max_width, int max_height) {
    ESP_LOGI(TAG, "Initializing hardware JPEG decoder for %dx%d", max_width, max_height);

    max_frame_width = max_width;
    max_frame_height = max_height;

    // Calculate buffer size for BGR888 output
    // Hardware decoder aligns output to 16-pixel blocks, so round up dimensions
    aligned_frame_width = (max_width + 15) & ~15;
    int aligned_height = (max_height + 15) & ~15;
    output_buffer_size = aligned_frame_width * aligned_height * 3;
    ESP_LOGI(TAG, "Aligned dimensions: %dx%d -> %dx%d (%zu bytes)",
             max_width, max_height, aligned_frame_width, aligned_height, output_buffer_size);

    // Try to allocate in internal SRAM first for maximum performance
    // DMA-capable, 64-byte aligned for best hardware compatibility
    output_buffer = heap_caps_aligned_alloc(64, output_buffer_size,
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (output_buffer) {
        ESP_LOGI(TAG, "Output buffer: %zu bytes in internal SRAM (fast)", output_buffer_size);
    } else {
        // Try JPEG-aligned allocation (may use internal or external)
        jpeg_decode_memory_alloc_cfg_t mem_cfg = {
            .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
        };
        size_t actual_size = 0;
        output_buffer = (uint8_t*)jpeg_alloc_decoder_mem(output_buffer_size, &mem_cfg, &actual_size);
        if (output_buffer) {
            ESP_LOGI(TAG, "Output buffer: %zu bytes via JPEG allocator", output_buffer_size);
        } else {
            // Final fallback to PSRAM
            output_buffer = heap_caps_aligned_alloc(64, output_buffer_size, MALLOC_CAP_SPIRAM);
            if (!output_buffer) {
                ESP_LOGE(TAG, "Failed to allocate output buffer (%zu bytes)", output_buffer_size);
                return ESP_ERR_NO_MEM;
            }
            ESP_LOGI(TAG, "Output buffer: %zu bytes in PSRAM (slower)", output_buffer_size);
        }
    }

    // Create the hardware JPEG decoder engine
    jpeg_decode_engine_cfg_t engine_cfg = {
        .intr_priority = 0,
        .timeout_ms = 100,
    };

    esp_err_t ret = jpeg_new_decoder_engine(&engine_cfg, &decoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create JPEG decoder engine: %s", esp_err_to_name(ret));
        heap_caps_free(output_buffer);
        output_buffer = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "Hardware JPEG decoder initialized (buffer: %zu bytes)", output_buffer_size);
    return ESP_OK;
}

esp_err_t mjpeg_decoder_decode(uint8_t* jpeg_data, size_t jpeg_size,
                                uint8_t** bgr_out, int* width, int* height) {
    if (!decoder || !jpeg_data || jpeg_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Configure decode parameters
    // Output BGR888 format to match display (display uses BGR byte order)
    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };

    // Prepare decode picture description
    jpeg_decode_picture_info_t pic_info = {0};

    // First, get the picture info (dimensions) from the JPEG header
    esp_err_t ret = jpeg_decoder_get_info(jpeg_data, jpeg_size, &pic_info);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get JPEG info: %s", esp_err_to_name(ret));
        return ret;
    }

    // Check if frame fits in buffer
    size_t needed_size = pic_info.width * pic_info.height * 3;
    if (needed_size > output_buffer_size) {
        ESP_LOGE(TAG, "Frame too large: %dx%d (%zu bytes) > buffer (%zu bytes)",
                 pic_info.width, pic_info.height, needed_size, output_buffer_size);
        return ESP_ERR_NO_MEM;
    }

    // Decode the JPEG frame
    uint32_t decoded_size = 0;
    ret = jpeg_decoder_process(decoder, &decode_cfg, jpeg_data, jpeg_size,
                               output_buffer, output_buffer_size, &decoded_size);

    if (ret != ESP_OK) {
        // Log error only occasionally to avoid flooding
        static int error_count = 0;
        if (error_count++ < 10) {
            ESP_LOGW(TAG, "JPEG decode failed: %s (frame %d)", esp_err_to_name(ret), error_count);
        }
        return ret;
    }

    // Log first successful decode
    static bool first_decode_logged = false;
    if (!first_decode_logged) {
        ESP_LOGI(TAG, "First frame decoded: %dx%d, %lu bytes",
                 pic_info.width, pic_info.height, (unsigned long)decoded_size);
        first_decode_logged = true;
    }

    *width = pic_info.width;
    *height = pic_info.height;
    *bgr_out = output_buffer;

    return ESP_OK;
}

void mjpeg_decoder_deinit(void) {
    if (decoder) {
        jpeg_del_decoder_engine(decoder);
        decoder = NULL;
    }

    if (output_buffer) {
        heap_caps_free(output_buffer);
        output_buffer = NULL;
    }

    output_buffer_size = 0;
    max_frame_width = 0;
    max_frame_height = 0;
    aligned_frame_width = 0;

    ESP_LOGI(TAG, "Hardware JPEG decoder deinitialized");
}

uint8_t* mjpeg_decoder_get_output_buffer(void) {
    return output_buffer;
}

size_t mjpeg_decoder_get_output_buffer_size(void) {
    return output_buffer_size;
}

// Copy BGR888 to framebuffer with 270-degree rotation and dual-axis letterboxing
// The display is rotated 270 degrees from the framebuffer orientation
// For 270° rotation: src(x,y) -> dst(height-1-y, x)
// Centers video in both X and Y when video is smaller than display
esp_err_t mjpeg_copy_to_framebuffer(uint8_t* bgr_in, uint8_t* fb_out,
                                     int src_width, int src_height,
                                     int display_width, int display_height) {
    if (!bgr_in || !fb_out) {
        return ESP_ERR_INVALID_ARG;
    }

    // Clamp source to display bounds (safety for oversized videos)
    int copy_width = src_width;
    int copy_height = src_height;
    if (copy_width > display_width) copy_width = display_width;
    if (copy_height > display_height) copy_height = display_height;

    // Calculate offsets to center video on screen
    int offset_x = (display_width - copy_width) / 2;   // Horizontal (pillarbox)
    int offset_y = (display_height - copy_height) / 2;  // Vertical (letterbox)
    if (offset_x < 0) offset_x = 0;
    if (offset_y < 0) offset_y = 0;

    // Source stride uses aligned width (hardware decoder pads to 16-pixel blocks)
    int src_stride = aligned_frame_width * 3;

    // The framebuffer is display_height rows x display_width columns (before rotation)
    // After 270° rotation, screen X maps to buffer Y, screen Y maps to buffer X (inverted)
    // Buffer stride = display_h_res pixels wide (the physical buffer width)
    int fb_stride_bytes = display_height * 3;  // Buffer row stride in bytes after rotation

    for (int src_y = 0; src_y < copy_height; src_y++) {
        uint8_t* src_row = bgr_in + src_y * src_stride;

        // Screen position of this source row (with centering offset)
        // Screen Y position: offset_y + src_y
        // After 270° rotation: screen_y maps to buf_x = (display_height - 1 - screen_y)
        int buf_x = display_height - 1 - (offset_y + src_y);

        for (int src_x = 0; src_x < copy_width; src_x++) {
            // Screen X position: offset_x + src_x
            // After 270° rotation: screen_x maps to buf_y
            int buf_y = offset_x + src_x;

            uint8_t* dst = fb_out + buf_y * fb_stride_bytes + buf_x * 3;

            dst[0] = src_row[src_x * 3 + 0];  // B
            dst[1] = src_row[src_x * 3 + 1];  // G
            dst[2] = src_row[src_x * 3 + 2];  // R
        }
    }

    return ESP_OK;
}
