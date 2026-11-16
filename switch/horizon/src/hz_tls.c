/* Horizon layer: thread-local storage for the clang-compiled objects, and host stack sizes.
 *
 * devkitA64's GCC reads the thread pointer through __aarch64_read_tp() (-mtp=soft), which libnx
 * answers from its per-thread block behind TPIDRRO_EL0. clang has no such mode: its code reads
 * TPIDR_EL0 directly. Horizon leaves TPIDR_EL0 to the program and keeps it per thread, so every
 * thread sets it once, before any clang-compiled code runs, to the value libnx would have given.
 * The TLS layout itself (16-byte TCB, then the module's block) is the linker's and is the same
 * for both compilers.
 *
 * Every thread is caught at libnx's threadCreate (linked with --wrap=threadCreate): newlib's
 * pthread_create and SDL both come through it. The main thread is set in a constructor that
 * runs before the other static constructors. */
#include <switch.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__aarch64_read_tp(void);

static inline void hz_tls_adopt(void)
{
    const uintptr_t tp = (uintptr_t)__aarch64_read_tp();
    __asm__ volatile("msr tpidr_el0, %0" : : "r"(tp));
}
__attribute__((constructor(101))) static void hz_tls_main_thread(void) { hz_tls_adopt(); }

typedef struct hz_thread_start { ThreadFunc entry; void *arg; } hz_thread_start;
static void hz_thread_entry(void *raw)
{
    const hz_thread_start start = *(hz_thread_start *)raw;
    free(raw);
    hz_tls_adopt();
    start.entry(start.arg);
}
Result __real_threadCreate(Thread *t, ThreadFunc entry, void *arg, void *stack_mem, size_t stack_sz, int prio, int cpuid);
/* Cores. A thread created with "the default core" (-2, what newlib's pthread_create passes) runs
 * on the process's one default core and stays there: every guest thread, SPU thread and the RSX
 * thread would share a single A57. Such threads are handed out round-robin over the cores this
 * process is allowed (3 from the homebrew menu, 4 through a forwarder that unlocks core 3).
 * hz_core_mode: 1 = round-robin (default), 0 = leave libnx's default. */
int hz_core_mode = 2;
uint64_t hz_core_mask;          /* the allowed cores, for the status screen */
uint64_t hz_core_threads[4];    /* threads started per core */
static int hz_next_core(void)
{
    static uint32_t counter;
    u64 mask = 0;
    int cores[4], count = 0;
    if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || !mask) return -2;
    hz_core_mask = mask;
    for (int core = 0; core < 4; ++core) if (mask & (1ull << core)) cores[count++] = core;
    if (!count) return -2;
    return cores[__atomic_fetch_add(&counter, 1, __ATOMIC_RELAXED) % (uint32_t)count];
}
/* Scheduling, as measured on this console by the inFAMOUS 1 runtime (its Decisions 80-89):
 *   - Horizon time-slices threads only at priority 59, and only on cores 0-2. At any other
 *     priority a thread keeps its core until it blocks, so the engine's spin waits (written for
 *     "some other core will run the thread I wait for") stall whole cores when there are more
 *     runnable threads than cores. The game has about 30.
 *   - a thread pinned to one core cannot use another that is idle.
 * hz_core_mode 2 (default): every default-core thread gets priority hz_thread_priority (59), a
 * preferred core dealt round-robin over the time-sliced cores, and leave to run on any allowed
 * core (hz_affinity_mask, 0 = all the process may use; core 3 comes with a forwarder).
 * hz_core_mode 1: pinned round-robin over the allowed cores, priority untouched (hz-0.3..0.7).
 * hz_core_mode 0: libnx's default. */
enum { HZ_THREAD_SLOTS = 160 };
static Handle hz_thread_handles[HZ_THREAD_SLOTS];
static uintptr_t hz_thread_entries[HZ_THREAD_SLOTS];
static uint32_t hz_thread_count;
/* Where every thread this process started is right now: pc, lr and sp of each (the thread is
 * paused for the read and resumed). Called by the status loop when the game has printed nothing
 * for a while: a hang shows as the same few addresses, which the saved ELF turns into function
 * names. Threads that have ended are skipped (their handle no longer answers). */
void hz_thread_report(void (*line)(const char *format, ...))
{
    const uint32_t count = hz_thread_count < HZ_THREAD_SLOTS ? hz_thread_count : HZ_THREAD_SLOTS;
    for (uint32_t i = 0; i < count; ++i) {
        ThreadContext ctx;
        const Handle handle = hz_thread_handles[i];
        if (R_FAILED(svcSetThreadActivity(handle, ThreadActivity_Paused))) continue;
        if (R_SUCCEEDED(svcGetThreadContext3(&ctx, handle)))
            line("[hz] thread %3u pc %016lx lr %016lx sp %016lx x0 %016lx x1 %016lx\n", (unsigned)i,
                 (unsigned long)ctx.pc.x, (unsigned long)ctx.lr, (unsigned long)ctx.sp,
                 (unsigned long)ctx.cpu_gprs[0].x, (unsigned long)ctx.cpu_gprs[1].x);
        svcSetThreadActivity(handle, ThreadActivity_Runnable);
    }
}
/* A sampling profile of the whole process: every 1/rate s each thread is paused, its pc and lr
 * read, and resumed. Samples go to a file as 24-byte records {u32 thread, u32 0, u64 pc, u64 lr}
 * after a 32-byte header {"HZPROF1\0", u64 code_ref, u64 rate, u64 threads}; code_ref is the
 * run-time address of a known function, so switch/tools/hz_profile.py can turn addresses into names
 * with the saved ELF. Runs on the calling thread for `seconds`. Returns the sample count. */
uint64_t hz_profile_run(const char *path, unsigned seconds, unsigned rate, uintptr_t code_ref)
{
    struct { uint32_t thread, zero; uint64_t pc, lr; } *samples;
    const uint32_t count = hz_thread_count < HZ_THREAD_SLOTS ? hz_thread_count : HZ_THREAD_SLOTS;
    const uint64_t capacity = (uint64_t)seconds * rate * (count ? count : 1);
    const uint64_t period = 1000000000ull / (rate ? rate : 100);
    uint64_t used = 0, header[4];
    FILE *f;
    samples = malloc(capacity * sizeof *samples);
    if (!samples) return 0;
    for (uint64_t tick = 0; tick < (uint64_t)seconds * rate; ++tick) {
        for (uint32_t i = 0; i < count && used < capacity; ++i) {
            ThreadContext ctx;
            const Handle handle = hz_thread_handles[i];
            if (R_FAILED(svcSetThreadActivity(handle, ThreadActivity_Paused))) continue;
            if (R_SUCCEEDED(svcGetThreadContext3(&ctx, handle))) {
                samples[used].thread = i; samples[used].zero = 0;
                samples[used].pc = ctx.pc.x; samples[used].lr = ctx.lr;
                ++used;
            }
            svcSetThreadActivity(handle, ThreadActivity_Runnable);
        }
        svcSleepThread(period);
    }
    f = fopen(path, "wb");
    if (f) {
        memcpy(&header[0], "HZPROF1", 8);
        header[1] = (uint64_t)code_ref; header[2] = rate; header[3] = count;
        fwrite(header, sizeof header, 1, f);
        fwrite(samples, sizeof *samples, used, f);
        fclose(f);
    }
    free(samples);
    return f ? used : 0;
}
int hz_thread_priority = 59;
uint64_t hz_affinity_mask;
/* Priority by thread role, called by the engine on each thread as it starts (host_place.c,
 * ps3_host_place_self). Every thread starts at hz_thread_priority (59), where Horizon time-slices
 * every 10 ms; a thread at 58 takes a core as soon as it wakes. The main PPU thread polls its SPU
 * jobs with usleep(3) and the RSX thread waits on the GPU, so both used to lose up to 10 ms per
 * wake to busy SPU workers (run 24, hz-0.25: the main thread 44-58% asleep, the frame 200+ ms).
 * HZ_PRIO_MAIN / HZ_PRIO_RSX / HZ_PRIO_SPU / HZ_PRIO_PPU / HZ_PRIO_OTHER = 0..63 (lower runs
 * first); unset = main and RSX 58, the rest unchanged. Roles: host_place.h (0 main .. 4 other). */
/* The engine's audio mixer thread (cellAudio.c audio_mix_thread_func, hz-0.34) asks for this at its start:
 * priority HZ_PRIO_AUDIO (default 57), ahead of the main / RSX threads (58) and everything else (59), so the
 * mixer runs when its block is due. It sleeps between blocks, so it takes little CPU. */
void hz_audio_thread_boost(void)
{
    const char *e = getenv("HZ_PRIO_AUDIO");
    const int prio = e && *e ? atoi(e) : 57;
    fprintf(stderr, "[hz] audio mixer thread: priority %d\n", prio);
    if (prio >= 0 && prio <= 63) svcSetThreadPriority(CUR_THREAD_HANDLE, prio);
}
/* Priority by guest thread name, called by the engine on each guest PPU thread as it starts
 * (sys_ppu_thread.c ppu_host_thread_proc, hz-0.37). HZ_PRIO_NAMES = "name:prio,name:prio" (exact
 * names, prio 0..63); unset = "AK::EventManager:57": the Wwise audio thread, which renders the next
 * blocks into the cellAudio ring. Run 36: the ring averaged 2.2 of 16 blocks filled and was empty in
 * 33% of samples while that thread waited at 59 behind busy SPU workers, so the sound chopped. */
void hz_ppu_thread_named(const char *name)
{
    const char *e = getenv("HZ_PRIO_NAMES");
    /* hz-0.41: also called for each SPU thread with its group's name (lv2_register.c spu_exec_thread_proc).
     * "RADG5" is the game's sound mixer: a dedicated SPU group whose one thread runs only the relocatable
     * audio modules (relocmod_0090ce80, 008f7300, 00905b00; console run 41: busy in about 80% of its
     * samples, at 59 behind the SPU workers). 58 puts it with the main and RSX threads, ahead of the
     * workers; not 57, as it is busy enough to keep the main thread off a core. */
    /* hz-0.43: RADG5 is Bink, not audio (run 42: worse with it at 58). The relocmod audio jobs run in the
     * one-SPU SPURS instance, named "CellSpursKernelGroup/1" by lv2_register.c (the title's SPU count after
     * the slash). 58 as for RADG5: busy ~78%, so not ahead of the main thread. */
    const char *p = e && *e ? e : "AK::EventManager:57,CellSpursKernelGroup/1:58";
    const size_t n = name ? strlen(name) : 0;
    if (!n || hz_core_mode != 2) return;
    while (*p) {
        const char *comma = strchr(p, ',');
        const char *colon;
        if (!comma) comma = p + strlen(p);
        /* The priority follows the LAST colon of the entry: names such as "AK::EventManager" hold colons. */
        for (colon = comma; colon > p && colon[-1] != ':'; --colon) {}
        --colon;
        if (colon >= p && *colon == ':' && (size_t)(colon - p) == n && !memcmp(p, name, n)) {
            const int prio = atoi(colon + 1);
            fprintf(stderr, "[hz] guest thread \"%s\": priority %d\n", name, prio);
            if (prio >= 0 && prio <= 63) svcSetThreadPriority(CUR_THREAD_HANDLE, prio);
            return;
        }
        p = *comma ? comma + 1 : comma;
    }
}
uint64_t hz_place_counts[5];
void hz_place_self(int role)
{
    static const char *const names[5] = {"HZ_PRIO_MAIN", "HZ_PRIO_RSX", "HZ_PRIO_SPU", "HZ_PRIO_PPU", "HZ_PRIO_OTHER"};
    static const int defaults[5] = {58, 58, -1, -1, -1};
    const char *e;
    int prio;
    if (role < 0 || role > 4 || hz_core_mode != 2) return;
    e = getenv(names[role]);
    prio = e && *e ? atoi(e) : defaults[role];
    if (__atomic_fetch_add(&hz_place_counts[role], 1, __ATOMIC_RELAXED) == 0)
        fprintf(stderr, "[hz] thread role %s: priority %d%s\n", names[role] + 8, prio, prio < 0 ? " (unchanged, 59)" : "");
    if (prio >= 0 && prio <= 63) svcSetThreadPriority(CUR_THREAD_HANDLE, prio);
}
uint64_t hz_sched_failures;     /* svcSetThreadCoreMask refusals */
Result __wrap_threadCreate(Thread *t, ThreadFunc entry, void *arg, void *stack_mem, size_t stack_sz, int prio, int cpuid)
{
    Result rc;
    const int was_default = cpuid == -2;
    u64 allowed = 0;
    hz_thread_start *start = malloc(sizeof *start);
    if (!start) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    hz_core_mask = allowed;
    if (was_default && hz_core_mode == 1) cpuid = hz_next_core();
    if (was_default && hz_core_mode == 2 && allowed) {
        static uint32_t counter;
        const u64 sliced = allowed & 7 ? allowed & 7 : allowed;
        int cores[4], count = 0;
        for (int core = 0; core < 4; ++core) if (sliced & (1ull << core)) cores[count++] = core;
        cpuid = cores[__atomic_fetch_add(&counter, 1, __ATOMIC_RELAXED) % (uint32_t)count];
        if (hz_thread_priority >= 0 && hz_thread_priority <= 63) prio = hz_thread_priority;
    }
    if (cpuid >= 0 && cpuid < 4) __atomic_fetch_add(&hz_core_threads[cpuid], 1, __ATOMIC_RELAXED);
    start->entry = entry;
    start->arg = arg;
    rc = __real_threadCreate(t, hz_thread_entry, start, stack_mem, stack_sz, prio, cpuid);
    if (R_FAILED(rc)) { free(start); return rc; }
    {   /* remembered for the stall report (hz_thread_report) */
        const uint32_t slot = __atomic_fetch_add(&hz_thread_count, 1, __ATOMIC_RELAXED);
        if (slot < HZ_THREAD_SLOTS) { hz_thread_handles[slot] = t->handle; hz_thread_entries[slot] = (uintptr_t)entry; }
    }
    if (was_default && hz_core_mode == 2 && allowed) {
        const u64 mask = hz_affinity_mask & allowed ? hz_affinity_mask & allowed : allowed;
        if (R_FAILED(svcSetThreadCoreMask(t->handle, cpuid, (u32)mask))) ++hz_sched_failures;
    }
    return rc;
}

/* Locks. newlib's pthread mutex is libnx's Mutex: when it is contended, unlock hands it to the
 * first waiter in the kernel, and that waiter may not run for a while. With many threads on few
 * cores every hot lock then turns into a queue ("lock convoy": 882 s of queueing in a 1200 s run
 * of the inFAMOUS 1 runtime; a barging lock took its loading screen from 3 fps to 25-30). Here a
 * thread first tries to take the lock itself for a short while (spinning, then yielding its
 * core), and only then queues in the kernel. Condition variables keep working: the lock word is
 * still libnx's. Linked with --wrap=pthread_mutex_lock. hz_lock_spins = 0 turns it off. */
int hz_lock_spins = 96;
int hz_lock_yields = 6;
uint64_t hz_lock_queued;        /* how often a lock still had to queue in the kernel */
int __real_pthread_mutex_lock(pthread_mutex_t *mutex);
int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex)
{
    int rc = pthread_mutex_trylock(mutex);
    if (rc != EBUSY) return rc;
    for (int i = 0; i < hz_lock_spins; ++i) {
        __asm__ volatile("yield");
        rc = pthread_mutex_trylock(mutex);
        if (rc != EBUSY) return rc;
    }
    for (int i = 0; i < hz_lock_yields; ++i) {
        svcSleepThread(0);   /* give the core to another runnable thread of this priority */
        rc = pthread_mutex_trylock(mutex);
        if (rc != EBUSY) return rc;
    }
    __atomic_fetch_add(&hz_lock_queued, 1, __ATOMIC_RELAXED);
    return __real_pthread_mutex_lock(mutex);
}

/* The engine asks for 256 MiB host stacks for guest PPU threads and 16 MiB for SPU threads: on a
 * desktop that is address space, here every byte of a stack is real memory from the heap. Requests
 * above the cap are cut to it (linked with --wrap=pthread_attr_setstacksize). The cap is a guess
 * until the real depth of the lifted call chains is measured: an overflow is NOT detected. */
size_t hz_stack_cap_bytes = 4u << 20;
uint64_t hz_stack_clamped_count;
int __real_pthread_attr_setstacksize(pthread_attr_t *attr, size_t size);
int __wrap_pthread_attr_setstacksize(pthread_attr_t *attr, size_t size)
{
    if (size > hz_stack_cap_bytes) { size = hz_stack_cap_bytes; ++hz_stack_clamped_count; }
    return __real_pthread_attr_setstacksize(attr, size);
}

/* Detached threads. newlib's pthread_detach on libnx does not release anything when the thread
 * ends: its kernel thread object and its stack (heap memory here) stay for good. The engine
 * detaches every guest thread nobody joins, and the game starts a short-lived one for each save
 * or load ("Game State Thread") and for each movie (Bink). Console run 8: after 45 minutes every
 * pthread_create answered ENOMEM, the save thread never ran, and the game showed "There was an
 * error while saving." about three times a second.
 * So detaching is done here instead (linked with --wrap=pthread_create, pthread_detach and
 * pthread_exit): a detached thread stays joinable underneath, says when its function has
 * returned, and is joined -- which closes it and frees its stack -- at the next pthread_create. */
typedef struct hz_pthread {
    void *(*fn)(void *);
    void *arg;
    pthread_t thread;
    int detached, done;
    struct hz_pthread *next;
} hz_pthread;
static Mutex hz_pthread_lock;
static hz_pthread *hz_pthreads;
static __thread hz_pthread *hz_pthread_self;
uint64_t hz_pthread_stats[3];   /* created, reaped after detach, creations that failed */
int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);
int __real_pthread_detach(pthread_t thread);
void __real_pthread_exit(void *value) __attribute__((noreturn));
static void *hz_pthread_main(void *raw)
{
    hz_pthread *record = raw;
    void *value;
    hz_pthread_self = record;
    value = record->fn(record->arg);
    __atomic_store_n(&record->done, 1, __ATOMIC_RELEASE);
    return value;
}
void __wrap_pthread_exit(void *value)
{
    if (hz_pthread_self) __atomic_store_n(&hz_pthread_self->done, 1, __ATOMIC_RELEASE);
    __real_pthread_exit(value);
}
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
    hz_pthread *record = calloc(1, sizeof *record), *finished = NULL, **link;
    int rc;
    if (!record) return ENOMEM;
    record->fn = fn;
    record->arg = arg;
    mutexLock(&hz_pthread_lock);
    for (link = &hz_pthreads; *link;) {
        hz_pthread *r = *link;
        if (r->detached && __atomic_load_n(&r->done, __ATOMIC_ACQUIRE)) {
            *link = r->next; r->next = finished; finished = r;
        } else link = &r->next;
    }
    mutexUnlock(&hz_pthread_lock);
    while (finished) {      /* before the new thread is made: its stack may be the one freed here */
        hz_pthread *r = finished;
        finished = r->next;
        pthread_join(r->thread, NULL);
        free(r);
        __atomic_fetch_add(&hz_pthread_stats[1], 1, __ATOMIC_RELAXED);
    }
    mutexLock(&hz_pthread_lock);
    rc = __real_pthread_create(&record->thread, attr, hz_pthread_main, record);
    if (rc == 0) {
        *thread = record->thread;
        record->next = hz_pthreads; hz_pthreads = record;
        __atomic_fetch_add(&hz_pthread_stats[0], 1, __ATOMIC_RELAXED);
    }
    mutexUnlock(&hz_pthread_lock);
    if (rc != 0) {
        free(record);
        __atomic_fetch_add(&hz_pthread_stats[2], 1, __ATOMIC_RELAXED);
        fprintf(stderr, "[hz] pthread_create failed (%d): %llu created, %llu reaped, %u libnx threads so far\n", rc,
                (unsigned long long)hz_pthread_stats[0], (unsigned long long)hz_pthread_stats[1], (unsigned)hz_thread_count);
    }
    return rc;
}
int __wrap_pthread_detach(pthread_t thread)
{
    int found = 0;
    mutexLock(&hz_pthread_lock);
    for (hz_pthread *r = hz_pthreads; r; r = r->next)
        if (pthread_equal(r->thread, thread)) { r->detached = 1; found = 1; break; }
    mutexUnlock(&hz_pthread_lock);
    return found ? 0 : __real_pthread_detach(thread);
}
