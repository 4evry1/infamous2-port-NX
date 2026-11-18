/* The frame clock owns markers. The status thread samples that same thread and
 * writes the capture after the window closes. Each published row is immutable. */
#include <switch.h>
#include <hz_target.h>
#include <hz_build.h>
#include <hz_fps_hud.h>
#include "hz_target_gate.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum capture_state { DISABLED, ARMED, REQUESTED, CAPTURING, FINISHED };
enum { EVENT_CAP = 131072, SAMPLE_CAP = 2000 };
static const uint64_t sample_period_ns = 10000000ull;
static const uint64_t capture_duration_ns = 20000000000ull;
typedef struct {
    uint64_t ns, cpu;
    unsigned phase, frame, draws;
} phase_event;
typedef struct {
    uint64_t ns, resume_ns, pc, lr;
} pc_sample;
static struct {
    atomic_int state;
    unsigned target, frame, draws, trigger_frame, trigger_draws;
    target_gate gate;
    Handle renderer;
    phase_event *events;
    atomic_uint count, dropped;
    pc_sample samples[SAMPLE_CAP];
} capture;
extern uint64_t hz_draw_thread_cpu_ns(void);

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000000000ull + ts.tv_nsec;
}

void hz_target_configure(const char *setting)
{
    if (!setting || !*setting || strspn(setting,"0123456789") != strlen(setting)) return;
    unsigned long draws = strtoul(setting, NULL, 10);
    if (!draws || draws > 100000) return;
    capture.target = (unsigned)draws;
    atomic_store(&capture.state, ARMED);
}

void hz_target_mark(enum hz_target_phase phase)
{
    if (atomic_load_explicit(&capture.state, memory_order_acquire) != CAPTURING) return;
    unsigned n = atomic_load_explicit(&capture.count, memory_order_relaxed);
    if (n >= EVENT_CAP) { atomic_fetch_add(&capture.dropped, 1); return; }
    capture.events[n] = (phase_event){ mono_ns(), hz_draw_thread_cpu_ns(), phase, capture.frame, capture.draws };
    atomic_store_explicit(&capture.count, n+1, memory_order_release);
}

static atomic_uint completed_guest_frame;
uint64_t hz_guest_frame_count(void)
{
    return atomic_load_explicit(&completed_guest_frame, memory_order_relaxed);
}

void hz_target_frame(unsigned frame, unsigned draws)
{
    atomic_store_explicit(&completed_guest_frame, frame, memory_order_relaxed);
    int state = atomic_load_explicit(&capture.state, memory_order_acquire);
    if (state == DISABLED || state == FINISHED) return;
    capture.frame = frame;
    capture.draws = draws;
    if (state == ARMED && target_is_steady(&capture.gate, capture.target, draws, mono_ns())) {
        capture.renderer = threadGetCurHandle();
        capture.trigger_frame = frame;
        capture.trigger_draws = draws;
        atomic_store_explicit(&capture.state, REQUESTED, memory_order_release);
    } else if (state == CAPTURING) {
        hz_target_mark(HZ_FRAME);
    }
}

static int save_capture(const char *directory, uintptr_t ref, uint64_t begin, uint64_t end,
                        unsigned used, unsigned failures)
{
    static const char *const phases[] = {"wait","wake","tick","tick_done","present","present_done",
        "drain","drain_done","pump","pump_done","frame","flip_check","flip_check_done"};
    char path[200], partial[220];
    snprintf(path, sizeof path, "%s/target-" HZ_VERSION "-%llu.tsv", directory, (unsigned long long)begin);
    snprintf(partial, sizeof partial, "%s.part", path);
    FILE *f = fopen(partial,"w");
    if (!f) return 0;
    const unsigned n = atomic_load_explicit(&capture.count, memory_order_acquire);
    fprintf(f, "# " HZ_VERSION " ref=%llx begin=%llu end=%llu target=%u trigger_frame=%u trigger_draws=%u samples=%u events=%u dropped=%u failures=%u\n",
        (unsigned long long)ref, (unsigned long long)begin, (unsigned long long)end, capture.target,
        capture.trigger_frame, capture.trigger_draws, used, n, atomic_load(&capture.dropped), failures);
    for (unsigned i=0; i<n; ++i) {
        const phase_event *e = &capture.events[i];
        fprintf(f,"E\t%llu\t%llu\t%s\t%u\t%u\n",(unsigned long long)e->ns,
            (unsigned long long)e->cpu,phases[e->phase],e->frame,e->draws);
    }
    for (unsigned i=0; i<used; ++i) {
        const pc_sample *p = &capture.samples[i];
        fprintf(f,"P\t%llu\t%llu\t%llx\t%llx\n",(unsigned long long)p->ns,
            (unsigned long long)p->resume_ns,(unsigned long long)p->pc,(unsigned long long)p->lr);
    }
    int ok = !ferror(f) && fflush(f)==0 && fsync(fileno(f))==0;
    if (fclose(f) != 0) ok = 0;
    return ok && rename(partial,path)==0;
}

void hz_target_poll(const char *directory, uintptr_t ref)
{
    if (atomic_load_explicit(&capture.state, memory_order_acquire) != REQUESTED) return;
    /* One capture per process. Keep its storage until exit so a renderer
     * crossing the stop boundary can finish publishing its last row. */
    capture.events = malloc(EVENT_CAP * sizeof *capture.events);
    if (!capture.events) {
        atomic_store(&capture.state, FINISHED);
        fprintf(stderr,"[target] could not allocate trace buffer\n");
        return;
    }
    unsigned used = 0, failures = 0;
    fprintf(stderr,"[target] " HZ_VERSION " starting frame=%u draws=%u target=%u\n",
        capture.trigger_frame,capture.trigger_draws,capture.target);
    const uint64_t begin = mono_ns(), deadline = begin + capture_duration_ns;
    atomic_store_explicit(&capture.state, CAPTURING, memory_order_release);
    uint64_t next = begin;
    while (used < SAMPLE_CAP) {
        uint64_t now = mono_ns();
        if (now >= deadline) break;
        if (now < next) { svcSleepThread(next-now); continue; }
        ThreadContext ctx;
        if (R_SUCCEEDED(svcSetThreadActivity(capture.renderer,ThreadActivity_Paused))) {
            uint64_t sampled = mono_ns();
            Result read_result = svcGetThreadContext3(&ctx,capture.renderer);
            Result resumed = svcSetThreadActivity(capture.renderer,ThreadActivity_Runnable);
            if (R_FAILED(resumed)) {
                resumed = svcSetThreadActivity(capture.renderer,ThreadActivity_Runnable);
                if (R_FAILED(resumed)) { ++failures; break; }
            }
            if (R_SUCCEEDED(read_result))
                capture.samples[used++] = (pc_sample){ sampled, mono_ns(), ctx.pc.x, ctx.lr };
            else ++failures;
        } else ++failures;
        /* Skip missed slots instead of suspending the renderer in a burst. */
        next += ((mono_ns()-next)/sample_period_ns + 1)*sample_period_ns;
    }
    atomic_store_explicit(&capture.state, FINISHED, memory_order_release);
    uint64_t end = mono_ns();
    int saved = save_capture(directory,ref,begin,end,used,failures);
    fprintf(stderr,"[target] " HZ_VERSION " saved=%d samples=%u failures=%u events=%u dropped=%u id=%llu\n",
        saved,used,failures,atomic_load(&capture.count),atomic_load(&capture.dropped),(unsigned long long)begin);
}
