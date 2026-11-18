#include "rsx_vertex_compact.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static u32 state = 0x12345678;
static u32 next(void) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; }
static unsigned cases;
static void check(const u8* source, u32 count, int wide, u32 base)
{
    rsx_vertex_ref expected[1024], actual[1024];
    assert(count <= 1024);
    memset(expected, 0xa5, sizeof expected); memset(actual, 0xa5, sizeof actual);
    u32 low = UINT32_MAX, high = 0;
    for (u32 i = 0; i < count; ++i) {
        const u8* p = source + i * (wide ? 4u : 2u);
        u32 value = ((u32)p[0] << 8) | p[1];
        if (wide) value = (value << 16) | ((u32)p[2] << 8) | p[3];
        expected[i] = (rsx_vertex_ref){value, base};
        if (value < low) low = value;
        if (value > high) high = value;
    }
    rsx_vertex_ref_bounds bounds;
    rsx_vertex_decode_index_run(source, actual, count, wide, base, &bounds);
    assert(bounds.low == low && bounds.high == high && bounds.base_index == base);
    assert(!memcmp(expected, actual, sizeof actual));
    rsx_vertex_remap old = {0}, candidate = {0};
    u32 old_count = 0xdeadbeef, new_count = old_count;
    int old_ok = rsx_vertex_remap_build_range(&old, expected, count, &old_count);
    int new_ok = rsx_vertex_remap_build_range_with_bounds(&candidate, actual, count, &bounds, &new_count);
    assert(old_ok == new_ok && old_count == new_count);
    assert(!memcmp(expected, actual, sizeof actual));
    if (old_ok) assert(!memcmp(old.occurrence_to_unique, candidate.occurrence_to_unique, count * sizeof(u32)));
    rsx_vertex_remap_destroy(&old); rsx_vertex_remap_destroy(&candidate);
    ++cases;
}
int main(void)
{
    u8 bytes[4112];
    for (int wide = 0; wide <= 1; ++wide) {
        for (u32 n = 0; n < 10000; ++n) {
            u32 count = n % 1025, offset = n % 16;
            for (u32 i = 0; i < count; ++i) {
                u32 value = n % 2 ? next() % (count ? count : 1) : next();
                u8* p = bytes + offset + i * (wide ? 4u : 2u);
                if (wide) { p[0] = value >> 24; p[1] = value >> 16; p[2] = value >> 8; p[3] = value; }
                else { p[0] = value >> 8; p[1] = value; }
            }
            check(bytes + offset, count, wide, next());
        }
    }
    for (u32 value = 0; value < 65536; ++value) {
        bytes[0] = value >> 8; bytes[1] = value;
        check(bytes, 1, 0, UINT32_MAX);
    }
    const u32 extremes[] = {0, 1, 0x7fffffff, 0x80000000, 0xfffffffe, UINT32_MAX};
    for (u32 k = 0; k < sizeof extremes / sizeof *extremes; ++k) {
        u32 value = extremes[k];
        bytes[0] = value >> 24; bytes[1] = value >> 16; bytes[2] = value >> 8; bytes[3] = value;
        check(bytes, 1, 1, UINT32_MAX);
    }
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    u8* guarded = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(guarded != MAP_FAILED && page >= 4096);
    assert(!mprotect(guarded + page, page, PROT_NONE));
    for (size_t i = 0; i < page; ++i) guarded[i] = (u8)i;
    assert(!mprotect(guarded, page, PROT_READ));
    for (int wide = 0; wide <= 1; ++wide)
        for (u32 count = 0; count <= 1024; ++count)
            check(guarded + page - count * (wide ? 4u : 2u), count, wide, 0);
    assert(!munmap(guarded, page * 2));
    printf("%u decoded-index bounds/remap cases passed\n", cases);
}
