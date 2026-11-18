#include "vk_staging_copy.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

static void check(size_t size, size_t source_offset, size_t destination_offset)
{
    assert(size <= 1048576 && source_offset < 32 && destination_offset < 32);
    size_t capacity = size + 128;
    unsigned char* source = malloc(capacity);
    unsigned char* destination = malloc(capacity);
    unsigned char* expected = malloc(capacity);
    assert(source && destination && expected);
    for (size_t i = 0; i < capacity; ++i) source[i] = (unsigned char)(i * 37 + i / 13);
    memset(destination, 0xa5, capacity);
    memcpy(expected, destination, capacity);
    memcpy(expected + destination_offset, source + source_offset, size);
    vk_staging_copy(destination + destination_offset, source + source_offset, size);
    assert(!memcmp(destination, expected, capacity));
    for (size_t i = 0; i < capacity; ++i)
        assert(source[i] == (unsigned char)(i * 37 + i / 13));
    free(expected); free(destination); free(source);
}

int main(void)
{
    unsigned cases = 0;
    for (size_t size = 0; size <= 256; ++size)
        for (size_t src = 0; src < 32; ++src)
            for (size_t dst = 0; dst < 32; ++dst) { check(size, src, dst); ++cases; }
    const size_t large[] = {511, 512, 513, 1023, 1024, 1025, 4095, 4096, 4097, 65535, 65536, 65537, 1048576};
    for (size_t i = 0; i < sizeof large / sizeof large[0]; ++i)
        for (size_t off = 0; off < 32; ++off) { check(large[i], off, 31 - off); ++cases; }
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char* src = mmap(NULL, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned char* dst = mmap(NULL, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(src != MAP_FAILED && dst != MAP_FAILED);
    assert(!mprotect(src + page, page, PROT_READ | PROT_WRITE));
    assert(!mprotect(dst + page, page, PROT_READ | PROT_WRITE));
    for (size_t i = 0; i < page; ++i) src[page + i] = (unsigned char)(i * 19);
    assert(!mprotect(src + page, page, PROT_READ));
    for (size_t size = 0; size <= page; ++size) {
        memset(dst + page, 0xa5, page);
        vk_staging_copy(dst + page * 2 - size, src + page * 2 - size, size);
        assert(!memcmp(dst + page * 2 - size, src + page * 2 - size, size));
        for (size_t i = 0; i < page - size; ++i) assert(dst[page + i] == 0xa5);
        ++cases;
    }
    assert(!munmap(src, page * 3)); assert(!munmap(dst, page * 3));
    printf("%u staging copy cases passed\n", cases);
}
