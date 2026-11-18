#pragma once
#include <stdint.h>

typedef struct {
    uint64_t start_ns, guest, outputs;
    unsigned base_tenths, output_tenths;
    int started, valid;
} hz_fps_window;

static inline int hz_fps_update(hz_fps_window *w, uint64_t ns, uint64_t guest, uint64_t outputs)
{
    if (!w->started || ns < w->start_ns || guest < w->guest || outputs < w->outputs) {
        *w = (hz_fps_window){.start_ns = ns, .guest = guest, .outputs = outputs, .started = 1};
        return 1;
    }
    uint64_t elapsed = ns - w->start_ns;
    if (elapsed < 1000000000ull) return 0;
    w->base_tenths = (unsigned)((double)(guest - w->guest) * 1e10 / elapsed + 0.5);
    w->output_tenths = (unsigned)((double)(outputs - w->outputs) * 1e10 / elapsed + 0.5);
    w->valid = 1;
    w->start_ns = ns;
    w->guest = guest;
    w->outputs = outputs;
    return 1;
}

uint64_t hz_guest_frame_count(void);
uint64_t hz_output_frame_count(void);
