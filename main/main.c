// Tanmatsu Video Player
// Plays MJPEG+MP3 AVI files from SD card with file browser, seeking, and headphone detection

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/led.h"
#include "bsp/power.h"
#include "bsp/audio.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/lcd_types.h"
#include "pax_gfx.h"
#include "usb_device.h"
#include "hershey_font.h"
#include "sdcard.h"
#include "file_browser.h"
#include "mjpeg_decoder.h"
#include "avi_parser.h"
#include "audio_player.h"

static const char* TAG = "videoplayer";

// Application states
typedef enum {
    APP_STATE_BROWSER,
    APP_STATE_PLAYING,
    APP_STATE_ERROR,
} app_state_t;

// Display globals
static size_t display_h_res = 0;
static size_t display_v_res = 0;
static lcd_color_rgb_pixel_format_t display_color_format = LCD_COLOR_PIXEL_FORMAT_RGB888;
static lcd_rgb_data_endian_t display_data_endian = LCD_RGB_DATA_ENDIAN_LITTLE;
static pax_buf_t fb = {0};
static QueueHandle_t input_event_queue = NULL;
static SemaphoreHandle_t vsync_sem = NULL;

// Application state
static app_state_t app_state = APP_STATE_BROWSER;
static file_browser_t browser = {0};
static char error_message[128] = {0};

// Video playback state
static avi_parser_t avi_parser = {0};
static int current_frame = 0;
static bool video_ended = false;
static int video_fps = 20;
static int frame_duration_ms = 50;

// Video frame ring buffer in PSRAM
#define VIDEO_BUFFER_FRAMES  16
#define VIDEO_FRAME_MAX_SIZE (64 * 1024)
#define PRE_BUFFER_TIME_MS   300

typedef struct {
    size_t size;
    int frame_index;
} buffered_frame_t;

static uint8_t* video_buffer_memory = NULL;
static buffered_frame_t video_frames[VIDEO_BUFFER_FRAMES];
static int video_write_idx = 0;
static int video_read_idx = 0;
static int video_buffered = 0;
static int next_frame_index = 0;
static bool end_of_file = false;

// Pending audio chunk
static uint8_t pending_audio_data[4096];
static size_t pending_audio_size = 0;

// Playback timing
static int64_t playback_start_time_us = 0;

// Pause state
static bool paused = false;
static int64_t pause_start_us = 0;
static int64_t total_pause_duration_us = 0;

// Mute state
static bool muted = false;
static int volume = 100;

// FPS overlay
static bool show_fps = false;
static int fps_frame_count = 0;
static int64_t fps_last_time_us = 0;
static float current_fps = 0.0f;

// Headphone detection
static int64_t last_hp_check_us = 0;
#define HP_CHECK_INTERVAL_US (3 * 1000000)

// Timing statistics
static uint32_t timing_decode_us = 0;
static uint32_t timing_copy_us = 0;
static uint32_t timing_frame_count = 0;

// Seek intervals (in seconds)
#define SEEK_SMALL   10
#define SEEK_MEDIUM  60
#define SEEK_LARGE   600

void blit(void) {
    bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
}

// --- Error display ---

static void show_error(const char* msg) {
    strncpy(error_message, msg, sizeof(error_message) - 1);
    error_message[sizeof(error_message) - 1] = '\0';
    app_state = APP_STATE_ERROR;
    ESP_LOGE(TAG, "Error: %s", msg);
}

static void draw_error_screen(uint8_t* fb_pixels, int stride, int height) {
    memset(fb_pixels, 0, stride * height * 3);
    hershey_draw_string(fb_pixels, stride, height, 50, 180, "ERROR", 32, 255, 60, 60);
    hershey_draw_string(fb_pixels, stride, height, 50, 240, error_message, 18, 255, 200, 200);
    hershey_draw_string(fb_pixels, stride, height, 50, 340, "Press any key to return", 16, 160, 160, 160);
}

// --- Video ring buffer ---

static inline uint8_t* video_buffer_slot(int idx) {
    return video_buffer_memory + (idx * VIDEO_FRAME_MAX_SIZE);
}

static int buffer_one_chunk(void) {
    if (pending_audio_size > 0) {
        if (audio_player_push_chunk(pending_audio_data, pending_audio_size) == ESP_OK) {
            pending_audio_size = 0;
        } else {
            return -2;
        }
    }

    if (video_buffered >= VIDEO_BUFFER_FRAMES) {
        return -1;
    }

    avi_chunk_t chunk;
    esp_err_t ret = avi_parser_next_chunk(&avi_parser, &chunk);

    if (ret != ESP_OK || chunk.type == AVI_CHUNK_END) {
        end_of_file = true;
        return 1;
    }

    if (chunk.type == AVI_CHUNK_AUDIO) {
        if (audio_player_push_chunk(chunk.data, chunk.size) == ESP_ERR_TIMEOUT) {
            if (chunk.size <= sizeof(pending_audio_data)) {
                memcpy(pending_audio_data, chunk.data, chunk.size);
                pending_audio_size = chunk.size;
            }
        }
        return 0;
    }

    if (chunk.type == AVI_CHUNK_VIDEO) {
        if (chunk.size > VIDEO_FRAME_MAX_SIZE) {
            ESP_LOGW(TAG, "Video frame too large: %zu, skipping", chunk.size);
            next_frame_index++;
            return 0;
        }

        uint8_t* dest = video_buffer_slot(video_write_idx);
        memcpy(dest, chunk.data, chunk.size);
        video_frames[video_write_idx].size = chunk.size;
        video_frames[video_write_idx].frame_index = next_frame_index++;

        video_write_idx = (video_write_idx + 1) % VIDEO_BUFFER_FRAMES;
        video_buffered++;
        return 0;
    }

    return 0;
}

static int prebuffer_chunks(void) {
    int target_frames = (PRE_BUFFER_TIME_MS * video_fps) / 1000;
    if (target_frames < 3) target_frames = 3;
    if (target_frames > VIDEO_BUFFER_FRAMES - 2) target_frames = VIDEO_BUFFER_FRAMES - 2;

    ESP_LOGI(TAG, "Pre-buffering %d frames...", target_frames);

    while (video_buffered < target_frames && !end_of_file) {
        int result = buffer_one_chunk();
        if (result == 1) break;
    }

    ESP_LOGI(TAG, "Pre-buffered %d video frames", video_buffered);
    return video_buffered;
}

static void reset_buffer_state(void) {
    video_write_idx = 0;
    video_read_idx = 0;
    video_buffered = 0;
    next_frame_index = 0;
    end_of_file = false;
    current_frame = 0;
    video_ended = false;
    pending_audio_size = 0;
}

// --- FPS overlay ---

static void update_fps(void) {
    fps_frame_count++;
    int64_t now = esp_timer_get_time();
    int64_t elapsed = now - fps_last_time_us;
    if (elapsed >= 1000000) {
        current_fps = (float)fps_frame_count * 1000000.0f / (float)elapsed;
        fps_frame_count = 0;
        fps_last_time_us = now;
    }
}

static void draw_fps_overlay(uint8_t* fb_pixels, int stride, int height) {
    if (!show_fps) return;
    char fps_str[16];
    snprintf(fps_str, sizeof(fps_str), "%.1f fps", current_fps);
    int tw = hershey_string_width(fps_str, 18);
    hershey_draw_string(fb_pixels, stride, height, 800 - tw - 10, 5, fps_str, 18, 0, 255, 0);
}

// --- Headphone detection ---

static void check_headphone_state(void) {
    int64_t now = esp_timer_get_time();
    if (now - last_hp_check_us < HP_CHECK_INTERVAL_US) return;
    last_hp_check_us = now;

    // Use BSP action events for headphone detection instead of direct coprocessor access
    // The headphone state is tracked via INPUT_EVENT_TYPE_ACTION events in the main loop
}

// --- Video playback ---

static bool process_video_frame(uint8_t* fb_pixels, int fb_stride, int fb_height) {
    // Read chunks to maintain buffers
    int chunks_read = 0;
    int max_chunks = 8;
    while (video_buffered < VIDEO_BUFFER_FRAMES && !end_of_file && chunks_read < max_chunks) {
        int result = buffer_one_chunk();
        if (result != 0) break;
        chunks_read++;
    }

    if (video_buffered == 0 && end_of_file) {
        audio_player_end_stream();
        ESP_LOGI(TAG, "Video ended at frame %d", current_frame);
        return true;
    }

    // Calculate expected frame from wall clock (accounting for pause)
    uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - playback_start_time_us - total_pause_duration_us) / 1000);
    int expected_frame = (int)(elapsed_ms / frame_duration_ms);

    if (current_frame > expected_frame) {
        return false;  // Ahead of schedule
    }

    if (video_buffered == 0) {
        return false;  // No frames ready
    }

    buffered_frame_t* frame = &video_frames[video_read_idx];
    uint8_t* frame_data = video_buffer_slot(video_read_idx);

    // Skip frames if behind
    int frames_skipped = 0;
    while (frame->frame_index < expected_frame && video_buffered > 1) {
        video_read_idx = (video_read_idx + 1) % VIDEO_BUFFER_FRAMES;
        video_buffered--;
        current_frame = frame->frame_index + 1;
        frames_skipped++;
        frame = &video_frames[video_read_idx];
        frame_data = video_buffer_slot(video_read_idx);
    }
    if (frames_skipped > 0) {
        ESP_LOGW(TAG, "Skipped %d frames (behind by %d)", frames_skipped, expected_frame - current_frame);
    }

    int64_t t0 = esp_timer_get_time();

    // Decode MJPEG frame
    uint8_t* bgr_out = NULL;
    int width = 0, height = 0;
    esp_err_t ret = mjpeg_decoder_decode(frame_data, frame->size, &bgr_out, &width, &height);

    video_read_idx = (video_read_idx + 1) % VIDEO_BUFFER_FRAMES;
    video_buffered--;

    int64_t t1 = esp_timer_get_time();

    if (ret == ESP_OK && bgr_out) {
        current_frame = frame->frame_index + 1;

        mjpeg_copy_to_framebuffer(bgr_out, fb_pixels, width, height, 800, 480);

        int64_t t2 = esp_timer_get_time();

        // FPS tracking
        update_fps();
        draw_fps_overlay(fb_pixels, fb_stride, fb_height);

        // Timing stats
        timing_decode_us += (t1 - t0);
        timing_copy_us += (t2 - t1);
        timing_frame_count++;

        if (timing_frame_count >= 30) {
            ESP_LOGI(TAG, "Avg(30): Decode=%.1fms Copy=%.1fms | Buf=%d",
                     timing_decode_us / 30000.0f, timing_copy_us / 30000.0f, video_buffered);
            timing_decode_us = 0;
            timing_copy_us = 0;
            timing_frame_count = 0;
        }
    }

    return false;
}

static esp_err_t start_playback(const char* path) {
    ESP_LOGI(TAG, "Starting playback: %s", path);

    esp_err_t ret = avi_parser_open(&avi_parser, path);
    if (ret != ESP_OK) {
        show_error("Failed to open file (not a valid AVI?)");
        return ret;
    }

    const avi_info_t* info = avi_parser_get_info(&avi_parser);

    if (!info->has_video) {
        avi_parser_close(&avi_parser);
        show_error("No video stream found in file");
        return ESP_ERR_NOT_FOUND;
    }

    if (info->width > 800 || info->height > 480) {
        ESP_LOGW(TAG, "Video %lux%lu exceeds display, will be clamped",
                 (unsigned long)info->width, (unsigned long)info->height);
    }

    video_fps = info->fps > 0 ? info->fps : 20;
    frame_duration_ms = 1000 / video_fps;

    ESP_LOGI(TAG, "AVI: %lux%lu @ %d fps, audio=%d, index=%d",
             (unsigned long)info->width, (unsigned long)info->height,
             video_fps, info->has_audio, avi_parser_can_seek(&avi_parser));

    // Allocate video buffer
    if (!video_buffer_memory) {
        size_t buffer_size = VIDEO_BUFFER_FRAMES * VIDEO_FRAME_MAX_SIZE;
        video_buffer_memory = heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
        if (!video_buffer_memory) {
            avi_parser_close(&avi_parser);
            show_error("Out of memory for video buffer");
            return ESP_ERR_NO_MEM;
        }
    }

    reset_buffer_state();

    // Init MJPEG decoder
    ret = mjpeg_decoder_init(info->width, info->height);
    if (ret != ESP_OK) {
        avi_parser_close(&avi_parser);
        show_error("Failed to init MJPEG decoder");
        return ret;
    }

    // Start audio
    if (info->has_audio) {
        ret = audio_player_start();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Audio start failed, continuing without audio");
        }
    }

    // Pre-buffer
    prebuffer_chunks();

    // Clear screen to black (for letterbox bars)
    pax_background(&fb, 0);
    blit();

    // Reset pause/timing state
    paused = false;
    total_pause_duration_us = 0;
    fps_frame_count = 0;
    fps_last_time_us = esp_timer_get_time();
    timing_decode_us = 0;
    timing_copy_us = 0;
    timing_frame_count = 0;

    // Sync timing to audio position
    uint32_t audio_already_ms = audio_player_get_position_ms();
    playback_start_time_us = esp_timer_get_time() - (audio_already_ms * 1000);

    app_state = APP_STATE_PLAYING;
    return ESP_OK;
}

static void stop_playback(void) {
    ESP_LOGI(TAG, "Stopping playback");
    audio_player_stop();
    mjpeg_decoder_deinit();
    avi_parser_close(&avi_parser);
    reset_buffer_state();
    paused = false;
}

static void do_seek(int delta_seconds) {
    if (!avi_parser_can_seek(&avi_parser)) return;

    int target_frame = current_frame + (delta_seconds * video_fps);
    if (target_frame < 0) target_frame = 0;
    int max_frame = avi_parser_get_frame_count(&avi_parser) - 1;
    if (target_frame > max_frame) target_frame = max_frame;

    int audio_idx = -1;
    esp_err_t ret = avi_parser_seek_to_frame(&avi_parser, target_frame, &audio_idx);
    if (ret != ESP_OK) return;

    // Flush video buffer
    video_write_idx = 0;
    video_read_idx = 0;
    video_buffered = 0;
    next_frame_index = target_frame;
    end_of_file = false;
    pending_audio_size = 0;

    // Stop and restart audio
    audio_player_stop();
    const avi_info_t* info = avi_parser_get_info(&avi_parser);
    if (info->has_audio) {
        audio_player_start();
    }

    // Pre-buffer from new position
    prebuffer_chunks();

    // Clear screen for new letterbox position
    pax_background(&fb, 0);

    // Reset timing
    current_frame = target_frame;
    video_ended = false;
    int64_t video_time_ms = (int64_t)target_frame * frame_duration_ms;
    playback_start_time_us = esp_timer_get_time() - (video_time_ms * 1000);
    total_pause_duration_us = 0;
    if (paused) {
        pause_start_us = esp_timer_get_time();
    }

    // Restore volume/mute state
    if (muted) {
        audio_player_set_volume(0);
    } else {
        audio_player_set_volume(volume);
    }

    ESP_LOGI(TAG, "Seeked to frame %d (%.1fs)", target_frame, target_frame / (float)video_fps);
}

// --- Main ---

void app_main(void) {
    usb_initialize();
    gpio_install_isr_service(0);

    // Initialize BSP
    const bsp_configuration_t bsp_configuration = {
        .display = {
            .requested_color_format = display_color_format,
            .num_fbs = 1,
        },
    };
    ESP_ERROR_CHECK(bsp_device_initialize(&bsp_configuration));

    // Turn off LEDs
    uint8_t led_data[18] = {0};
    bsp_led_write(led_data, sizeof(led_data));

    // Get display parameters
    ESP_ERROR_CHECK(bsp_display_get_parameters(&display_h_res, &display_v_res,
                                                &display_color_format, &display_data_endian));

    bsp_display_rotation_t display_rotation = bsp_display_get_default_rotation();

    pax_buf_type_t format = PAX_BUF_24_888RGB;
    if (display_color_format == LCD_COLOR_PIXEL_FORMAT_RGB565) {
        format = PAX_BUF_16_565RGB;
    }

    pax_orientation_t orientation = PAX_O_UPRIGHT;
    switch (display_rotation) {
        case BSP_DISPLAY_ROTATION_90:  orientation = PAX_O_ROT_CCW; break;
        case BSP_DISPLAY_ROTATION_180: orientation = PAX_O_ROT_HALF; break;
        case BSP_DISPLAY_ROTATION_270: orientation = PAX_O_ROT_CW; break;
        default: break;
    }

    pax_buf_init(&fb, NULL, display_h_res, display_v_res, format);
    pax_buf_reversed(&fb, display_data_endian == LCD_RGB_DATA_ENDIAN_BIG);
    pax_buf_set_orientation(&fb, orientation);

    // Get input queue
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    // Enable vsync
    esp_err_t te_err = bsp_display_set_tearing_effect_mode(BSP_DISPLAY_TE_V_BLANKING);
    if (te_err == ESP_OK) {
        te_err = bsp_display_get_tearing_effect_semaphore(&vsync_sem);
    }
    if (te_err != ESP_OK || vsync_sem == NULL) {
        ESP_LOGW(TAG, "Vsync not available");
        vsync_sem = NULL;
    }

    // Framebuffer access
    uint8_t* fb_pixels = (uint8_t*)pax_buf_get_pixels_rw(&fb);
    int fb_stride = display_h_res;
    int fb_height = display_v_res;

    // Initialize SD card
    esp_err_t res = sdcard_init();
    if (res != ESP_OK) {
        show_error("SD card not found. Insert card and reboot.");
    }

    // Initialize audio
    if (app_state != APP_STATE_ERROR) {
        res = audio_player_init();
        if (res != ESP_OK) {
            ESP_LOGW(TAG, "Audio init failed - videos will play without sound");
        }
    }

    // Initialize file browser
    if (app_state != APP_STATE_ERROR) {
        fb_init(&browser);
    }

    // Input state tracking
    bool key_up = false;
    bool key_down = false;
    bool key_left = false;
    bool key_right = false;
    bool key_enter = false;
    bool key_esc = false;
    bool key_space = false;
    bool key_f = false;
    bool key_m = false;
    bool ctrl_held = false;

    // Main loop
    while (1) {
        // Process input events
        bsp_input_event_t event;
        while (xQueueReceive(input_event_queue, &event, 0) == pdTRUE) {
            if (event.type == INPUT_EVENT_TYPE_SCANCODE) {
                bsp_input_scancode_t scancode = event.args_scancode.scancode;
                bool released = (scancode & BSP_INPUT_SCANCODE_RELEASE_MODIFIER) != 0;
                bsp_input_scancode_t base = scancode & ~BSP_INPUT_SCANCODE_RELEASE_MODIFIER;

                switch (base) {
                    case BSP_INPUT_SCANCODE_ESCAPED_GREY_UP:
                        key_up = !released;
                        break;
                    case BSP_INPUT_SCANCODE_ESCAPED_GREY_DOWN:
                        key_down = !released;
                        break;
                    case BSP_INPUT_SCANCODE_ESCAPED_GREY_LEFT:
                        key_left = !released;
                        break;
                    case BSP_INPUT_SCANCODE_ESCAPED_GREY_RIGHT:
                        key_right = !released;
                        break;
                    case BSP_INPUT_SCANCODE_ENTER:
                        key_enter = !released;
                        break;
                    case BSP_INPUT_SCANCODE_ESC:
                        key_esc = !released;
                        break;
                    case BSP_INPUT_SCANCODE_SPACE:
                        key_space = !released;
                        break;
                    case BSP_INPUT_SCANCODE_LEFTCTRL:
                    case BSP_INPUT_SCANCODE_ESCAPED_RCTRL:
                        ctrl_held = !released;
                        break;
                    default:
                        break;
                }
            } else if (event.type == INPUT_EVENT_TYPE_KEYBOARD) {
                // Handle ASCII keys for F and M
                char c = event.args_keyboard.ascii;
                if (c == 'f' || c == 'F') key_f = true;
                if (c == 'm' || c == 'M') key_m = true;
            } else if (event.type == INPUT_EVENT_TYPE_NAVIGATION) {
                if (event.args_navigation.state) {  // Press only
                    switch (event.args_navigation.key) {
                        case BSP_INPUT_NAVIGATION_KEY_VOLUME_UP:
                            if (volume < 100) volume += 10;
                            if (volume > 100) volume = 100;
                            if (!muted) audio_player_set_volume(volume);
                            ESP_LOGI(TAG, "Volume: %d%%", volume);
                            break;
                        case BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN:
                            if (volume > 0) volume -= 10;
                            if (volume < 0) volume = 0;
                            if (!muted) audio_player_set_volume(volume);
                            ESP_LOGI(TAG, "Volume: %d%%", volume);
                            break;
                        default:
                            break;
                    }
                }
            } else if (event.type == INPUT_EVENT_TYPE_ACTION) {
                bsp_device_restart_to_launcher();
            }
        }

        // State machine
        switch (app_state) {
            case APP_STATE_BROWSER: {
                if (key_esc) {
                    key_esc = false;
                    bsp_device_restart_to_launcher();
                }

                if (key_up) {
                    key_up = false;
                    fb_move_up(&browser);
                }
                if (key_down) {
                    key_down = false;
                    fb_move_down(&browser);
                }
                if (key_enter) {
                    key_enter = false;
                    char selected_path[FB_MAX_PATH];
                    if (fb_select(&browser, selected_path, sizeof(selected_path))) {
                        // Show loading screen before the slow open/parse/prebuffer
                        memset(fb_pixels, 0, fb_stride * fb_height * 3);
                        hershey_draw_string(fb_pixels, fb_stride, fb_height,
                                            250, 230, "Loading, please wait...", 22,
                                            200, 200, 200);
                        blit();
                        start_playback(selected_path);
                    }
                }

                if (app_state == APP_STATE_BROWSER) {
                    fb_draw(&browser, fb_pixels, fb_stride, fb_height);
                }
                break;
            }

            case APP_STATE_PLAYING: {
                // Handle Esc/Enter - return to browser
                if (key_esc || key_enter) {
                    key_esc = false;
                    key_enter = false;
                    stop_playback();
                    app_state = APP_STATE_BROWSER;
                    break;
                }

                // Pause
                if (key_space) {
                    key_space = false;
                    paused = !paused;
                    audio_player_pause(paused);
                    if (paused) {
                        pause_start_us = esp_timer_get_time();
                        ESP_LOGI(TAG, "Paused");
                    } else {
                        total_pause_duration_us += esp_timer_get_time() - pause_start_us;
                        ESP_LOGI(TAG, "Resumed");
                    }
                }

                // FPS toggle
                if (key_f) {
                    key_f = false;
                    show_fps = !show_fps;
                }

                // Mute toggle
                if (key_m) {
                    key_m = false;
                    muted = !muted;
                    audio_player_set_volume(muted ? 0 : volume);
                    ESP_LOGI(TAG, "Mute: %s", muted ? "on" : "off");
                }

                // Seeking
                if (key_left) {
                    key_left = false;
                    do_seek(ctrl_held ? -SEEK_LARGE : -SEEK_SMALL);
                }
                if (key_right) {
                    key_right = false;
                    do_seek(ctrl_held ? SEEK_LARGE : SEEK_SMALL);
                }
                if (key_up) {
                    key_up = false;
                    do_seek(SEEK_MEDIUM);
                }
                if (key_down) {
                    key_down = false;
                    do_seek(-SEEK_MEDIUM);
                }

                // Process video (unless paused)
                if (!paused) {
                    video_ended = process_video_frame(fb_pixels, fb_stride, fb_height);

                    if (video_ended) {
                        stop_playback();
                        app_state = APP_STATE_BROWSER;
                    }
                }

                // Headphone check
                check_headphone_state();
                break;
            }

            case APP_STATE_ERROR: {
                draw_error_screen(fb_pixels, fb_stride, fb_height);

                // Any key returns to browser
                if (key_esc || key_enter || key_space || key_up || key_down) {
                    key_esc = false;
                    key_enter = false;
                    key_space = false;
                    key_up = false;
                    key_down = false;

                    if (sdcard_is_mounted()) {
                        app_state = APP_STATE_BROWSER;
                    } else {
                        // Try remounting SD card
                        res = sdcard_init();
                        if (res == ESP_OK) {
                            fb_init(&browser);
                            app_state = APP_STATE_BROWSER;
                        }
                    }
                }
                break;
            }
        }

        // Reset single-shot key states
        key_f = false;
        key_m = false;

        // Vsync + blit
        if (vsync_sem != NULL) {
            // Short timeout during playback to avoid blocking too long
            TickType_t timeout = (app_state == APP_STATE_PLAYING && !paused)
                                 ? pdMS_TO_TICKS(5) : pdMS_TO_TICKS(50);
            xSemaphoreTake(vsync_sem, timeout);
        }
        blit();

        if (app_state != APP_STATE_PLAYING || paused) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}
