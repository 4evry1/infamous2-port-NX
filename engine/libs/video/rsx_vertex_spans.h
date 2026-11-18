#ifndef RSX_VERTEX_SPANS_H
#define RSX_VERTEX_SPANS_H

#include <stdint.h>
#include <string.h>

struct rsx_vertex_span {
    const unsigned char *begin, *end;
};

/* Preserve the original hash coverage, including gaps of up to 64 bytes. */
static inline int rsx_vertex_spans_apart(const unsigned char *end, const unsigned char *begin)
{
    return (uintptr_t)begin > (uintptr_t)end && (uintptr_t)begin - (uintptr_t)end > 64u;
}

/* One slot per input attribute is sufficient. Keep the union in address order
 * so interleaved attributes normally just extend the last span in place. */
static inline unsigned rsx_vertex_span_add(struct rsx_vertex_span *spans, unsigned count,
                                           const unsigned char *begin, const unsigned char *end)
{
    if (!count || (uintptr_t)begin >= (uintptr_t)spans[count - 1].begin) {
        if (count && !rsx_vertex_spans_apart(spans[count - 1].end, begin)) {
            if ((uintptr_t)end > (uintptr_t)spans[count - 1].end) spans[count - 1].end = end;
            return count;
        }
        spans[count] = (struct rsx_vertex_span){begin, end};
        return count + 1;
    }

    unsigned first = 0;
    while (rsx_vertex_spans_apart(spans[first].end, begin)) ++first;
    unsigned after = first;
    while (after < count && !rsx_vertex_spans_apart(end, spans[after].begin)) {
        if ((uintptr_t)spans[after].begin < (uintptr_t)begin) begin = spans[after].begin;
        if ((uintptr_t)spans[after].end > (uintptr_t)end) end = spans[after].end;
        ++after;
    }
    if (after != first + 1 && after < count)
        memmove(spans + first + 1, spans + after, (count - after) * sizeof *spans);
    spans[first] = (struct rsx_vertex_span){begin, end};
    return count - (after - first) + 1;
}

#endif
