// Audio Player - MP3 decode and I2S output (chunk-based streaming)
// Runs on Core 1 for parallel audio/video processing

#include "audio_player.h"
#include "simd_memcpy.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "bsp/audio.h"
#include "bsp/input.h"
#include "driver/i2s_std.h"
#include "esp_mp3_dec.h"
#include <string.h>

static const char* TAG = "audio_player";

// External declaration for bsp_audio_initialize
extern esp_err_t bsp_audio_initialize(uint32_t rate);

// Audio task configuration
#define AUDIO_TASK_STACK_SIZE   (16 * 1024)
#define AUDIO_TASK_PRIORITY     6
#define AUDIO_TASK_CORE         1

// Sample rate (will be updated from decoder)
#define AUDIO_SAMPLE_RATE       44100

// Buffer sizes
#define PCM_BUFFER_SAMPLES      1152  // MP3 frame size
#define PCM_BUFFER_SIZE         (PCM_BUFFER_SAMPLES * 2 * sizeof(int16_t))  // Stereo 16-bit

// Audio chunk for queue
#define AUDIO_CHUNK_MAX_SIZE    4096  // Max size of a single audio chunk
#define AUDIO_QUEUE_LENGTH      16    // Number of chunks to buffer (more headroom for high FPS)

typedef struct __attribute__((aligned(16))) {
    uint8_t data[AUDIO_CHUNK_MAX_SIZE];
    size_t size;
} audio_chunk_t;

// Audio state
static i2s_chan_handle_t i2s_tx_handle = NULL;
static TaskHandle_t audio_task_handle = NULL;
static QueueHandle_t audio_queue = NULL;
static volatile bool audio_playing = false;
static volatile bool audio_stop_requested = false;
static volatile bool audio_paused = false;
static volatile bool stream_ended = false;
static volatile uint64_t samples_written = 0;
static SemaphoreHandle_t audio_mutex = NULL;

// Timing for A/V sync
static int64_t audio_start_time_us = 0;
static volatile uint32_t actual_sample_rate = AUDIO_SAMPLE_RATE;

// Stream format, discovered from the first decoded MP3 frame. Reset on each
// start so a mono/22k clip doesn't keep the I2S config from a prior stereo/44k
// clip.
static volatile int  audio_channels    = 2;
static volatile bool audio_format_seen = false;

// PCM buffer in internal SRAM for DMA
static DRAM_ATTR int16_t pcm_buffer[PCM_BUFFER_SAMPLES * 2] __attribute__((aligned(16)));

// Headphone state tracking. On newer coprocessor firmware the amp is
// automatically disabled when headphones are plugged in; on older firmware
// we must do this manually. Tracking the state here and gating the
// amplifier through amp_set() keeps the behavior correct on both.
static bool headphones_connected = false;
static bool amp_wanted_on = false;

static void amp_set(bool want_on) {
    amp_wanted_on = want_on;
    // Only drive the built-in speaker amp when no headphones are plugged in.
    bsp_audio_set_amplifier(want_on && !headphones_connected);
}

// Forward declarations
static void audio_task(void* arg);

esp_err_t audio_player_init(void) {
    // Create mutex for thread safety
    audio_mutex = xSemaphoreCreateMutex();
    if (!audio_mutex) {
        ESP_LOGE(TAG, "Failed to create audio mutex");
        return ESP_ERR_NO_MEM;
    }

    // Try to get existing I2S handle first
    esp_err_t ret = bsp_audio_get_i2s_handle(&i2s_tx_handle);
    if (ret != ESP_OK || !i2s_tx_handle) {
        ESP_LOGI(TAG, "Initializing BSP audio at 44.1kHz");
        ret = bsp_audio_initialize(44100);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize BSP audio: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = bsp_audio_get_i2s_handle(&i2s_tx_handle);
        if (ret != ESP_OK || !i2s_tx_handle) {
            ESP_LOGE(TAG, "Failed to get I2S handle: %s", esp_err_to_name(ret));
            return ret;
        }
    } else {
        ESP_LOGI(TAG, "Using existing BSP audio I2S handle");
    }

    // Seed headphone state from the BSP so the very first amp_set() call
    // below already knows whether headphones are plugged in.
    bool hp_state = false;
    if (bsp_input_read_action(BSP_INPUT_ACTION_TYPE_AUDIO_JACK, &hp_state) == ESP_OK) {
        headphones_connected = hp_state;
    }

    amp_set(true);
    bsp_audio_set_volume(100.0f);

    ESP_LOGI(TAG, "Audio player initialized (headphones %s)",
             headphones_connected ? "connected" : "disconnected");
    return ESP_OK;
}

esp_err_t audio_player_start(void) {
    if (audio_playing) {
        ESP_LOGW(TAG, "Audio already playing, stopping first");
        audio_player_stop();
    }

    // Create audio queue
    audio_queue = xQueueCreate(AUDIO_QUEUE_LENGTH, sizeof(audio_chunk_t));
    if (!audio_queue) {
        ESP_LOGE(TAG, "Failed to create audio queue");
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(audio_mutex, portMAX_DELAY);

    samples_written = 0;
    audio_stop_requested = false;
    stream_ended = false;
    audio_playing = true;
    audio_format_seen = false;
    audio_channels = 2;
    audio_start_time_us = esp_timer_get_time();

    xSemaphoreGive(audio_mutex);

    amp_set(true);

    // Re-enable the I2S channel (stop() disables it to silence the DAC).
    // Ignore ESP_ERR_INVALID_STATE, which just means it is already enabled
    // on the very first start after init.
    if (i2s_tx_handle) {
        i2s_channel_enable(i2s_tx_handle);
    }

    // Create audio task on Core 1
    BaseType_t result = xTaskCreatePinnedToCore(
        audio_task,
        "audio_player",
        AUDIO_TASK_STACK_SIZE,
        NULL,
        AUDIO_TASK_PRIORITY,
        &audio_task_handle,
        AUDIO_TASK_CORE
    );

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio task");
        vQueueDelete(audio_queue);
        audio_queue = NULL;
        audio_playing = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Audio playback started");
    return ESP_OK;
}

esp_err_t audio_player_push_chunk(const uint8_t* data, size_t size) {
    if (!audio_queue || !audio_playing) {
        return ESP_ERR_INVALID_STATE;
    }

    if (size > AUDIO_CHUNK_MAX_SIZE) {
        ESP_LOGW(TAG, "Audio chunk too large: %zu > %d", size, AUDIO_CHUNK_MAX_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    audio_chunk_t chunk;
    simd_memcpy(chunk.data, data, size);
    chunk.size = size;

    // Try to push with very short timeout (don't block video)
    if (xQueueSend(audio_queue, &chunk, pdMS_TO_TICKS(5)) != pdTRUE) {
        // Queue full - caller should stop pushing and try again later
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

void audio_player_end_stream(void) {
    stream_ended = true;
}

void audio_player_stop(void) {
    if (audio_playing || audio_task_handle) {
        ESP_LOGI(TAG, "Stopping audio playback");

        audio_stop_requested = true;

        // Wait for task to finish
        int timeout_ms = 500;
        while (audio_playing && timeout_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            timeout_ms -= 10;
        }

        if (audio_task_handle && audio_playing) {
            ESP_LOGW(TAG, "Force deleting audio task");
            vTaskDelete(audio_task_handle);
        }

        audio_task_handle = NULL;
        audio_playing = false;
    }

    // Clean up queue
    if (audio_queue) {
        vQueueDelete(audio_queue);
        audio_queue = NULL;
    }

    // Flush I2S with silence, then disable the channel so the DMA stops
    // looping its last buffer (otherwise the DAC/I2S chip keeps replaying
    // the same sample even though the amplifier is off).
    if (i2s_tx_handle) {
        memset(pcm_buffer, 0, PCM_BUFFER_SIZE);
        size_t bytes_written = 0;
        for (int i = 0; i < 4; i++) {
            i2s_channel_write(i2s_tx_handle, pcm_buffer, PCM_BUFFER_SIZE,
                              &bytes_written, pdMS_TO_TICKS(20));
        }
        i2s_channel_disable(i2s_tx_handle);
    }

    amp_set(false);
}

void audio_player_pause(bool pause) {
    audio_paused = pause;
    if (i2s_tx_handle) {
        if (pause) {
            i2s_channel_disable(i2s_tx_handle);
        } else {
            i2s_channel_enable(i2s_tx_handle);
        }
    }
}

bool audio_player_is_playing(void) {
    return audio_playing;
}

uint32_t audio_player_get_position_ms(void) {
    return (uint32_t)((samples_written * 1000ULL) / actual_sample_rate);
}

void audio_player_set_headphones_connected(bool connected) {
    if (headphones_connected == connected) return;
    headphones_connected = connected;
    ESP_LOGI(TAG, "Headphones %s", connected ? "connected" : "disconnected");
    // Re-apply the amp state so the speaker follows the new jack state.
    // On newer coprocessor firmware the coprocessor has already done this
    // itself; the bsp_audio_set_amplifier call is then a harmless no-op
    // since the requested state matches the current state.
    amp_set(amp_wanted_on);
}

void audio_player_set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    bsp_audio_set_volume((float)volume);
}

void audio_player_deinit(void) {
    audio_player_stop();
    amp_set(false);

    if (audio_mutex) {
        vSemaphoreDelete(audio_mutex);
        audio_mutex = NULL;
    }

    ESP_LOGI(TAG, "Audio player deinitialized");
}

// Audio decode and playback task
static void audio_task(void* arg) {
    (void)arg;
    void* mp3_decoder = NULL;
    esp_audio_err_t ret;

    ESP_LOGI(TAG, "Audio task started");

    // Create MP3 decoder
    ret = esp_mp3_dec_open(NULL, 0, &mp3_decoder);
    if (ret != ESP_AUDIO_ERR_OK || !mp3_decoder) {
        ESP_LOGE(TAG, "Failed to create MP3 decoder: %d", ret);
        goto cleanup;
    }

    // Allocate decode output buffer
    uint8_t* frame_buffer = heap_caps_malloc(8192, MALLOC_CAP_DEFAULT);
    if (!frame_buffer) {
        ESP_LOGE(TAG, "Failed to allocate frame buffer");
        goto cleanup;
    }

    // Accumulator for the raw MP3 bitstream. AVI audio chunks are fixed-size
    // byte slabs and are NOT frame-aligned: an MP3 frame may span two or more
    // chunks. Camera recordings (22050 Hz mono, 64 kbps) use ~208-byte chunks
    // with frames that drift across chunk boundaries. We therefore need a
    // continuous stream buffer so the decoder can always see a full frame.
    // 8 KiB easily covers any MP3 frame (max ~1440 B) plus one incoming chunk
    // (max AUDIO_CHUNK_MAX_SIZE = 4096 B).
    #define ACCUM_SIZE 8192
    uint8_t* accum = heap_caps_malloc(ACCUM_SIZE, MALLOC_CAP_DEFAULT);
    if (!accum) {
        ESP_LOGE(TAG, "Failed to allocate MP3 accumulator");
        heap_caps_free(frame_buffer);
        goto cleanup;
    }
    size_t accum_len = 0;

    // Diagnostics
    uint32_t chunks_received = 0;
    uint32_t frames_decoded = 0;
    uint32_t frames_errors = 0;
    uint32_t resync_bytes  = 0;

    audio_chunk_t chunk;

    // Main decode loop
    while (!audio_stop_requested) {
        // While paused, sleep instead of decoding
        if (audio_paused) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Fill the accumulator from the queue. Only pull when there's room
        // for a full incoming chunk. Block on the first pull when the buffer
        // is empty (and the stream hasn't ended) so we don't spin.
        while (accum_len + AUDIO_CHUNK_MAX_SIZE <= ACCUM_SIZE) {
            TickType_t wait_time;
            if (accum_len == 0 && !stream_ended) {
                wait_time = pdMS_TO_TICKS(100);
            } else {
                wait_time = 0;
            }
            if (xQueueReceive(audio_queue, &chunk, wait_time) != pdTRUE) {
                break;
            }
            memcpy(accum + accum_len, chunk.data, chunk.size);
            accum_len += chunk.size;
            chunks_received++;
        }

        if (accum_len == 0) {
            if (stream_ended) {
                ESP_LOGI(TAG, "=== AUDIO END: samples=%llu ===", (unsigned long long)samples_written);
                ESP_LOGI(TAG, "=== AUDIO STATS: chunks=%lu, decoded=%lu, errors=%lu, resync=%lu ===",
                         (unsigned long)chunks_received, (unsigned long)frames_decoded,
                         (unsigned long)frames_errors, (unsigned long)resync_bytes);
                break;
            }
            continue;
        }

        // Decode as much as we can from the accumulator.
        size_t pos = 0;
        bool   made_progress = false;
        while (pos < accum_len && !audio_stop_requested) {
            esp_audio_dec_in_raw_t raw = {
                .buffer = accum + pos,
                .len = accum_len - pos,
                .consumed = 0,
                .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
            };

            esp_audio_dec_out_frame_t frame = {
                .buffer = frame_buffer,
                .len = 8192,
                .needed_size = 0,
                .decoded_size = 0,
            };

            esp_audio_dec_info_t dec_info = {0};
            ret = esp_mp3_dec_decode(mp3_decoder, &raw, &frame, &dec_info);

            if (ret == ESP_AUDIO_ERR_OK && frame.decoded_size > 0) {
                frames_decoded++;

                // Configure I2S from the first decoded frame's format.
                // AVI's auds/strf header isn't parsed, so the decoder is our
                // only source of truth for rate/channel count. Camera clips
                // are 22050 Hz mono, pre-converted clips are 44100 Hz stereo.
                if (!audio_format_seen) {
                    ESP_LOGI(TAG, "Audio format: %d Hz, %d ch, %d bits",
                             dec_info.sample_rate, dec_info.channel, dec_info.bits_per_sample);

                    uint32_t new_rate = dec_info.sample_rate > 0
                                            ? (uint32_t)dec_info.sample_rate
                                            : AUDIO_SAMPLE_RATE;
                    if (new_rate != actual_sample_rate) {
                        // reconfig_std_clock requires the channel disabled.
                        i2s_channel_disable(i2s_tx_handle);
                        esp_err_t rc = bsp_audio_set_rate(new_rate);
                        if (rc != ESP_OK) {
                            ESP_LOGW(TAG, "bsp_audio_set_rate(%lu) failed: %s",
                                     (unsigned long)new_rate, esp_err_to_name(rc));
                        }
                        i2s_channel_enable(i2s_tx_handle);
                        actual_sample_rate = new_rate;
                    }

                    audio_channels = (dec_info.channel == 1) ? 1 : 2;
                    audio_format_seen = true;
                }

                // Copy decoded samples to the aligned DMA buffer with 50%
                // volume. I2S is always stereo 16-bit, so mono sources must
                // be duplicated to L/R — otherwise mono samples are read as
                // L/R pairs and playback runs at double speed.
                int16_t* src = (int16_t*)frame_buffer;
                int16_t* dst = pcm_buffer;
                size_t   bytes_to_write;

                if (audio_channels == 1) {
                    size_t num_in = frame.decoded_size / sizeof(int16_t);
                    size_t max_in = PCM_BUFFER_SIZE / (2 * sizeof(int16_t));
                    if (num_in > max_in) num_in = max_in;
                    for (size_t i = 0; i < num_in; i++) {
                        int16_t s = src[i] >> 1;
                        dst[i * 2]     = s;
                        dst[i * 2 + 1] = s;
                    }
                    bytes_to_write = num_in * 2 * sizeof(int16_t);
                } else {
                    size_t copy_size = frame.decoded_size;
                    if (copy_size > PCM_BUFFER_SIZE) {
                        copy_size = PCM_BUFFER_SIZE;
                    }
                    size_t num_samples = copy_size / sizeof(int16_t);
                    for (size_t i = 0; i < num_samples; i++) {
                        dst[i] = src[i] >> 1;
                    }
                    bytes_to_write = copy_size;
                }

                // Write to I2S
                size_t bytes_written = 0;
                i2s_channel_write(i2s_tx_handle, pcm_buffer, bytes_to_write,
                                  &bytes_written, portMAX_DELAY);

                // Stereo 16-bit frames are 4 bytes.
                samples_written += bytes_written / 4;

                pos += raw.consumed;
                made_progress = true;
            } else if (raw.consumed > 0) {
                // Partial parse — consumed some bytes (e.g. header) without
                // emitting a frame. Advance and try again.
                pos += raw.consumed;
                made_progress = true;
            } else {
                // Decoder couldn't advance: needs more data OR the current
                // byte isn't a valid sync. Stop decoding for this pass; if
                // we have plenty of data buffered the resync scan below will
                // skip junk bytes.
                break;
            }
        }

        // Compact the accumulator.
        if (pos > 0) {
            if (pos < accum_len) {
                memmove(accum, accum + pos, accum_len - pos);
            }
            accum_len -= pos;
        }

        // If we made no progress and the accumulator has more than a frame's
        // worth of data (and more is unlikely to arrive — the producer would
        // have filled us by now), the stream is out of sync. Scan forward
        // for the next MP3 sync word (11 bits: 0xFF followed by a byte with
        // top 3 bits set).
        if (!made_progress && accum_len > 2048) {
            size_t scan = 1;
            while (scan + 1 < accum_len) {
                if (accum[scan] == 0xff && (accum[scan + 1] & 0xe0) == 0xe0) {
                    break;
                }
                scan++;
            }
            frames_errors++;
            resync_bytes += scan;
            if (scan >= accum_len - 1) {
                // No sync found at all — dump everything and hope the next
                // batch starts clean.
                accum_len = 0;
            } else {
                memmove(accum, accum + scan, accum_len - scan);
                accum_len -= scan;
            }
        }

        // Debug log every ~3 seconds
        static uint64_t last_debug_samples = 0;
        if (samples_written - last_debug_samples >= actual_sample_rate * 3) {
            int64_t elapsed_us = esp_timer_get_time() - audio_start_time_us;
            int64_t elapsed_ms = elapsed_us / 1000;
            int64_t audio_pos_ms = (samples_written * 1000) / actual_sample_rate;
            int64_t drift_ms = audio_pos_ms - elapsed_ms;
            ESP_LOGI(TAG, "Audio sync: elapsed=%lldms, audio_pos=%lldms, drift=%+lldms",
                     elapsed_ms, audio_pos_ms, drift_ms);
            last_debug_samples = samples_written;
        }
    }

    heap_caps_free(accum);
    heap_caps_free(frame_buffer);

cleanup:
    if (mp3_decoder) {
        esp_mp3_dec_close(mp3_decoder);
    }

    ESP_LOGI(TAG, "Audio task ending (samples written: %llu)", (unsigned long long)samples_written);

    audio_playing = false;
    audio_task_handle = NULL;
    vTaskDelete(NULL);
}
