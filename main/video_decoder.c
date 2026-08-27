// Video Decoder - H.264 software decoding using open-source h264bsd
// Outputs I420 planar YUV (to be reformatted for PPA hardware)

#include "video_decoder.h"
#include "h264bsd_decoder.h"
#include "h264bsd_util.h"
#include "h264bsd_storage.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>

static const char* TAG = "video_decoder";

static storage_t* decoder = NULL;
static int configured_width = 0;
static int configured_height = 0;
static uint32_t pic_id = 0;

esp_err_t video_decoder_init(int max_width, int max_height) {
    ESP_LOGI(TAG, "Initializing h264bsd decoder for %dx%d", max_width, max_height);

    configured_width = max_width;
    configured_height = max_height;

    decoder = h264bsdAlloc();
    if (!decoder) {
        ESP_LOGE(TAG, "Failed to allocate decoder");
        return ESP_ERR_NO_MEM;
    }

    u32 ret = h264bsdInit(decoder, 1);  // noOutputReordering=1 for low latency
    if (ret != HANTRO_OK) {
        ESP_LOGE(TAG, "Failed to init decoder: %u", ret);
        h264bsdFree(decoder);
        decoder = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "h264bsd decoder initialized");
    return ESP_OK;
}

esp_err_t video_decoder_decode(uint8_t* nal_data, size_t nal_size,
                               uint8_t** yuv_out, int* width, int* height,
                               uint8_t** ppa_out, uint32_t* ppa_stride) {
    if (!decoder || !nal_data || nal_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *yuv_out = NULL;
    if (ppa_out) *ppa_out = NULL;
    u32 readBytes = 0;
    u8* remaining = nal_data;
    u32 remaining_len = nal_size;

    // Feed all NAL units in this chunk to the decoder
    while (remaining_len > 0) {
        u32 ret = h264bsdDecode(decoder, remaining, remaining_len, pic_id, &readBytes);

        if (readBytes == 0 && ret != H264BSD_PIC_RDY && ret != H264BSD_HDRS_RDY) {
            break;  // No progress, avoid infinite loop
        }

        remaining += readBytes;
        remaining_len -= readBytes;

        switch (ret) {
            case H264BSD_PIC_RDY: {
                // Picture decoded — get the output
                u32 outPicId = 0, isIdr = 0, numErrMbs = 0;
                u8* pic = h264bsdNextOutputPicture(decoder, &outPicId, &isIdr, &numErrMbs);
                if (pic) {
                    *width = h264bsdPicWidth(decoder) * 16;
                    *height = h264bsdPicHeight(decoder) * 16;
                    *yuv_out = pic;
                    if (ppa_out && ppa_stride) {
                        u32 stride = 0;
                        *ppa_out = h264bsdNextOutputPicturePpa(decoder, &stride);
                        *ppa_stride = stride;
                    }
                    pic_id++;

                    static bool first_frame = true;
                    if (first_frame) {
                        ESP_LOGI(TAG, "First h264bsd frame: %dx%d", *width, *height);
                        first_frame = false;
                    }

                    return ESP_OK;
                }
                break;
            }
            case H264BSD_HDRS_RDY: {
                // SPS/PPS decoded, resolution may be available now
                u32 w = h264bsdPicWidth(decoder);
                u32 h = h264bsdPicHeight(decoder);
                if (w && h) {
                    ESP_LOGI(TAG, "Stream headers: %ux%u", w * 16, h * 16);
                }
                // Continue decoding — there may be more data in this chunk
                break;
            }
            case H264BSD_RDY:
                // NAL unit processed but no picture yet, continue
                break;
            case H264BSD_ERROR:
                ESP_LOGW(TAG, "h264bsd decode error");
                return ESP_FAIL;
            case H264BSD_PARAM_SET_ERROR:
                ESP_LOGE(TAG, "h264bsd param set error");
                return ESP_FAIL;
            case H264BSD_MEMALLOC_ERROR:
                ESP_LOGE(TAG, "h264bsd memory allocation error");
                return ESP_ERR_NO_MEM;
            default:
                break;
        }
    }

    // No picture produced yet (decoder is buffering)
    return ESP_ERR_NOT_FINISHED;
}

bool video_decoder_get_visible_size(int* width, int* height) {
    if (!decoder || !width || !height) return false;

    u32 coded_w = h264bsdPicWidth(decoder) * 16;
    u32 coded_h = h264bsdPicHeight(decoder) * 16;
    if (coded_w == 0 || coded_h == 0) return false;

    u32 flag = 0, left = 0, top = 0, crop_w = 0, crop_h = 0;
    h264bsdCroppingParams(decoder, &flag, &left, &crop_w, &top, &crop_h);

    // Only a top/left-anchored window is useful here: the PPA consumes
    // a block from the start of the buffer and we have nowhere to put a
    // left/top offset without re-basing the pointer, which would break
    // the row stride. Every encoder that merely pads to a macroblock
    // boundary -- the P4's included -- crops from the right and bottom
    // only, so this is the case that matters.
    if (flag && left == 0 && top == 0 &&
        crop_w > 0 && crop_h > 0 && crop_w <= coded_w && crop_h <= coded_h) {
        *width  = (int)crop_w;
        *height = (int)crop_h;
    } else {
        *width  = (int)coded_w;
        *height = (int)coded_h;
    }
    return true;
}

void video_decoder_deinit(void) {
    if (decoder) {
        h264bsdShutdown(decoder);
        h264bsdFree(decoder);
        decoder = NULL;
    }

    configured_width = 0;
    configured_height = 0;
    pic_id = 0;

    ESP_LOGI(TAG, "h264bsd decoder deinitialized");
}
