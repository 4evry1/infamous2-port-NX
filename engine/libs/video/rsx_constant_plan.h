#ifndef PS3RECOMP_RSX_CONSTANT_PLAN_H
#define PS3RECOMP_RSX_CONSTANT_PLAN_H

#include "ps3emu/ps3types.h"
#include <string.h>

enum { RSX_CONSTANT_ROWS = 512, RSX_CONSTANT_ROW_BYTES = 16,
       RSX_CONSTANT_MAX_SPANS = 8, RSX_CONSTANT_MIN_SAVED_BYTES = 256 };

typedef struct { u32 first, count; } rsx_constant_span;
/* count == 0 requests the existing contiguous upload. */
typedef struct {
    u32 count, rows;
    rsx_constant_span spans[RSX_CONSTANT_MAX_SPANS];
} rsx_constant_plan;

static inline int rsx_constant_plan_valid(const rsx_constant_plan* p, u32 first, u32 count)
{
    if (!p || !p->count || p->count > RSX_CONSTANT_MAX_SPANS ||
        first > RSX_CONSTANT_ROWS || count > RSX_CONSTANT_ROWS - first) return 0;
    u32 end = first, rows = 0;
    for (u32 i = 0; i < p->count; i++) {
        const rsx_constant_span s = p->spans[i];
        if (!s.count || s.first < end || s.first > first + count ||
            s.count > first + count - s.first) return 0;
        end = s.first + s.count; rows += s.count;
    }
    return rows == p->rows;
}

static inline int rsx_constant_plan_equal(const rsx_constant_plan* a, const rsx_constant_plan* b)
{
    return a->count <= RSX_CONSTANT_MAX_SPANS && a->count == b->count && a->rows == b->rows &&
           !memcmp(a->spans, b->spans, a->count * sizeof a->spans[0]);
}

static inline void rsx_constant_plan_build(rsx_constant_plan* p, const u64 bits[8], u32 first, u32 count)
{
    memset(p, 0, sizeof *p);
    if (first > RSX_CONSTANT_ROWS || count > RSX_CONSTANT_ROWS - first) return;
    for (u32 c = first; c < first + count;) {
        if (!(bits[c / 64] & (1ull << (c % 64)))) { c++; continue; }
        const u32 start = c++;
        while (c < first + count && (bits[c / 64] & (1ull << (c % 64)))) c++;
        if (p->count == RSX_CONSTANT_MAX_SPANS) { memset(p, 0, sizeof *p); return; }
        p->spans[p->count++] = (rsx_constant_span){start, c - start};
        p->rows += c - start;
    }
    if (p->count < 2 || (count - p->rows) * RSX_CONSTANT_ROW_BYTES < RSX_CONSTANT_MIN_SAVED_BYTES)
        memset(p, 0, sizeof *p);
}

static inline void rsx_constant_plan_copy(void* dst, const void* src, const rsx_constant_plan* p)
{
    for (u32 i = 0; i < p->count; i++) {
        const size_t offset = (size_t)p->spans[i].first * RSX_CONSTANT_ROW_BYTES;
        memcpy((u8*)dst + offset, (const u8*)src + offset,
               (size_t)p->spans[i].count * RSX_CONSTANT_ROW_BYTES);
    }
}
#endif
