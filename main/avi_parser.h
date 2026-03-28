// AVI Parser - RIFF/AVI container parsing for MJPEG video (file streaming)
// Extended with idx1 index support for seeking
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include "esp_err.h"

// Video codec type (detected from AVI strf chunk)
typedef enum {
    AVI_CODEC_UNKNOWN,
    AVI_CODEC_MJPEG,    // MJPG / mjpg
    AVI_CODEC_H264,     // H264 / h264 / X264 / avc1
} avi_codec_t;

// AVI stream info
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t video_frames;
    uint32_t audio_sample_rate;
    uint16_t audio_channels;
    uint16_t audio_bits_per_sample;
    bool has_video;
    bool has_audio;
    avi_codec_t video_codec;
} avi_info_t;

// AVI chunk types
typedef enum {
    AVI_CHUNK_VIDEO,    // 00dc - compressed video
    AVI_CHUNK_AUDIO,    // 01wb - audio data
    AVI_CHUNK_OTHER,    // Other chunks (ignored)
    AVI_CHUNK_END,      // End of movi list
} avi_chunk_type_t;

// AVI chunk descriptor
typedef struct {
    avi_chunk_type_t type;
    uint8_t* data;      // Pointer into frame buffer
    size_t size;        // Chunk data size
} avi_chunk_t;

// Index entry for seeking
typedef struct {
    uint32_t offset;    // Absolute file offset of chunk data (after 8-byte header)
    uint32_t size;      // Chunk data size
} avi_index_entry_t;

// AVI parser state (for streaming from file)
typedef struct {
    FILE* file;             // Open file handle
    char file_path[256];    // Path for reopening after header parse
    size_t file_size;       // Total file size
    size_t movi_start;      // Start of movi list data (file offset)
    size_t movi_end;        // End of movi list
    size_t current_pos;     // Current position in file
    uint8_t* frame_buffer;  // Buffer for reading chunks
    size_t frame_buffer_size;
    avi_info_t info;        // Parsed stream info

    // Index for seeking (allocated in PSRAM)
    avi_index_entry_t* video_index;
    int video_index_count;
    avi_index_entry_t* audio_index;
    int audio_index_count;
    bool has_index;         // True if idx1 was successfully parsed
} avi_parser_t;

// Initialize parser with AVI file path
// Opens file with fastopen for optimal SD card performance
// Parses headers, locates movi list, and parses idx1 index if present
esp_err_t avi_parser_open(avi_parser_t* parser, const char* path);

// Get stream info
const avi_info_t* avi_parser_get_info(const avi_parser_t* parser);

// Get next video chunk from movi list (skips audio chunks)
// Reads chunk into internal buffer
// Returns ESP_OK if chunk found, ESP_ERR_NOT_FOUND at end
esp_err_t avi_parser_next_chunk(avi_parser_t* parser, avi_chunk_t* chunk);

// Seek to a specific video frame by index number
// Repositions file pointer so the next avi_parser_next_chunk call
// returns data starting from (approximately) the target frame.
// Also returns the corresponding audio index for re-syncing audio.
// audio_index_out: set to the audio chunk index nearest the target frame (-1 if none)
// Returns ESP_ERR_NOT_SUPPORTED if no index available
esp_err_t avi_parser_seek_to_frame(avi_parser_t* parser, int frame_number, int* audio_index_out);

// Check if seeking is supported (idx1 index was found)
bool avi_parser_can_seek(const avi_parser_t* parser);

// Get total number of indexed video frames
int avi_parser_get_frame_count(const avi_parser_t* parser);

// Reset parser to beginning of movi list
void avi_parser_rewind(avi_parser_t* parser);

// Close file and free resources
void avi_parser_close(avi_parser_t* parser);

// Get current position estimate (for progress display)
// Returns percentage 0-100
int avi_parser_get_progress(const avi_parser_t* parser);
