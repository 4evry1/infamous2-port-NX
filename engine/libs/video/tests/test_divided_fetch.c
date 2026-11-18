#include "rsx_vertex_compact.h"
#include "rsx_vertex_formats.h"
#include <assert.h>
#include <fenv.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

enum { MAX_VERTICES = 65, SOURCE_BYTES = 4096, OUTPUT_BYTES = MAX_VERTICES * 256 + 32 };
static u32 random_state = 0x12345678;
static u32 random_word(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static void check_overlapping_source_output(void)
{
    for (u32 type = 1; type <= 7; ++type) {
        for (u32 offset = 0; offset <= 20; ++offset) {
            u8 expected[128], actual[128];
            for (u32 i = 0; i < sizeof expected; ++i) expected[i] = (u8)random_word();
            memcpy(actual, expected, sizeof actual);
            rsx_vertex_fetch_plan plan = {0};
            rsx_vertex_layout_plan_init(&plan.layout, 1);
            plan.attr[0].desc.type = type;
            plan.attr[0].desc.size = 4;
            plan.attr[0].desc.frequency = 7;
            plan.attr[0].elem_size = rsx_vertex_attrib_size(type, 4);
            plan.attr[0].stride = plan.attr[0].elem_size;
            const rsx_vertex_ref refs[] = {{0, 0}, {1, 0}, {2, 0}, {3, 0}};
            plan.attr[0].stable_base = expected + offset;
            assert(rsx_vertex_fetch_all_ex(&plan, refs, 4, expected, 0));
            plan.attr[0].stable_base = actual + offset;
            assert(rsx_vertex_fetch_all_ex(&plan, refs, 4, actual, 1));
            assert(!memcmp(expected, actual, sizeof actual));
        }
    }
    puts("PASS: 147 overlapping source/output cases retain sequential decoding semantics");
}

static void check_protected_repetition(void)
{
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    u8* source_map = mmap(NULL, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    u8* output_map = mmap(NULL, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(source_map != MAP_FAILED && output_map != MAP_FAILED);
    assert(!mprotect(output_map + page, page, PROT_READ | PROT_WRITE));
    for (u32 type = 1; type <= 7; ++type) {
        for (u32 size = 1; size <= 5; ++size) {
            const u32 bytes = rsx_vertex_attrib_size(type, size);
            for (u32 at_end = 0; at_end <= 1; ++at_end) {
                assert(!mprotect(source_map + page, page, PROT_READ | PROT_WRITE));
                u8* source = source_map + page + (at_end ? page - bytes : 0);
                for (u32 i = 0; i < bytes; ++i) source[i] = (u8)random_word();
                assert(!mprotect(source_map + page, page, PROT_READ));
                rsx_vertex_fetch_plan plan = {0};
                rsx_vertex_layout_plan_init(&plan.layout, 1);
                plan.attr[0].desc.type = type;
                plan.attr[0].desc.size = size;
                plan.attr[0].desc.frequency = 0xffffffff;
                plan.attr[0].elem_size = bytes;
                plan.attr[0].stride = bytes;
                plan.attr[0].stable_base = source;
                rsx_vertex_ref refs[MAX_VERTICES];
                for (u32 i = 0; i < MAX_VERTICES; ++i) refs[i] = (rsx_vertex_ref){i, 0};
                u8 expected[MAX_VERTICES * 16];
                memset(output_map + page, 0xa5, page);
                u8* actual = output_map + page * 2 - sizeof expected;
                assert(rsx_vertex_fetch_all_ex(&plan, refs, MAX_VERTICES, expected, 0));
                assert(rsx_vertex_fetch_all_ex(&plan, refs, MAX_VERTICES, actual, 1));
                assert(!memcmp(expected, actual, sizeof expected));
                for (size_t i = 0; i < page - sizeof expected; ++i) assert(output_map[page + i] == 0xa5);
            }
        }
    }
    assert(!munmap(source_map, page * 3));
    assert(!munmap(output_map, page * 3));
    puts("PASS: 70 protected/read-only repeated sources, protected output ends and untouched prefixes");
}

int main(void)
{
    const u32 frequencies[] = {2, 3, 4, 7, 16, 63, 64, 65, 1024, 65535, 0x100000, 0xffffffff};
    const int modes[] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    const int original_rounding = fegetround();
    check_overlapping_source_output();
    check_protected_repetition();
    u8 source[SOURCE_BYTES + 1];
    u8 expected[OUTPUT_BYTES], actual[OUTPUT_BYTES];
    rsx_vertex_ref refs[MAX_VERTICES];
    unsigned cases = 0;
    for (u32 mode = 0; mode < 4; ++mode) {
        assert(!fesetround(modes[mode]));
        for (u32 trial = 0; trial < 16384; ++trial) {
            rsx_vertex_fetch_plan plan = {0};
            const u32 mask = trial < 16 ? 1u << trial : random_word() & 0xffff;
            rsx_vertex_layout_plan_init(&plan.layout, mask);
            const u32 count = 1 + random_word() % MAX_VERTICES;
            const u32 base = random_word();
            const u32 first = trial % 11 == 0 ? 0xffff0 : random_word() & 0xfffff;
            for (u32 i = 0; i < count; ++i) refs[i] = (rsx_vertex_ref){first + i - base, base};
            for (u32 i = 0; i < sizeof source; ++i) source[i] = (u8)random_word();
            for (u32 attr = 0; attr < 16; ++attr) {
                rsx_vertex_fetch_attr* fetch = &plan.attr[attr];
                fetch->desc.type = 1 + (trial + attr) % 7;
                fetch->desc.size = 1 + (trial / 7 + attr) % 5;
                fetch->desc.frequency = frequencies[(trial + attr) % 12];
                fetch->elem_size = rsx_vertex_attrib_size(fetch->desc.type, fetch->desc.size);
                fetch->stride = trial % 17 == 0 ? 0 : fetch->elem_size + trial % 4;
                const u32 frequency = fetch->desc.frequency;
                fetch->stable_first = first / frequency;
                fetch->stable_last = (first + count - 1) / frequency;
                fetch->stable_base = source + 1;
                for (u32 component = 0; component < 4; ++component)
                    fetch->default_value[component] = (float)(attr * 4 + component);
                if (trial % 13 == 0) plan.divider_mask |= 1u << attr;
                if (trial % 19 == 0) fetch->stable_base = NULL;
                if (trial % 23 == 0) fetch->stable_first++;
                if (trial % 29 == 0 && fetch->stable_last) fetch->stable_last--;
                if (trial % 31 == 0) fetch->desc.type = 0;
                if (trial % 37 == 0) fetch->desc.size = 0;
            }
            memset(expected, 0xa5, sizeof expected);
            memset(actual, 0xa5, sizeof actual);
            const int reference_ok = rsx_vertex_fetch_all_ex(&plan, refs, count, expected, 0);
            const int fast_ok = rsx_vertex_fetch_all_ex(&plan, refs, count, actual, 1);
            assert(reference_ok == fast_ok);
            assert(!memcmp(expected, actual, sizeof actual));
            ++cases;
        }
    }
    assert(!fesetround(original_rounding));
    printf("PASS: %u divided-fetch cases, all slots, four rounding modes, wrapped/base/modulo/missing/invalid fallbacks and poisoned outputs\n", cases);
}
