#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef uint64_t u64;
typedef struct { int id; } rsx_vertex_layout_plan;
typedef struct { int id; } eng_vc_entry;
static eng_vc_entry entry = {9};
static u64 ds_vc_ns = 100, ticks;
static int clock_calls, probes, missing;
static u64 eng_ds_now(void) { ++clock_calls; return ticks; }
static eng_vc_entry* eng_vc_probe(const rsx_vertex_layout_plan *layout, u64 *content) {
    assert(layout->id == 7); ++probes; ticks += 37; *content = 12345;
    return missing ? NULL : &entry;
}
#include "production.inc"
int main(void) {
    rsx_vertex_layout_plan layout = {7};
    for (int timed = 0; timed < 2; ++timed) for (missing = 0; missing < 2; ++missing) {
        u64 content = 0, before = ds_vc_ns; int calls = clock_calls, count = probes;
        assert(eng_vc_probe_measured(&layout, &content, timed) == (missing ? NULL : &entry));
        assert(content == 12345 && probes == count + 1);
        assert(clock_calls == calls + 2 * timed);
        assert(ds_vc_ns == before + 37 * timed);
    }
    puts("PASS: hit/miss contract, additive timing, no clock reads when disabled");
}
