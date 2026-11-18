#define _GNU_SOURCE
#include "rsx_vertex_compact.h"
#include "rsx_vertex_formats.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static u8 memory[(1u << 23) + 64];
static const u8* map_memory(void* user, u32 location, u32 offset, u32 bytes) {
    (void)user; (void)location;
    if ((u64)offset + bytes > sizeof memory) return NULL;
    return memory + offset;
}

int main(void) {
    const u32 frequencies[] = {0, 1, 2, 3, 7, 16, 255, 1024, 65535};
    const u32 counts[] = {1, 2, 3, 7, 8, 15, 16, 31, 32, 33, 63, 64, 65, 257, 4096};
    const u32 starts[] = {0, 1, 5, 65534, 0xffff0u, UINT32_MAX - 5};
    const u32 bases[] = {0, 3, 65533, UINT32_MAX - 3};
    static rsx_vertex_ref refs[4096];
    const size_t lazy_page = (size_t)sysconf(_SC_PAGESIZE);
    u8* lazy_pages = mmap(NULL, lazy_page * 3, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(lazy_pages != MAP_FAILED);
    assert(mprotect(lazy_pages + lazy_page, lazy_page, PROT_READ | PROT_WRITE) == 0);
    rsx_vertex_ref* lazy_ref = (rsx_vertex_ref*)(lazy_pages + lazy_page * 2 - sizeof *lazy_ref);
    unsigned cases = 0, lazy_cases = 0;
    for (unsigned fi = 0; fi < sizeof frequencies / sizeof frequencies[0]; ++fi)
    for (unsigned ci = 0; ci < sizeof counts / sizeof counts[0]; ++ci)
    for (unsigned si = 0; si < sizeof starts / sizeof starts[0]; ++si)
    for (unsigned bi = 0; bi < sizeof bases / sizeof bases[0]; ++bi)
    for (u32 modulo = 0; modulo < 2; ++modulo) {
        u32 count = counts[ci];
        for (u32 i = 0; i < count; ++i) refs[i] = (rsx_vertex_ref){starts[si] + i, bases[bi]};
        rsx_vertex_fetch_plan walk = {0};
        rsx_vertex_layout_plan_init(&walk.layout, 1u | (1u << 3));
        walk.guest_ptr = map_memory; walk.base_offset = 4;
        walk.divider_mask = modulo ? 1u << 3 : 0;
        for (u32 slot = 0; slot < walk.layout.count; ++slot) {
            const u32 attr = walk.layout.attrs[slot];
            rsx_vertex_fetch_attr* f = &walk.attr[attr];
            f->desc.type = RSX_VTX_TYPE_FLOAT; f->desc.size = 1;
            f->desc.frequency = attr == 3 ? frequencies[fi] : 0;
            f->desc.offset = attr == 3 ? 8 : 16;
            f->stride = attr == 3 ? 8 : 4; f->elem_size = 4;
        }
        rsx_vertex_fetch_plan fast = walk;
        rsx_vertex_fetch_plan_prepare_ex(&walk, refs, count, 0);
        rsx_vertex_fetch_plan_prepare_ex(&fast, refs, count, 1);
        for (u32 slot = 0; slot < walk.layout.count; ++slot) {
            const u32 attr = walk.layout.attrs[slot];
            assert(walk.attr[attr].stable_base == fast.attr[attr].stable_base);
            assert(walk.attr[attr].stable_first == fast.attr[attr].stable_first);
            assert(walk.attr[attr].stable_last == fast.attr[attr].stable_last);
        }
        const u32 first_element = rsx_vertex_element_index(refs[0].vertex_id, refs[0].base_index, 0, 0);
        if ((u64)first_element + count - 1u <= 0xfffffu) {
            *lazy_ref = refs[0];
            rsx_vertex_fetch_plan lazy = walk;
            rsx_vertex_fetch_plan_prepare_ex(&lazy, lazy_ref, count, 1);
            for (u32 slot = 0; slot < walk.layout.count; ++slot) {
                const u32 attr = walk.layout.attrs[slot];
                assert(walk.attr[attr].stable_base == lazy.attr[attr].stable_base);
                assert(walk.attr[attr].stable_first == lazy.attr[attr].stable_first);
                assert(walk.attr[attr].stable_last == lazy.attr[attr].stable_last);
            }
            ++lazy_cases;
        }
        ++cases;
    }
    assert(munmap(lazy_pages, lazy_page * 3) == 0);
    printf("PASS: %u protected single-reference lazy preparation cases\n", lazy_cases);
    rsx_vertex_remap remap = {0};
    rsx_vertex_ref mixed[] = {{3, 0}, {2, 1}, {1, 1}};
    rsx_vertex_ref before[3]; memcpy(before, mixed, sizeof mixed);
    u32 output = 0x12345678u;
    assert(!rsx_vertex_remap_build_range(&remap, mixed, 3, &output));
    assert(output == 0x12345678u && !memcmp(mixed, before, sizeof mixed));
    assert(remap.occurrence_to_unique == NULL && remap.occurrence_capacity == 0);
    rsx_vertex_ref edge[] = {{UINT32_MAX, 4}, {UINT32_MAX - 1, 4}, {UINT32_MAX, 4}};
    assert(rsx_vertex_remap_build_range(&remap, edge, 3, &output));
    assert(output == 2 && edge[0].vertex_id == UINT32_MAX - 1 && edge[1].vertex_id == UINT32_MAX);
    assert(rsx_vertex_remap_index(&remap, 0) == 1 && rsx_vertex_remap_index(&remap, 1) == 0);
    rsx_vertex_ref wide[] = {{0, 4}, {UINT32_MAX, 4}};
    output = 123;
    assert(!rsx_vertex_remap_build_range(&remap, wide, 2, &output) && output == 123);
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    u8* pages = mmap(NULL, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(pages != MAP_FAILED);
    assert(mprotect(pages + page, page, PROT_READ | PROT_WRITE) == 0);
    for (u32 count = 1; count <= 33; ++count) {
        rsx_vertex_ref* bounded = (rsx_vertex_ref*)(pages + page * 2 - count * sizeof *bounded);
        for (u32 i = 0; i < count; ++i) bounded[i] = (rsx_vertex_ref){count - i - 1, 7};
        assert(rsx_vertex_remap_build_range(&remap, bounded, count, &output) && output == count);
        for (u32 i = 0; i < count; ++i)
            assert(bounded[i].vertex_id == i && rsx_vertex_remap_index(&remap, i) == count - i - 1);
    }
    assert(munmap(pages, page * 3) == 0);
    rsx_vertex_remap_destroy(&remap);
    printf("PASS: %u endpoint/walk range cases; rebasing/wrap/modulo/divide; mixed-base rejection and UINT32 edges; protected ends\n", cases);
}
