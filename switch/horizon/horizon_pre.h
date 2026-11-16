/* Horizon layer: force-included (-include) into every engine and port source built for Switch.
 * Small spelling differences between glibc/macOS and newlib+libnx, kept out of the engine tree. */
#ifndef HZ_PRE_H
#define HZ_PRE_H
#include <setjmp.h>
#include <time.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef __cplusplus
extern "C" {
#endif
/* newlib has no signal mask to skip saving: the underscore forms are the plain ones */
#define _setjmp(env)       setjmp(env)
#define _longjmp(env, val) longjmp(env, val)
time_t timegm(struct tm *tm);
int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *abstime);
int clock_nanosleep(clockid_t clock, int flags, const struct timespec *request, struct timespec *remain);
#ifndef TIMER_ABSTIME
#  define TIMER_ABSTIME 4
#endif
/* Per-thread CPU time (profiling counters): newlib has no such clock. clock_gettime() on this id
 * fails with EINVAL through libnx, so those counters read as zero. */
#ifndef CLOCK_THREAD_CPUTIME_ID
#  define CLOCK_THREAD_CPUTIME_ID ((clockid_t)3)
#endif
#ifndef PTHREAD_STACK_MIN
#  define PTHREAD_STACK_MIN 16384
#endif
/* The engine parks a thread with a signal for SuspendThread (samplers, stall probes: diagnostics).
 * No signals here: the number only has to exist; sigaction() refuses it (horizon/include/signal.h). */
#define PS3_SUSPEND_SIGNAL 31
/* x86 spin-wait hint -> arm64 yield */
#define _mm_pause() __asm__ volatile("yield")
/* x86 time-stamp counter -> the arm64 physical counter (profiling only). Not cntvct_el0: Horizon
 * does not let a program read the virtual counter (the inFAMOUS 1 build faulted on it on the
 * console, 2026-11-12); cntpct_el0 is what libnx's armGetSystemTick reads. */
static inline uint64_t hz_rdtsc(void) { uint64_t v; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v)); return v; }
#define __builtin_ia32_rdtsc() hz_rdtsc()
#ifdef __cplusplus
}
#endif
#endif
