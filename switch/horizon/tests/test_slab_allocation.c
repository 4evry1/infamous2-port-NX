
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
typedef uint32_t Result;
typedef struct {void *backing, *alias;} hz_slab;
typedef struct {uintptr_t base; hz_slab *slabs;} hz_window;
static uint64_t s_committed_bytes, hz_mman_commit_cap = 0x70000000, hz_mman_stats[3];
static int s_process = 1;
#define Perm_Rw 3
#define R_SUCCEEDED(r) ((r) == 0)
#define R_FAILED(r) ((r) != 0)
static _Alignas(0x200000) unsigned char storage[0x201000];
static size_t available, requested_alignment;
static int failure_stage, allocated, mapped, freed, unmaps, map_calls;
struct mallinfo {size_t uordblks, fordblks, keepcost;};
static struct mallinfo mallinfo(void) {return (struct mallinfo){0x90000000, available, 4096};}
static void *test_memalign(size_t alignment, size_t size) {
    requested_alignment = alignment;
    /* The checked-in toolchain's newlib reserves size + alignment + bookkeeping. */
    if (size + alignment + 64 > available) return NULL;
    assert(!allocated);
    allocated = 1;
    return storage + 4096;
}
static void test_free(void *p) {assert(p == storage + 4096 && allocated && !mapped); allocated = 0; ++freed;}
static void virtmemLock(void) {}
static void virtmemUnlock(void) {}
static void *virtmemFindCodeMemory(size_t size, size_t alignment) {
    assert(size == 0x200000 && alignment == 0x200000);
    return failure_stage == 1 ? NULL : (void *)0x80000000;
}
static Result svcMapProcessCodeMemory(int proc, u64 dst, u64 src, u64 size) {
    assert(proc == 1 && dst == 0x80000000 && src == (uintptr_t)(storage + 4096));
    assert(!(src % 4096) && size == 0x200000);
    ++map_calls;
    if (failure_stage == 2) return 2;
    mapped = 1; return 0;
}
static Result svcSetProcessMemoryPermission(int proc, u64 dst, u64 size, int perm) {
    assert(proc == 1 && dst == 0x80000000 && size == 0x200000 && perm == 3 && mapped);
    return failure_stage == 3 ? 3 : 0;
}
static Result svcMapProcessMemory(void *dst, int proc, u64 src, u64 size) {
    assert((uintptr_t)dst == 0x100200000 && proc == 1 && src == 0x80000000 && size == 0x200000 && mapped);
    return failure_stage == 4 ? 4 : 0;
}
static Result svcUnmapProcessCodeMemory(int proc, u64 dst, u64 src, u64 size) {
    assert(proc == 1 && dst == 0x80000000 && src == (uintptr_t)(storage + 4096) && size == 0x200000 && mapped);
    mapped = 0; ++unmaps; return 0;
}
static void slab_map_failure_regions(void *backing, void *alias) { assert(backing == storage + 4096 && alias == (void *)0x80000000 && allocated && !mapped); }
#define memalign test_memalign
#define free test_free
#include "production.inc"
int main(void) {
    hz_slab slabs[2] = {0}; hz_window w = {0x100000000, slabs};
    available = 3 * 1024 * 1024;
    memset(storage, 0xAA, sizeof(storage));
    assert(slab_acquire(&w, 1));
    assert(requested_alignment == 4096 && slabs[1].backing == storage + 4096);
    assert(s_committed_bytes == 0x200000 && hz_mman_stats[0] == s_committed_bytes);
    for (size_t i = 4096; i < sizeof(storage); ++i) assert(storage[i] == 0);
    for (size_t i = 0; i < 4096; ++i) assert(storage[i] == 0xAA);
    allocated = mapped = 0; slabs[1] = (hz_slab){0}; s_committed_bytes = hz_mman_stats[0] = 0;
    for (int stage = 0; stage <= 4; ++stage) {
        failure_stage = stage; available = stage == 0 ? 1024 : 3 * 1024 * 1024;
        int old_free = freed, old_unmaps = unmaps;
        assert(!slab_acquire(&w, 1));
        assert(!allocated && !mapped && !slabs[1].backing && !slabs[1].alias);
        assert(s_committed_bytes == 0 && hz_mman_stats[0] == 0);
        assert(freed == old_free + (stage != 0));
        assert(unmaps == old_unmaps + (stage >= 3));
    }
    failure_stage = 0; available = 3 * 1024 * 1024;
    hz_mman_commit_cap = 0; int previous_calls = map_calls;
    assert(!slab_acquire(&w, 1) && map_calls == previous_calls && !allocated);
    hz_mman_commit_cap = 0x70000000;
    assert(slab_acquire(&w, 1));
    assert(hz_mman_stats[1] == 6 && s_committed_bytes == 0x200000);
    puts("PASS: constrained heap, zeroing, retry, cap, alias/map/permission cleanup");
}
