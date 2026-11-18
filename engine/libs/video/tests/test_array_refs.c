#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef struct { u32 vertex_id, base_index; } rsx_vertex_ref;
static struct { rsx_vertex_ref* refs; u32 n_refs, cap_refs; } dc;
static unsigned allocations;
static int fail_allocation;
static void* checked_realloc(void* p, size_t n) {
    ++allocations;
    return fail_allocation ? NULL : realloc(p, n);
}
#define realloc checked_realloc
#define rsx_lim_event(...) ((void)0)
#define rsx_lim_use(...) ((void)0)
#include "production.inc"
#undef realloc
int main(void) {
    assert(dc_append_array_refs(9, 0));
    assert(!dc.refs && !dc.n_refs && !allocations);
    assert(dc_append_array_refs(UINT32_MAX - 2u, 6));
    assert(dc.n_refs == 6 && allocations == 1);
    for (u32 i = 0; i < 6; ++i) {
        assert(dc.refs[i].vertex_id == UINT32_MAX - 2u + i);
        assert(dc.refs[i].base_index == 0);
    }
    dc.refs[0].base_index = 17;
    assert(dc_append_array_refs(27, 4096));
    assert(dc.n_refs == 4102 && allocations == 2 && dc.refs[0].base_index == 17);
    for (u32 i = 0; i < 4096; ++i)
        assert(dc.refs[i + 6].vertex_id == 27 + i && dc.refs[i + 6].base_index == 0);
    const u32 before = dc.n_refs, capacity = dc.cap_refs;
    rsx_vertex_ref* pointer = dc.refs;
    fail_allocation = 1;
    assert(!dc_append_array_refs(0, capacity));
    assert(dc.n_refs == before && dc.cap_refs == capacity && dc.refs == pointer);
    assert(!dc_append_array_refs(0, UINT32_MAX));
    assert(dc.n_refs == before && dc.cap_refs == capacity && dc.refs == pointer);
    const unsigned calls = allocations;
    assert(dc_append_array_refs(123, 0) && allocations == calls);
    assert(dc_append_array_refs(123, 3) && allocations == calls);
    assert(dc.n_refs == before + 3 && dc.refs[before].vertex_id == 123);
    assert(dc.refs[0].base_index == 17 && dc.refs[6].vertex_id == 27);
    free(dc.refs);
    puts("PASS: array append order, wrap, zero count, growth, reuse, overflow and allocation failure");
}
