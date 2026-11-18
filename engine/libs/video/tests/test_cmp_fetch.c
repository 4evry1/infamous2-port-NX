#define _GNU_SOURCE
#include "rsx_vertex_compact.h"
#include "rsx_vertex_formats.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void store_word(u8* out, u32 word) {
    for (u32 k = 0; k < 4; ++k) out[k] = (u8)(word >> (24 - 8 * k));
}

static void check_run(u32 type, const u8* src, u32 count, u32 stride, u32 first, u32 size) {
    rsx_vertex_fetch_plan plan = {0};
    rsx_vertex_layout_plan_init(&plan.layout, 1u);
    rsx_vertex_fetch_attr* fetch = &plan.attr[0];
    fetch->desc.type = type;
    fetch->desc.size = size;
    fetch->desc.stride = stride;
    fetch->elem_size = rsx_vertex_attrib_size(type, size);
    fetch->stride = stride;
    fetch->stable_base = src;
    fetch->stable_first = first;
    fetch->stable_last = first + count - 1;
    rsx_vertex_ref refs[2048];
    u8 actual[2048 * 16];
    assert(count <= 2048);
    for (u32 i = 0; i < count; ++i) {
        refs[i].vertex_id = first + i - 3;
        refs[i].base_index = 3;
    }
    memset(actual, 0xcc, sizeof actual);
    for (int consecutive = 0; consecutive <= 1; ++consecutive) {
        memset(actual, 0xcc, sizeof actual);
        assert(rsx_vertex_fetch_all_ex(&plan, refs, count, actual, consecutive));
        for (u32 i = 0; i < count; ++i) {
            float expected[4];
            assert(rsx_vertex_decode_element(type, size, src + (u64)i * stride, expected));
            assert(memcmp(actual + i * 16, expected, 16) == 0);
        }
        for (u32 i = count * 16; i < sizeof actual; ++i) assert(actual[i] == 0xcc);
    }
}

int main(void) {
    u8 data[2048 * 17 + 1];
    unsigned runs = 0;
    const u32 strides[] = {4, 5, 8, 17};
    for (unsigned st = 0; st < sizeof strides / sizeof strides[0]; ++st)
    for (unsigned field = 0; field < 3; ++field)
    for (unsigned polarity = 0; polarity < 2; ++polarity) {
        const u32 stride = strides[st];
        for (u32 i = 0; i < 2048; ++i) {
            const u32 shifts[] = {0, 11, 22};
            const u32 mask = field == 2 ? 1023u : 2047u;
            u32 word = polarity ? 0xffffffffu : 0;
            word = (word & ~(mask << shifts[field])) | ((i & mask) << shifts[field]);
            store_word(data + 1 + (u64)i * stride, word);
        }
        for (u32 size = 1; size <= 5; ++size) {
            check_run(RSX_VTX_TYPE_CMP32, data + 1, 2048, stride, 7, size);
            ++runs;
        }
    }
    for (u32 size = 3; size <= 4; ++size)
    for (u32 pad = 0; pad <= 1; ++pad)
    for (u32 chunk = 0; chunk < 32; ++chunk) {
        const u32 stride = size * 2 + pad;
        for (u32 i = 0; i < 2048; ++i)
        for (u32 k = 0; k < size; ++k) {
            const u16 value = (u16)((chunk * 2048u + i) ^ (k * 0x5555u));
            data[1 + i * stride + k * 2] = (u8)(value >> 8);
            data[1 + i * stride + k * 2 + 1] = (u8)value;
        }
        check_run(RSX_VTX_TYPE_SNORM16, data + 1, 2048, stride, 7, size);
        if (size == 4) check_run(RSX_VTX_TYPE_HALF, data + 1, 2048, stride, 7, size);
    }
    puts("PASS: all 65536 SNORM16 encodings per lane sizes 3/4 and HALF4 encodings per lane, tight and odd stride");
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    u8* pages = mmap(NULL, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(pages != MAP_FAILED);
    assert(mprotect(pages + page, page, PROT_READ | PROT_WRITE) == 0);
    for (u32 count = 1; count <= 7; ++count) {
        u8* end = pages + 2 * page - count * 4;
        for (u32 i = 0; i < count; ++i) store_word(end + 4 * i, 0x80200400u + i);
        check_run(RSX_VTX_TYPE_CMP32, end, count, 4, 7, 3);
        check_run(RSX_VTX_TYPE_CMP32, end + (count - 1) * 4, count, 0, 7, 3);
    }
    for (u32 size = 1; size <= 4; ++size)
    for (u32 count = 1; count <= 7; ++count)
    for (u32 pad = 0; pad <= 3; ++pad) {
        const u32 stride = size * 2 + pad;
        u8* end = pages + 2 * page - ((count - 1) * stride + size * 2);
        for (u32 i = 0; i < count; ++i)
        for (u32 k = 0; k < size; ++k) {
            const u16 value = (i + k) & 1 ? 0x8000u : 0x7fffu;
            end[i * stride + k * 2] = (u8)(value >> 8);
            end[i * stride + k * 2 + 1] = (u8)value;
        }
        check_run(RSX_VTX_TYPE_SNORM16, end, count, stride, 7, size);
        check_run(RSX_VTX_TYPE_SNORM16, end + (count - 1) * stride, count, 0, 7, size);
    }
    const u16 half_bits[] = {0, 0x8000, 1, 0x83ff, 0x0400, 0x7bff, 0x7c00, 0xfc00, 0x7e55, 0x7c01};
    for (u32 count = 1; count <= 17; ++count)
    for (u32 pad = 0; pad <= 7; ++pad) {
        const u32 stride = 8 + pad;
        u8* end = pages + 2 * page - ((count - 1) * stride + 8);
        for (u32 i = 0; i < count; ++i)
        for (u32 k = 0; k < 4; ++k) {
            const u16 h = half_bits[(i + k) % 10];
            end[i * stride + k * 2] = h >> 8;
            end[i * stride + k * 2 + 1] = h;
        }
        assert(!mprotect(pages + page, page, PROT_READ));
        check_run(RSX_VTX_TYPE_HALF, end, count, stride, 7, 4);
        check_run(RSX_VTX_TYPE_HALF, end + (count - 1) * stride, count, 0, 7, 4);
        assert(!mprotect(pages + page, page, PROT_READ | PROT_WRITE));
    }
    puts("PASS: HALF4 protected/read-only ends, tight/odd/zero strides, sign/subnormal/infinity/NaN bits");
    const u32 float_bits[] = {0u, 0x80000000u, 0x7f800000u, 0xff800000u,
                              0x7fc01234u, 0x7f801234u, 1u, 0x807fffffu};
    for (u32 size = 1; size <= 4; ++size)
    for (u32 count = 1; count <= 7; ++count)
    for (u32 pad = 0; pad <= 3; ++pad) {
        const u32 stride = size * 4 + pad;
        u8* end = pages + 2 * page - ((count - 1) * stride + size * 4);
        for (u32 i = 0; i < count; ++i)
        for (u32 k = 0; k < size; ++k)
            store_word(end + i * stride + k * 4, float_bits[(i + k) % 8]);
        check_run(RSX_VTX_TYPE_FLOAT, end, count, stride, 7, size);
        check_run(RSX_VTX_TYPE_FLOAT, end + (count - 1) * stride, count, 0, 7, size);
    }
    assert(munmap(pages, page * 3) == 0);
    printf("PASS: %u packed-field runs, all component encodings, unaligned/strided/zero-stride inputs, protected ends; SNORM16 and FLOAT boundary runs\n", runs);
}
