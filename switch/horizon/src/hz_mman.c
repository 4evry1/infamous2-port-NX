/* Horizon layer: mmap / mprotect / munmap / madvise for the ps3recomp engine.
 *
 * What the engine asks for on a POSIX host:
 *   - one huge anonymous MAP_NORESERVE mapping (the 4 GiB guest window, plus slack so it can be
 *     trimmed to a 4 GiB-aligned base), which Linux backs lazily, page by page, on first touch;
 *   - mprotect(PROT_READ|PROT_WRITE) as "commit" (vm_commit), mprotect(PROT_NONE) for guard pages;
 *   - a few small anonymous mappings.
 *
 * Horizon has no lazy backing and no overcommit, so a huge mapping becomes an address-space
 * reservation with nothing behind it, and memory is put behind it in 2 MiB slabs when a range is
 * committed. The slab sequence is the one proven on the console in the inFAMOUS 1 runtime
 * (inFAMOUS-native-switch/native/adapters/guest_space_flat_horizon.c): heap block -> code alias
 * (svcMapProcessCodeMemory) -> RW -> mapped at its place in the window (svcMapProcessMemory).
 *
 * NOT DONE YET: commit on first touch. The engine's inline guest accesses assume every address in
 * the window is readable and writable; a touch of a slab nobody committed faults here. Either the
 * regions the game uses are committed up front (hz_mman_commit), or a fault handler commits them.
 */
#include <sys/mman.h>
#include <hz_layer.h>
#include <switch.h>
#include <errno.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    HZ_SLAB_BYTES = 0x200000,
    HZ_BACKING_ALIGNMENT = 0x1000,
    HZ_MAX_WINDOWS = 4,
    HZ_MAX_SLABS = 8192,            /* 16 GiB of window address space in 2 MiB slabs */
    HZ_SMALL_MAX = 64
};
#define HZ_WINDOW_MIN_BYTES 0x40000000ull   /* an anonymous mapping this large is a reservation */

typedef struct hz_slab { void *backing, *alias; } hz_slab;
typedef struct hz_window {
    uintptr_t base;
    uint64_t size;
    VirtmemReservation *reservation;
    hz_slab *slabs;
    uint32_t slab_count;
} hz_window;

static Mutex s_lock;
static hz_window s_windows[HZ_MAX_WINDOWS];
static struct { void *p; size_t len; } s_small[HZ_SMALL_MAX];
static Handle s_process = INVALID_HANDLE;
static uint64_t s_committed_bytes;
/* The most guest memory that may be committed. A Switch application has about 3.2 GiB in all. */
uint64_t hz_mman_commit_cap = 0x70000000ull;
/* For the log: committed bytes, failed slab commits, last kernel result of a failure. */
uint64_t hz_mman_stats[3];

/* ---- a real handle of this process (see guest_space_flat_horizon.c) ------------------------ */
typedef struct handle_request { Handle server, received; } handle_request;
static void receive_process_handle(void *argument)
{
    handle_request *request = argument;
    void *tls = armGetTls();
    s32 index = 0;
    hipcMakeRequest(tls, (HipcMetadata){0});
    if (R_SUCCEEDED(svcReplyAndReceive(&index, &request->server, 1, INVALID_HANDLE, 5000000000ULL))) {
        const HipcParsedRequest message = hipcParseRequest(tls);
        if (message.meta.num_copy_handles == 1) request->received = message.data.copy_handles[0];
    }
    svcCloseHandle(request->server);
}
static Handle own_process_handle(void)
{
    handle_request request = {INVALID_HANDLE, INVALID_HANDLE};
    Handle client = INVALID_HANDLE;
    Thread receiver;
    const Handle supplied = envGetOwnProcessHandle();
    if (supplied != INVALID_HANDLE) return supplied;
    if (R_FAILED(svcCreateSession(&request.server, &client, 0, 0))) return INVALID_HANDLE;
    memset(&receiver, 0, sizeof receiver);
    if (R_FAILED(threadCreate(&receiver, receive_process_handle, &request, NULL, 0x4000, 0x20, 0))) {
        svcCloseHandle(request.server); svcCloseHandle(client);
        return INVALID_HANDLE;
    }
    if (R_FAILED(threadStart(&receiver))) {
        threadClose(&receiver); svcCloseHandle(request.server); svcCloseHandle(client);
        return INVALID_HANDLE;
    }
    {
        HipcMetadata metadata = {0};
        metadata.num_copy_handles = 1;
        hipcMakeRequest(armGetTls(), metadata).copy_handles[0] = CUR_PROCESS_HANDLE;
    }
    svcSendSyncRequest(client);
    svcCloseHandle(client);
    threadWaitForExit(&receiver);
    threadClose(&receiver);
    return request.received;
}

/* ---- windows and slabs ---------------------------------------------------------------------- */
static hz_window *window_of(uintptr_t address)
{
    for (int i = 0; i < HZ_MAX_WINDOWS; ++i)
        if (s_windows[i].base && address >= s_windows[i].base && address - s_windows[i].base < s_windows[i].size)
            return &s_windows[i];
    return NULL;
}
static void slab_map_failure_regions(void *backing, void *alias)
{
    const uintptr_t addresses[] = {(uintptr_t)backing, (uintptr_t)backing + HZ_SLAB_BYTES - 1,
                                   (uintptr_t)alias, (uintptr_t)alias + HZ_SLAB_BYTES - 1};
    for (unsigned i = 0; i < 4; ++i) {
        MemoryInfo memory = {0};
        u32 page = 0;
        const Result query = svcQueryMemory(&memory, &page, addresses[i]);
        fprintf(stderr, "[hz-memory-map] endpoint=%u address=0x%llx query=0x%x region=0x%llx+0x%llx type=%u attr=0x%x perm=0x%x\n",
                i, (unsigned long long)addresses[i], query, (unsigned long long)memory.addr,
                (unsigned long long)memory.size, memory.type, memory.attr, memory.perm);
    }
}

static bool slab_acquire(hz_window *w, uint32_t index)
{
    void *backing, *alias;
    Result result = 0;
    const char *stage = "commit_cap";
    if (s_committed_bytes + HZ_SLAB_BYTES > hz_mman_commit_cap) goto failed;
    stage = "aligned_heap";
    /* The mapping syscalls require page alignment. Slab alignment makes newlib
     * request an extra 2 MiB contiguous span and fragments the shared heap. */
    backing = memalign(HZ_BACKING_ALIGNMENT, HZ_SLAB_BYTES);
    if (!backing) goto failed;
    memset(backing, 0, HZ_SLAB_BYTES);
    stage = "code_alias";
    virtmemLock();
    alias = virtmemFindCodeMemory(HZ_SLAB_BYTES, HZ_SLAB_BYTES);
    if (alias) {
        stage = "map_code";
        result = svcMapProcessCodeMemory(s_process, (u64)(uintptr_t)alias, (u64)(uintptr_t)backing, HZ_SLAB_BYTES);
    }
    virtmemUnlock();
    if (!alias || R_FAILED(result)) {
        if (alias && hz_mman_stats[1] < 4) slab_map_failure_regions(backing, alias);
        free(backing);
        goto failed;
    }
    stage = "permission";
    result = svcSetProcessMemoryPermission(s_process, (u64)(uintptr_t)alias, HZ_SLAB_BYTES, Perm_Rw);
    if (R_SUCCEEDED(result)) {
        stage = "map_process";
        result = svcMapProcessMemory((void *)(w->base + (uintptr_t)index * HZ_SLAB_BYTES), s_process,
                                     (u64)(uintptr_t)alias, HZ_SLAB_BYTES);
    }
    if (R_FAILED(result)) {
        svcUnmapProcessCodeMemory(s_process, (u64)(uintptr_t)alias, (u64)(uintptr_t)backing, HZ_SLAB_BYTES);
        free(backing);
        goto failed;
    }
    w->slabs[index].backing = backing;
    w->slabs[index].alias = alias;
    s_committed_bytes += HZ_SLAB_BYTES;
    hz_mman_stats[0] = s_committed_bytes;
    return true;
failed:
    ++hz_mman_stats[1];
    hz_mman_stats[2] = result;
    const uint64_t failures = hz_mman_stats[1];
    if (failures <= 4 || !(failures & (failures - 1))) {
        const struct mallinfo heap = mallinfo();
        fprintf(stderr, "[hz-memory] slab failure #%llu stage=%s result=0x%x slab=%u committed=%llu cap=%llu heap_used=%zu heap_free=%zu heap_top=%zu\n",
                (unsigned long long)failures, stage, result, index,
                (unsigned long long)s_committed_bytes, (unsigned long long)hz_mman_commit_cap,
                heap.uordblks, heap.fordblks, heap.keepcost);
    }
    return false;
}
/* Put memory behind [address, address + length) of a window. Already committed slabs stay as
 * they are (their contents too). 0, or -1 with errno = ENOMEM. */
int hz_mman_commit(void *address, size_t length)
{
    int rc = 0;
    hz_window *w;
    if (!length) return 0;
    mutexLock(&s_lock);
    w = window_of((uintptr_t)address);
    if (!w || (uintptr_t)address - w->base + length > w->size) { mutexUnlock(&s_lock); errno = EINVAL; return -1; }
    {
        const uint64_t first = ((uintptr_t)address - w->base) / HZ_SLAB_BYTES;
        const uint64_t last = ((uintptr_t)address - w->base + length - 1) / HZ_SLAB_BYTES;
        for (uint64_t slab = first; slab <= last; ++slab)
            if (!w->slabs[slab].backing && !slab_acquire(w, (uint32_t)slab)) { errno = ENOMEM; rc = -1; break; }
    }
    mutexUnlock(&s_lock);
    return rc;
}
/* Is this address in a window, and has it memory behind it? (for a fault handler and VirtualQuery) */
int hz_mman_query(const void *address, int *committed)
{
    hz_window *w;
    int found = 0;
    mutexLock(&s_lock);
    w = window_of((uintptr_t)address);
    if (w) {
        found = 1;
        if (committed) *committed = w->slabs[((uintptr_t)address - w->base) / HZ_SLAB_BYTES].backing != NULL;
    }
    mutexUnlock(&s_lock);
    return found;
}

/* Stand-in for commit on first touch: guest ranges (offsets from the window's first 4 GiB-aligned
 * address, which is where the engine puts guest address 0) that get memory as soon as a window of
 * 4 GiB or more is reserved. The Switch entry (hz_entry.c) fills the table before main() runs. */
struct hz_precommit_range hz_mman_precommit[HZ_PRECOMMIT_MAX];
uint64_t hz_mman_precommit_failed;   /* how many table entries could not be committed */
static void hz_mman_precommit_window(uintptr_t base, uint64_t size)
{
    const uintptr_t guest0 = (base + 0xFFFFFFFFull) & ~(uintptr_t)0xFFFFFFFFull;
    if (size < 0x100000000ull) return;
    for (int i = 0; i < HZ_PRECOMMIT_MAX; ++i) {
        const struct hz_precommit_range r = hz_mman_precommit[i];
        if (!r.length) continue;
        if (guest0 - base + r.guest + r.length > size ||
            hz_mman_commit((void *)(guest0 + r.guest), r.length) != 0)
            ++hz_mman_precommit_failed;
    }
}

static void *reserve_window(size_t length)
{
    hz_window *w = NULL;
    const uint64_t size = (length + HZ_SLAB_BYTES - 1) & ~(uint64_t)(HZ_SLAB_BYTES - 1);
    if (size / HZ_SLAB_BYTES > HZ_MAX_SLABS) { errno = ENOMEM; return MAP_FAILED; }
    if (s_process == INVALID_HANDLE) s_process = own_process_handle();   /* before the virtmem lock */
    if (s_process == INVALID_HANDLE) { errno = ENOMEM; return MAP_FAILED; }
    mutexLock(&s_lock);
    for (int i = 0; i < HZ_MAX_WINDOWS && !w; ++i) if (!s_windows[i].base) w = &s_windows[i];
    if (w) {
        w->slab_count = (uint32_t)(size / HZ_SLAB_BYTES);
        w->slabs = calloc(w->slab_count, sizeof *w->slabs);
        if (w->slabs) {
            virtmemLock();
            w->base = (uintptr_t)virtmemFindAslr(size, HZ_SLAB_BYTES);
            if (w->base) w->reservation = virtmemAddReservation((void *)w->base, size);
            virtmemUnlock();
        }
        if (!w->slabs || !w->base || !w->reservation) {
            free(w->slabs);
            memset(w, 0, sizeof *w);
            w = NULL;
        } else {
            w->size = size;
        }
    }
    mutexUnlock(&s_lock);
    if (!w) { errno = ENOMEM; return MAP_FAILED; }
    hz_mman_precommit_window(w->base, size);
    return (void *)w->base;
}

/* ---- the POSIX calls ------------------------------------------------------------------------ */
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    (void)off;
    if (!len || fd != -1 || !(flags & MAP_ANONYMOUS)) { errno = EINVAL; return MAP_FAILED; }
    if (addr && window_of((uintptr_t)addr)) {
        /* a mapping placed inside a window (MAP_FIXED): memory behind that range, zeroed */
        if (prot != PROT_NONE) {
            if (hz_mman_commit(addr, len) != 0) return MAP_FAILED;
            memset(addr, 0, len);
        }
        return addr;
    }
    if (flags & MAP_FIXED) { errno = ENOMEM; return MAP_FAILED; }
    if (len >= HZ_WINDOW_MIN_BYTES) return reserve_window(len);
    {
        const size_t size = (len + 0xFFF) & ~(size_t)0xFFF;
        void *p = memalign(0x1000, size);
        bool recorded = false;
        if (!p) { errno = ENOMEM; return MAP_FAILED; }
        memset(p, 0, size);
        mutexLock(&s_lock);
        for (int i = 0; i < HZ_SMALL_MAX && !recorded; ++i)
            if (!s_small[i].p) { s_small[i].p = p; s_small[i].len = size; recorded = true; }
        mutexUnlock(&s_lock);
        if (!recorded) { free(p); errno = ENOMEM; return MAP_FAILED; }   /* table full */
        return p;
    }
}
int munmap(void *addr, size_t len)
{
    (void)len;
    /* Inside a window: the engine trims the slack around its 4 GiB-aligned base. The reservation
     * cannot be split, and nothing is behind the trimmed part, so there is nothing to give back. */
    if (window_of((uintptr_t)addr)) return 0;
    mutexLock(&s_lock);
    for (int i = 0; i < HZ_SMALL_MAX; ++i)
        if (s_small[i].p == addr) {
            s_small[i].p = NULL; s_small[i].len = 0;
            mutexUnlock(&s_lock);
            free(addr);
            return 0;
        }
    mutexUnlock(&s_lock);
    errno = EINVAL;
    return -1;
}
int mprotect(void *addr, size_t len, int prot)
{
    /* In a window, read/write means "commit" (vm_commit). PROT_NONE is a guard page: it would
     * need a 4 KiB hole in a 2 MiB slab, which this backend cannot make, so the guard is lost.
     * Outside a window (heap blocks from the small-mapping path) protections are not applied. */
    if (prot != PROT_NONE && window_of((uintptr_t)addr)) return hz_mman_commit(addr, len);
    return 0;
}
int madvise(void *addr, size_t len, int advice)
{
    (void)addr; (void)len; (void)advice;
    return 0;   /* MADV_DONTNEED would zero the range on Linux; callers here memset on reuse */
}
