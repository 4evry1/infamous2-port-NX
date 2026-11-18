#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef uint8_t u8;
typedef uint64_t u64;
#include "span_hash_under_test.h"

static uint64_t random_state = UINT64_C(0x354bd092ef754813);
static uint64_t random_word(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

static int compare_hashes(const void* left, const void* right) {
    const u64 a = *(const u64*)left, b = *(const u64*)right;
    return (a > b) - (a < b);
}

static size_t distinct_byte_pairs(u64 (*hash)(const u8*, u64, u64), unsigned first_offset, unsigned second_offset) {
    u64 hashes[65536];
    u8 bytes[128] = {0};
    for (unsigned first = 0; first < 256; ++first) {
        for (unsigned second = 0; second < 256; ++second) {
            bytes[first_offset] = (u8)first;
            bytes[second_offset] = (u8)second;
            hashes[first * 256 + second] = hash(bytes, sizeof bytes, 123);
        }
    }
    qsort(hashes, 65536, sizeof hashes[0], compare_hashes);
    size_t distinct = 1;
    for (size_t i = 1; i < 65536; ++i) distinct += hashes[i] != hashes[i - 1];
    return distinct;
}

static void check_precomputed_equivalence(void) {
    unsigned char generated[XXH3_SECRET_DEFAULT_SIZE];
    XXH3_generateSecret_fromSeed(generated, RSX_VERTEX_HASH_SEED);
    assert(sizeof generated == sizeof rsx_vertex_hash_secret);
    assert(!memcmp(generated, rsx_vertex_hash_secret, sizeof generated));
    XXH3_generateSecret_fromSeed(generated, RSX_INDEX_HASH_SEED);
    assert(!memcmp(generated, rsx_index_hash_secret, sizeof generated));
    const u64 seeds[] = {RSX_VERTEX_HASH_SEED, RSX_INDEX_HASH_SEED, 0, 1, UINT64_MAX, UINT64_C(0x123456789abcdef)};
    u8 data[8208];
    for (size_t i = 0; i < sizeof data; ++i) data[i] = (u8)random_word();
    size_t cases = 0;
    for (size_t seed = 0; seed < sizeof seeds / sizeof *seeds; ++seed)
    for (size_t alignment = 0; alignment < 16; ++alignment)
    for (size_t n = 0; n <= 8192; ++n) {
        assert(eng_hash_span(data + alignment, n, seeds[seed]) ==
               XXH3_64bits_withSeed(data + alignment, n, seeds[seed]));
        ++cases;
    }
    printf("PASS: %zu exact seeded-hash equivalence cases; immutable parameters match pinned library\n", cases);
}

int main(void) {
    check_precomputed_equivalence();
    const unsigned pairs[][2] = {{7,15}, {3,11}, {0,8}, {7,8}, {0,15}, {7,71}, {63,127}};
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; ++i) {
        const size_t candidate_pairs = distinct_byte_pairs(eng_hash_span, pairs[i][0], pairs[i][1]);
        printf("Distinct byte pairs %u/%u: candidate=%zu of65536\n", pairs[i][0], pairs[i][1], candidate_pairs);
        fflush(stdout);
        assert(candidate_pairs == 65536);
    }
    const u64 seeds[] = {0, 1, UINT64_MAX, UINT64_C(0x458abc3401fe882d), RSX_VERTEX_HASH_SEED, RSX_INDEX_HASH_SEED};
    const size_t lengths[] = {1, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65,
                              127, 128, 129, 239, 240, 241, 255, 256, 257, 511, 1023, 1024, 1025, 4096};
    unsigned char data[8192];
    for (size_t i = 0; i < sizeof data; ++i) data[i] = (u8)random_word();
    size_t mutations = 0;
    for (size_t seed = 0; seed < sizeof seeds / sizeof seeds[0]; ++seed) {
        assert(eng_hash_span(NULL, 0, seeds[seed]) == eng_hash_span(data, 0, seeds[seed]));
        for (size_t alignment = 0; alignment < 16; ++alignment) {
            for (size_t j = 0; j < sizeof lengths / sizeof lengths[0]; ++j) {
                const size_t length = lengths[j];
                const u64 before = eng_hash_span(data + alignment, length, seeds[seed]);
                for (size_t offset = 0; offset < length; ++offset) {
                    for (unsigned bit = 0; bit < 8; ++bit) {
                        data[alignment + offset] ^= (u8)(1u << bit);
                        assert(eng_hash_span(data + alignment, length, seeds[seed]) != before);
                        data[alignment + offset] ^= (u8)(1u << bit);
                        ++mutations;
                    }
                }
            }
        }
    }
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    u8* pages = mmap(NULL, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(pages != MAP_FAILED);
    assert(mprotect(pages + page, page, PROT_READ | PROT_WRITE) == 0);
    memset(pages + page, 0x9d, page);
    assert(mprotect(pages + page, page, PROT_READ) == 0);
    const u64 guarded_seeds[] = {123, RSX_VERTEX_HASH_SEED, RSX_INDEX_HASH_SEED};
    for (size_t seed = 0; seed < sizeof guarded_seeds / sizeof *guarded_seeds; ++seed)
    for (size_t length = 0; length <= page; ++length) {
        const u8* end = pages + page * 2 - length;
        assert(eng_hash_span(end, length, guarded_seeds[seed]) ==
               XXH3_64bits_withSeed(end, length, guarded_seeds[seed]));
        assert(eng_hash_span(pages + page, length, guarded_seeds[seed]) ==
               XXH3_64bits_withSeed(pages + page, length, guarded_seeds[seed]));
    }
    assert(munmap(pages, page * 3) == 0);
    printf("PASS: %zu single-bit mutations; 16 alignments; 6 seeds; zero-length null equivalence; protected bounds\n", mutations);
}
