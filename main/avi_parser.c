// AVI Parser - RIFF/AVI container parsing for MJPEG video (file streaming)
// Extended with idx1 index support for seeking

#include "avi_parser.h"
#include "fastopen.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <limits.h>
#include <sys/stat.h>

// Large file seek wrapper that handles offsets > 2GB on 32-bit systems
// Uses two-step fseek to avoid long overflow, preserving stdio buffering
static int avi_fseek(FILE* f, size_t offset, int whence) {
    if (whence == SEEK_SET && offset > (size_t)LONG_MAX) {
        // Seek in two steps for offsets > 2GB
        if (fseek(f, LONG_MAX, SEEK_SET) != 0) return -1;
        return fseek(f, (long)(offset - LONG_MAX), SEEK_CUR);
    }
    return fseek(f, (long)offset, whence);
}

// Get file size using stat() to avoid ftell() 2GB limit
static size_t avi_file_size(FILE* f) {
    struct stat st;
    if (fstat(fileno(f), &st) != 0) return 0;
    return (size_t)st.st_size;
}

static const char* TAG = "avi_parser";

// Maximum size for a single video frame (100KB should be plenty for MJPEG)
#define MAX_FRAME_SIZE (100 * 1024)

// Buffer size for reading AVI headers
#define HEADER_BUFFER_SIZE (16 * 1024)

// RIFF/AVI chunk IDs (little-endian FourCC)
#define FOURCC_RIFF 0x46464952  // "RIFF"
#define FOURCC_AVI  0x20495641  // "AVI "
#define FOURCC_LIST 0x5453494C  // "LIST"
#define FOURCC_HDRL 0x6C726468  // "hdrl"
#define FOURCC_MOVI 0x69766F6D  // "movi"
#define FOURCC_AVIH 0x68697661  // "avih"
#define FOURCC_STRH 0x68727473  // "strh"
#define FOURCC_STRF 0x66727473  // "strf"
#define FOURCC_VIDS 0x73646976  // "vids"
#define FOURCC_AUDS 0x73647561  // "auds"
#define FOURCC_IDX1 0x31786469  // "idx1"

// Read a 32-bit little-endian value
static inline uint32_t read_u32_le(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

// Read bytes from file at current position
static bool read_bytes(FILE* f, uint8_t* buf, size_t count) {
    return fread(buf, 1, count, f) == count;
}

// Parse AVI main header (avih chunk)
static void parse_avih(const uint8_t* data, size_t size, avi_info_t* info) {
    if (size < 56) return;

    uint32_t us_per_frame = read_u32_le(data);
    info->fps = us_per_frame > 0 ? 1000000 / us_per_frame : 30;
    info->width = read_u32_le(data + 32);
    info->height = read_u32_le(data + 36);
    info->video_frames = read_u32_le(data + 16);

    ESP_LOGI(TAG, "AVI header: %lux%lu @ %lu fps, %lu frames",
             (unsigned long)info->width, (unsigned long)info->height,
             (unsigned long)info->fps, (unsigned long)info->video_frames);
}

// Parse stream header (strh chunk)
static void parse_strh(const uint8_t* data, size_t size, avi_info_t* info,
                       bool* is_video, bool* is_audio) {
    if (size < 48) return;

    uint32_t fcc_type = read_u32_le(data);

    if (fcc_type == FOURCC_VIDS) {
        *is_video = true;
        info->has_video = true;
        uint32_t scale = read_u32_le(data + 20);
        uint32_t rate = read_u32_le(data + 24);
        if (scale > 0) {
            info->fps = rate / scale;
        }
        ESP_LOGI(TAG, "Video stream: rate=%lu scale=%lu fps=%lu",
                 (unsigned long)rate, (unsigned long)scale, (unsigned long)info->fps);
    } else if (fcc_type == FOURCC_AUDS) {
        *is_audio = true;
        info->has_audio = true;
        ESP_LOGI(TAG, "Audio stream found");
    }
}

// Parse stream format (strf chunk) for video
static void parse_strf_video(const uint8_t* data, size_t size, avi_info_t* info) {
    if (size < 40) return;

    uint32_t width = read_u32_le(data + 4);
    uint32_t height = read_u32_le(data + 8);

    if ((int32_t)height < 0) {
        height = (uint32_t)(-(int32_t)height);
    }

    info->width = width;
    info->height = height;

    char fourcc[5] = {0};
    memcpy(fourcc, data + 16, 4);

    // Detect codec from FourCC
    uint32_t fcc = read_u32_le(data + 16);
    if (fcc == 0x47504A4D || fcc == 0x67706A6D) {  // "MJPG" or "mjpg"
        info->video_codec = AVI_CODEC_MJPEG;
    } else if (fcc == 0x34363248 || fcc == 0x34363268 ||  // "H264" or "h264"
               fcc == 0x34363258 || fcc == 0x34363278 ||  // "X264" or "x264"
               fcc == 0x31637661) {                        // "avc1"
        info->video_codec = AVI_CODEC_H264;
    } else {
        info->video_codec = AVI_CODEC_UNKNOWN;
    }

    ESP_LOGI(TAG, "Video format: %lux%lu codec=%s (type=%d)",
             (unsigned long)width, (unsigned long)height, fourcc, info->video_codec);
}

// Parse header list from buffer
static void parse_hdrl_buffer(const uint8_t* buffer, size_t size, avi_info_t* info) {
    bool current_is_video = false;
    bool current_is_audio = false;
    size_t offset = 0;

    while (offset + 8 <= size) {
        uint32_t chunk_id = read_u32_le(buffer + offset);
        uint32_t chunk_size = read_u32_le(buffer + offset + 4);

        if (chunk_id == FOURCC_AVIH) {
            parse_avih(buffer + offset + 8, chunk_size, info);
        } else if (chunk_id == FOURCC_LIST) {
            offset += 12;
            continue;
        } else if (chunk_id == FOURCC_STRH) {
            parse_strh(buffer + offset + 8, chunk_size, info,
                      &current_is_video, &current_is_audio);
        } else if (chunk_id == FOURCC_STRF) {
            if (current_is_video) {
                parse_strf_video(buffer + offset + 8, chunk_size, info);
            }
            current_is_video = false;
            current_is_audio = false;
        }

        offset += 8 + chunk_size;
        if (chunk_size & 1) offset++;
    }
}

// Parse idx1 index chunk
// Each entry is 16 bytes: [FourCC(4) | flags(4) | offset(4) | size(4)]
static esp_err_t parse_idx1(avi_parser_t* parser, size_t idx1_offset, uint32_t idx1_size) {
    ESP_LOGI(TAG, "Parsing idx1 index: offset=%zu size=%lu", idx1_offset, (unsigned long)idx1_size);

    int entry_count = idx1_size / 16;
    if (entry_count == 0) {
        ESP_LOGW(TAG, "Empty idx1 chunk");
        return ESP_ERR_NOT_FOUND;
    }

    // First pass: count video and audio entries
    int video_count = 0;
    int audio_count = 0;

    #define IDX1_READ_BATCH 256
    uint8_t idx_buf[IDX1_READ_BATCH * 16];

    avi_fseek(parser->file, idx1_offset, SEEK_SET);

    for (int base = 0; base < entry_count; base += IDX1_READ_BATCH) {
        int batch = entry_count - base;
        if (batch > IDX1_READ_BATCH) batch = IDX1_READ_BATCH;

        if (!read_bytes(parser->file, idx_buf, batch * 16)) break;

        for (int i = 0; i < batch; i++) {
            uint32_t fourcc = read_u32_le(idx_buf + i * 16);
            uint8_t type_hi = (fourcc >> 16) & 0xFF;
            uint8_t type_lo = (fourcc >> 24) & 0xFF;

            if (type_hi == 'd' && (type_lo == 'c' || type_lo == 'b')) {
                video_count++;
            } else if (type_hi == 'w' && type_lo == 'b') {
                audio_count++;
            }
        }
    }

    ESP_LOGI(TAG, "Index contains %d video, %d audio entries", video_count, audio_count);

    if (video_count == 0) {
        ESP_LOGW(TAG, "No video entries in index");
        return ESP_ERR_NOT_FOUND;
    }

    // Allocate index arrays in PSRAM
    parser->video_index = heap_caps_malloc(video_count * sizeof(avi_index_entry_t), MALLOC_CAP_SPIRAM);
    if (!parser->video_index) {
        ESP_LOGE(TAG, "Failed to allocate video index (%d entries)", video_count);
        return ESP_ERR_NO_MEM;
    }

    if (audio_count > 0) {
        parser->audio_index = heap_caps_malloc(audio_count * sizeof(avi_index_entry_t), MALLOC_CAP_SPIRAM);
        if (!parser->audio_index) {
            ESP_LOGW(TAG, "Failed to allocate audio index, seeking will skip audio sync");
        }
    }

    // Second pass: populate index arrays
    avi_fseek(parser->file, idx1_offset, SEEK_SET);

    int vi = 0, ai = 0;
    bool offset_mode_detected = false;
    bool offsets_are_relative = true;

    for (int base = 0; base < entry_count; base += IDX1_READ_BATCH) {
        int batch = entry_count - base;
        if (batch > IDX1_READ_BATCH) batch = IDX1_READ_BATCH;

        if (!read_bytes(parser->file, idx_buf, batch * 16)) break;

        for (int i = 0; i < batch; i++) {
            uint32_t fourcc = read_u32_le(idx_buf + i * 16);
            uint32_t offset = read_u32_le(idx_buf + i * 16 + 8);
            uint32_t size   = read_u32_le(idx_buf + i * 16 + 12);

            // Detect offset mode from first entry
            if (!offset_mode_detected) {
                offsets_are_relative = (offset < parser->movi_start);
                offset_mode_detected = true;
                ESP_LOGI(TAG, "Index offset mode: %s (first offset=%lu, movi_start=%zu)",
                         offsets_are_relative ? "relative" : "absolute",
                         (unsigned long)offset, parser->movi_start);
            }

            // Convert to absolute file offset of the chunk header
            // idx1 offsets are relative to the start of the LIST data (including "movi" FourCC)
            // Our movi_start is already past the "movi" FourCC (4 bytes), so subtract 4
            uint32_t abs_offset;
            if (offsets_are_relative) {
                abs_offset = parser->movi_start + offset - 4;
            } else {
                abs_offset = offset;
            }

            uint8_t type_hi = (fourcc >> 16) & 0xFF;
            uint8_t type_lo = (fourcc >> 24) & 0xFF;

            if (type_hi == 'd' && (type_lo == 'c' || type_lo == 'b') && vi < video_count) {
                parser->video_index[vi].offset = abs_offset;
                parser->video_index[vi].size = size;
                vi++;
            } else if (type_hi == 'w' && type_lo == 'b' && parser->audio_index && ai < audio_count) {
                parser->audio_index[ai].offset = abs_offset;
                parser->audio_index[ai].size = size;
                ai++;
            }
        }
    }

    parser->video_index_count = vi;
    parser->audio_index_count = ai;
    parser->has_index = true;

    ESP_LOGI(TAG, "Index parsed: %d video frames, %d audio chunks (%.1f MB PSRAM)",
             vi, ai,
             (vi * sizeof(avi_index_entry_t) + ai * sizeof(avi_index_entry_t)) / (1024.0f * 1024.0f));

    return ESP_OK;
}

// Parse AVI headers and find movi list
static esp_err_t parse_avi_headers(avi_parser_t* parser) {
    uint8_t header[12];

    // Read and verify RIFF header
    avi_fseek(parser->file, 0, SEEK_SET);
    if (!read_bytes(parser->file, header, 12)) {
        ESP_LOGE(TAG, "Failed to read RIFF header");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t riff_id = read_u32_le(header);
    uint32_t riff_size = read_u32_le(header + 4);
    uint32_t avi_id = read_u32_le(header + 8);

    if (riff_id != FOURCC_RIFF || avi_id != FOURCC_AVI) {
        ESP_LOGE(TAG, "Not a valid AVI file");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "AVI file: %zu bytes (RIFF size: %lu)",
             parser->file_size, (unsigned long)riff_size);

    // Scan for hdrl, movi lists, and idx1
    size_t offset = 12;
    uint8_t chunk_header[12];
    bool found_movi = false;

    while (offset + 12 <= parser->file_size) {
        avi_fseek(parser->file, offset, SEEK_SET);
        if (!read_bytes(parser->file, chunk_header, 12)) {
            break;
        }

        uint32_t chunk_id = read_u32_le(chunk_header);
        uint32_t chunk_size = read_u32_le(chunk_header + 4);

        if (chunk_id == FOURCC_LIST) {
            uint32_t list_type = read_u32_le(chunk_header + 8);

            if (list_type == FOURCC_HDRL) {
                size_t hdrl_size = chunk_size - 4;
                if (hdrl_size > HEADER_BUFFER_SIZE) {
                    hdrl_size = HEADER_BUFFER_SIZE;
                }

                uint8_t* hdrl_buffer = malloc(hdrl_size);
                if (hdrl_buffer) {
                    avi_fseek(parser->file, offset + 12, SEEK_SET);
                    if (read_bytes(parser->file, hdrl_buffer, hdrl_size)) {
                        parse_hdrl_buffer(hdrl_buffer, hdrl_size, &parser->info);
                    }
                    free(hdrl_buffer);
                }
            } else if (list_type == FOURCC_MOVI) {
                parser->movi_start = offset + 12;
                parser->movi_end = offset + 8 + chunk_size;
                parser->current_pos = parser->movi_start;
                found_movi = true;

                ESP_LOGI(TAG, "Found movi list: offset=%zu size=%zu",
                         parser->movi_start, parser->movi_end - parser->movi_start);
            }
        } else if (chunk_id == FOURCC_IDX1 && found_movi) {
            parse_idx1(parser, offset + 8, chunk_size);
        }

        offset += 8 + chunk_size;
        if (chunk_size & 1) offset++;
    }

    if (!found_movi) {
        ESP_LOGE(TAG, "movi list not found");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "AVI parsed: %lux%lu @ %lu fps, video=%d audio=%d index=%d",
             (unsigned long)parser->info.width,
             (unsigned long)parser->info.height,
             (unsigned long)parser->info.fps,
             parser->info.has_video, parser->info.has_audio,
             parser->has_index);

    return ESP_OK;
}

esp_err_t avi_parser_open(avi_parser_t* parser, const char* path) {
    if (!parser || !path) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(parser, 0, sizeof(avi_parser_t));
    snprintf(parser->file_path, sizeof(parser->file_path), "%s", path);

    parser->file = fastopen(path, "rb");
    if (!parser->file) {
        ESP_LOGE(TAG, "Failed to open AVI file: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    parser->file_size = avi_file_size(parser->file);

    parser->frame_buffer_size = MAX_FRAME_SIZE;
    parser->frame_buffer = heap_caps_malloc(parser->frame_buffer_size, MALLOC_CAP_SPIRAM);
    if (!parser->frame_buffer) {
        ESP_LOGE(TAG, "Failed to allocate frame buffer (%zu bytes)", parser->frame_buffer_size);
        fastclose(parser->file);
        parser->file = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = parse_avi_headers(parser);
    if (ret != ESP_OK) {
        heap_caps_free(parser->frame_buffer);
        parser->frame_buffer = NULL;
        fastclose(parser->file);
        parser->file = NULL;
        return ret;
    }

    // After header/idx1 parsing the stdio buffer may have read far into the file.
    // Reopen with a fresh fastopen handle to guarantee clean buffer state for
    // sequential playback starting at movi_start.
    fastclose(parser->file);
    parser->file = fastopen(parser->file_path, "rb");
    if (!parser->file) {
        ESP_LOGE(TAG, "Failed to reopen AVI file");
        heap_caps_free(parser->frame_buffer);
        parser->frame_buffer = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    avi_fseek(parser->file, parser->movi_start, SEEK_SET);

    return ESP_OK;
}

const avi_info_t* avi_parser_get_info(const avi_parser_t* parser) {
    return &parser->info;
}

esp_err_t avi_parser_next_chunk(avi_parser_t* parser, avi_chunk_t* chunk) {
    if (!parser || !chunk || !parser->file) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t chunk_header[8];

    while (parser->current_pos + 8 <= parser->movi_end) {
        avi_fseek(parser->file, parser->current_pos, SEEK_SET);
        if (!read_bytes(parser->file, chunk_header, 8)) {
            ESP_LOGE(TAG, "Failed to read chunk header at %zu", parser->current_pos);
            break;
        }

        uint32_t chunk_id = read_u32_le(chunk_header);
        uint32_t chunk_size = read_u32_le(chunk_header + 4);

        if (chunk_size > parser->frame_buffer_size) {
            ESP_LOGW(TAG, "Chunk too large: %lu bytes (max %zu), skipping",
                     (unsigned long)chunk_size, parser->frame_buffer_size);
            parser->current_pos += 8 + chunk_size;
            if (chunk_size & 1) parser->current_pos++;
            continue;
        }

        uint8_t type_hi = (chunk_id >> 16) & 0xFF;
        uint8_t type_lo = (chunk_id >> 24) & 0xFF;

        if (type_hi == 'd' && (type_lo == 'c' || type_lo == 'b')) {
            if (!read_bytes(parser->file, parser->frame_buffer, chunk_size)) {
                ESP_LOGE(TAG, "Failed to read video chunk data");
                break;
            }

            chunk->type = AVI_CHUNK_VIDEO;
            chunk->data = parser->frame_buffer;
            chunk->size = chunk_size;

            parser->current_pos += 8 + chunk_size;
            if (chunk_size & 1) parser->current_pos++;

            return ESP_OK;
        }

        if (type_hi == 'w' && type_lo == 'b') {
            if (!read_bytes(parser->file, parser->frame_buffer, chunk_size)) {
                ESP_LOGE(TAG, "Failed to read audio chunk data");
                break;
            }

            chunk->type = AVI_CHUNK_AUDIO;
            chunk->data = parser->frame_buffer;
            chunk->size = chunk_size;

            parser->current_pos += 8 + chunk_size;
            if (chunk_size & 1) parser->current_pos++;

            return ESP_OK;
        }

        parser->current_pos += 8 + chunk_size;
        if (chunk_size & 1) parser->current_pos++;
    }

    chunk->type = AVI_CHUNK_END;
    chunk->data = NULL;
    chunk->size = 0;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t avi_parser_seek_to_frame(avi_parser_t* parser, int frame_number, int* audio_index_out) {
    if (!parser || !parser->has_index) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (frame_number < 0) frame_number = 0;
    if (frame_number >= parser->video_index_count) {
        frame_number = parser->video_index_count - 1;
    }

    // Seek to the chunk header
    // Flush stdio buffer before large seek to ensure DMA buffer consistency
    fflush(parser->file);
    parser->current_pos = parser->video_index[frame_number].offset;

    if (audio_index_out) {
        *audio_index_out = -1;
        if (parser->audio_index && parser->audio_index_count > 0) {
            uint32_t target_offset = parser->video_index[frame_number].offset;
            int best = 0;
            int lo = 0, hi = parser->audio_index_count - 1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                if (parser->audio_index[mid].offset <= target_offset) {
                    best = mid;
                    lo = mid + 1;
                } else {
                    hi = mid - 1;
                }
            }
            *audio_index_out = best;
        }
    }

    ESP_LOGI(TAG, "Seeked to frame %d (offset=%lu)",
             frame_number, (unsigned long)parser->video_index[frame_number].offset);

    return ESP_OK;
}

bool avi_parser_can_seek(const avi_parser_t* parser) {
    return parser && parser->has_index && parser->video_index_count > 0;
}

int avi_parser_get_frame_count(const avi_parser_t* parser) {
    if (!parser) return 0;
    return parser->video_index_count;
}

void avi_parser_rewind(avi_parser_t* parser) {
    if (parser) {
        parser->current_pos = parser->movi_start;
    }
}

void avi_parser_close(avi_parser_t* parser) {
    if (!parser) return;

    if (parser->frame_buffer) {
        heap_caps_free(parser->frame_buffer);
        parser->frame_buffer = NULL;
    }

    if (parser->video_index) {
        heap_caps_free(parser->video_index);
        parser->video_index = NULL;
    }

    if (parser->audio_index) {
        heap_caps_free(parser->audio_index);
        parser->audio_index = NULL;
    }

    if (parser->file) {
        fastclose(parser->file);
        parser->file = NULL;
    }

    parser->file_size = 0;
    parser->movi_start = 0;
    parser->movi_end = 0;
    parser->current_pos = 0;
    parser->video_index_count = 0;
    parser->audio_index_count = 0;
    parser->has_index = false;
}

int avi_parser_get_progress(const avi_parser_t* parser) {
    if (!parser || parser->movi_end <= parser->movi_start) {
        return 0;
    }

    size_t total = parser->movi_end - parser->movi_start;
    size_t current = parser->current_pos - parser->movi_start;

    return (int)((current * 100) / total);
}
