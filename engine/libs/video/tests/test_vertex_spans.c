/* Independent byte-set oracle for the ranges used to validate resident vertices. */
#include "../rsx_vertex_spans.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned char arena[1536];
static unsigned state = 1;
static unsigned next(void) { state = state * 1664525u + 1013904223u; return state >> 8; }

static void check(const unsigned starts[], const unsigned lengths[], unsigned count)
{
    struct rsx_vertex_span spans[16];
    unsigned char covered[sizeof arena] = {0};
    unsigned nr = 0;
    for (unsigned i = 0; i < count; ++i) {
        assert(lengths[i] && starts[i] + lengths[i] <= sizeof arena);
        memset(covered + starts[i], 1, lengths[i]);
        nr = rsx_vertex_span_add(spans, nr, arena + starts[i], arena + starts[i] + lengths[i]);
    }
    unsigned run = 0, i = 0;
    while (i < sizeof arena) {
        if (!covered[i]) { ++i; continue; }
        unsigned start = i, end = i + 1;
        for (++i; i < sizeof arena && i - end <= 64; ++i)
            if (covered[i]) end = i + 1;
        assert(run < nr && spans[run].begin == arena + start && spans[run].end == arena + end);
        ++run;
    }
    assert(run == nr);
}

int main(void)
{
    unsigned starts[16] = {193, 0, 128, 320, 65, 0, 60, 320};
    unsigned lengths[16] = {1, 1, 1, 64, 1, 128, 9, 64};
    for (unsigned n = 0; n <= 8; ++n) check(starts, lengths, n);
    for (unsigned trial = 0; trial < 20000; ++trial) {
        unsigned n = next() % 17;
        for (unsigned i = 0; i < n; ++i) {
            starts[i] = next() % 1280;
            lengths[i] = 1 + next() % 256;
        }
        check(starts, lengths, n);
    }
    puts("20,009 vertex span layouts matched the independent byte-coverage oracle");
}
