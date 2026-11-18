#include "../audio_route_trace.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    audio_route_trace trace = {0};
    audio_route_observe(&trace, 100, 0, 2048, 0);
    assert(!trace.active.start_ns && !trace.recovered);
    audio_route_observe(&trace, 200, 1, 1024, 0);
    audio_route_observe(&trace, 800, 1, 256, 1);
    audio_route_observe(&trace, 1000, 1, 512, 1);
    assert(trace.active.start_ns == 200 && !trace.active.ready_ns && !trace.recovered);
    audio_route_observe(&trace, 1400, 0, 0, 0);
    assert(trace.recovered == 1 && !trace.active.start_ns);
    assert(trace.longest.start_ns == 200 && trace.longest.ready_ns == 1400);
    assert(trace.longest.first_queue == 1024 && !trace.longest.min_queue && !trace.longest.last_queue);
    assert(trace.longest.mixed_empty == 2);
    audio_route_observe(&trace, 1800, 1, 512, 0);
    audio_route_observe(&trace, 2000, 0, 768, 0);
    assert(trace.recovered == 2 && trace.longest.start_ns == 200);
    audio_route_observe(&trace, 2100, 1, 1792, 0);
    audio_route_observe(&trace, 6000, 0, 2048, 0);
    assert(trace.recovered == 3 && trace.longest.start_ns == 2100);
    assert(trace.longest.min_queue == 1792 && trace.longest.last_queue == 2048 && !trace.longest.mixed_empty);
    puts("PCM gap timing, queue low-water and recovery accounting passed");
}
