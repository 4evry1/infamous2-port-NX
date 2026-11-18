#pragma once
#include <stdint.h>
typedef struct {
    uint64_t first_ns, last_ns;
    unsigned anchor, consecutive;
} target_gate;

static inline int target_is_steady(target_gate *gate, unsigned target, unsigned draws, uint64_t now)
{
    unsigned band = target/32 + 1, drift = target/80 + 1;
    if (!target || draws + (uint64_t)band < target || draws > (uint64_t)target + band) {
        gate->first_ns = 0;
        return 0;
    }
    if (!gate->first_ns || now - gate->last_ns > 2000000000ull ||
        draws + (uint64_t)drift < gate->anchor || draws > (uint64_t)gate->anchor + drift) {
        gate->first_ns = now;
        gate->anchor = draws;
        gate->consecutive = 0;
    }
    gate->last_ns = now;
    ++gate->consecutive;
    return gate->consecutive >= 30 && now - gate->first_ns >= 15000000000ull;
}
