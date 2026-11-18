#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
static const u8* memory;
static u32 available;
static unsigned calls, failures;
static const u8* eng_guest_ptr(void* user, u32 location, u32 offset, u32 size) {
    (void)user;
    ++calls;
    if (location != 1 || offset != 7 || size > available) return NULL;
    return memory;
}
static u32 hint_span, hints;
static void record_hint(const u8* address)
{
    assert(hint_span >= 64 && address >= memory && address <= memory + hint_span - 64);
    assert((size_t)(address - memory) % 2048 == 0);
    ++hints;
}
#include "production.inc"

/* Original chained digest. Exercise overlapping and exactly coincident tails,
 * unaligned bases and full-sized mip spans while checking every issued hint. */
static void verify_sparse_compatibility(void)
{
    enum { LIMIT = (2 << 20) + 127 };
    u8 *buf = malloc(LIMIT + 16);
    assert(buf);
    for (u32 i = 0; i < LIMIT + 16; ++i) buf[i] = (u8)(i * 71u + (i >> 9));
    const u32 boundaries[] = {64, 2048, 8192, 16384, 18432, 65536, 262144, 2 << 20};
    for (u32 align = 0; align < 16; ++align) {
        memory = buf + align;
        for (unsigned j = 0; j < sizeof boundaries / sizeof *boundaries; ++j) {
            for (int delta = -63; delta <= 65; ++delta) {
                available = hint_span = boundaries[j] + delta;
                int readable = 0;
                u32 tail = available > 64 ? available - 64 : 0;
                u64 ref = XXH3_64bits(memory, available > 64 ? 64 : available);
                for (u32 start = 2048; start <= tail; start += 2048)
                    ref = XXH3_64bits_withSeed(memory + start, 64, ref);
                if (tail % 2048) ref = XXH3_64bits_withSeed(memory + tail, 64, ref);
                assert(eng_texture_sparse_hash(1, 7, available, &readable) == ref && readable);
            }
        }
    }
    assert(hints > 0);
    free(buf);
}
static u64 hash(int sparse, u32 size) {
    hint_span = size;
    int readable = 0;
    const u64 h = sparse ? eng_texture_sparse_hash(1, 7, size, &readable)
                         : eng_texture_content_hash(1, 7, size, &readable);
    assert(readable == 1);
    return h;
}
static void changed(u64 a, u64 b, const char* label) {
    if (a == b) { fprintf(stderr, "FAIL: %s collision\n", label); ++failures; }
}
int main(void) {
    (void)record_hint;
    verify_sparse_compatibility();
    u8 data[8192 + 16] = {0};
    memory = data; available = 8192;
    const u32 legacy_sizes[] = {1, 7, 63, 64, 65, 127, 128, 129, 2048, 2112, 4097};
    const u64 legacy_expected[] = {0x18f37141ff5d84c9ull, 0x56cdd6b485c36116ull, 0xbc1b28276ece92aeull, 0x635c6e2fdee58cf0ull, 0xbb9bee57c00e1429ull, 0x6c759afe19d51f8eull, 0x44572e65ac980b50ull, 0xb82fa9c4465c1789ull, 0x43cfa8df15349910ull, 0x41991112c1a16a30ull, 0x611dfcb7739f06c9ull};
    for (u32 i = 0; i < available; ++i) data[i] = (u8)(i * 37u + 3u);
    for (u32 i = 0; i < sizeof legacy_sizes / sizeof legacy_sizes[0]; ++i) {
        int readable = 0;
        assert(eng_texture_pack_hash(1, 7, legacy_sizes[i], &readable) == legacy_expected[i]);
        assert(readable == 1);
    }
    memset(data, 0, sizeof data);
    u64 before = hash(0, 128);
    data[7] ^= 128; data[71] ^= 128;
    changed(before, hash(0, 128), "full paired high bits");
    memset(data, 0, sizeof data);
    before = hash(1, 128);
    data[7] ^= 128; data[15] ^= 128;
    changed(before, hash(1, 128), "sparse paired high bits");
    memset(data, 0, sizeof data);
    before = hash(1, 64); data[7] ^= 128;
    changed(before, hash(1, 64), "sparse duplicate-tail single high bit");
    if (failures) return 1;
    unsigned mutations = 0;
    for (u32 align = 0; align < 16; ++align) {
        memory = data + align;
        for (u32 i = 0; i < available; ++i) data[align + i] = (u8)(i * 37u + align);
        const u64 full = hash(0, available), sparse = hash(1, available);
        for (u32 i = 0; i < available; ++i)
        for (u32 bit = 0; bit < 8; ++bit) {
            data[align + i] ^= 1u << bit;
            changed(full, hash(0, available), "full single bit");
            const u64 sampled = hash(1, available);
            if (i % 2048 < 64 || i >= available - 64)
                changed(sparse, sampled, "sampled single bit");
            else assert(sparse == sampled);
            data[align + i] ^= 1u << bit;
            ++mutations;
        }
    }
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    u8* pages = mmap(NULL, page * 4, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(pages != MAP_FAILED);
    assert(mprotect(pages + page, page * 2, PROT_READ | PROT_WRITE) == 0);
    memset(pages + page, 0xa5, page * 2);
    const u32 sizes[] = {1, 2, 3, 7, 8, 15, 16, 31, 32, 63, 64, 65, 127, 128,
                         129, 2047, 2048, 2049, 2111, 2112, 2113, 4095, 4096, 4097};
    for (u32 i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
        available = sizes[i]; memory = pages + page * 3 - available;
        before = hash(1, available);
        u8* tail = (u8*)memory + available - 1;
        *tail ^= 1; changed(before, hash(1, available), "tail bit"); *tail ^= 1;
        (void)hash(0, available);
        for (int sparse = 0; sparse < 2; ++sparse) {
            int readable = 1;
            u64 h = sparse ? eng_texture_sparse_hash(1, 7, available + 1, &readable)
                           : eng_texture_content_hash(1, 7, available + 1, &readable);
            assert(h == 0 && !readable);
        }
    }
    for (int sparse = 0; sparse < 2; ++sparse) {
        int readable = 1;
        unsigned previous = calls;
        u64 h = sparse ? eng_texture_sparse_hash(1, 7, 0, &readable)
                       : eng_texture_content_hash(1, 7, 0, &readable);
        assert(h == 0 && !readable && calls == previous);
    }
    assert(munmap(pages, page * 4) == 0);
    assert(!failures);
    printf("PASS: %u bit mutations; legacy identities, sampled/unsampled parity, collisions, alignment, protected ends, denied and zero mappings\n", mutations);
}
