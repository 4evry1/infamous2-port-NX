#ifndef AUDIO_ROUTE_TRACE_H
#define AUDIO_ROUTE_TRACE_H
#include <stdint.h>

typedef struct {
    uint64_t start_ns, ready_ns;
    unsigned first_queue, min_queue, last_queue, mixed_empty;
} audio_route_gap;

typedef struct {
    audio_route_gap active, longest;
    unsigned recovered;
    uint64_t report_ns;
} audio_route_trace;

static inline void audio_route_observe(audio_route_trace *trace, uint64_t now,
                                      int empty, unsigned queued, int will_mix_empty)
{
    audio_route_gap *gap = &trace->active;
    if (empty && !gap->start_ns)
        *gap = (audio_route_gap){ .start_ns = now, .first_queue = queued, .min_queue = queued };
    if (!gap->start_ns) return;
    gap->last_queue = queued;
    if (queued < gap->min_queue) gap->min_queue = queued;
    if (empty) {
        gap->mixed_empty += will_mix_empty;
        return;
    }
    gap->ready_ns = now;
    ++trace->recovered;
    if (now - gap->start_ns >= trace->longest.ready_ns - trace->longest.start_ns)
        trace->longest = *gap;
    *gap = (audio_route_gap){0};
}
#endif
