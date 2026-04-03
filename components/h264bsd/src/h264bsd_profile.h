#ifndef H264BSD_PROFILE_H
#define H264BSD_PROFILE_H

#include "basetype.h"

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#define H264_GET_TIME_US() ((u32)esp_timer_get_time())
#else
#include <time.h>
static inline u32 h264_get_time_us_posix(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u32)(ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
}
#define H264_GET_TIME_US() h264_get_time_us_posix()
#endif

enum {
    H264_TIMER_WRITE_OUTPUT_BLOCKS,
    H264_TIMER_CONVERT_PPA,
    H264_TIMER_FILTER_PICTURE,
    H264_TIMER_INTER_PREDICTION,
    H264_TIMER_INTRA_PREDICTION,
    H264_TIMER_PROCESS_RESIDUAL,
    H264_TIMER_LUMA_INTERP,
    H264_TIMER_CHROMA_INTERP,
    H264_TIMER_COUNT
};

extern u32 h264bsd_timers[H264_TIMER_COUNT];
extern u32 h264bsd_luma_case_counts[16];

static inline void h264bsdTimerAdd(int id, u32 elapsed) {
    h264bsd_timers[id] += elapsed;
}

void h264bsdResetTimers(void);
u32 h264bsdGetTimer(int id);

#endif /* H264BSD_PROFILE_H */
