/* Horizon layer: the small libc/POSIX calls newlib+libnx do not provide and the ps3recomp engine
 * links against. Each says what it does instead; none pretends to succeed where the engine would
 * then rely on behaviour that is not there. */
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <switch.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <malloc.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ---- signals: none on Horizon ---------------------------------------------------------------- */
int hz_sigaction(int sig, const struct hz_sigaction *act, struct hz_sigaction *old)
{
    (void)sig; (void)act;
    if (old) memset(old, 0, sizeof *old);
    errno = ENOSYS;
    return -1;
}
int pthread_kill(pthread_t thread, int sig)
{
    (void)thread;
    return sig == 0 ? 0 : ENOSYS;   /* 0 = "is it alive": yes; delivering a signal: cannot */
}

/* ---- dynamic loading: none on Horizon, except one name --------------------------------------- */
/* The Vulkan backend finds its driver with dlopen("libvulkan.so.1") and asks it for
 * vkGetInstanceProcAddr; every other entry point comes from that one. Here the driver is Mesa NVK,
 * linked in statically (portlibs libvulkan.a), so "opening" a Vulkan library gives a token handle
 * and that one symbol is the driver's own function. Nothing else can be opened. */
typedef void (*hz_void_function)(void);
hz_void_function vkGetInstanceProcAddr(void *instance, const char *name);
static char s_vulkan_token;
int dladdr(const void *addr, Dl_info *info) { (void)addr; if (info) memset(info, 0, sizeof *info); return 0; }
void *dlopen(const char *path, int mode)
{
    (void)mode;
    return path && strstr(path, "vulkan") ? (void *)&s_vulkan_token : NULL;
}
void *dlsym(void *handle, const char *name)
{
    if (handle == (void *)&s_vulkan_token && name && !strcmp(name, "vkGetInstanceProcAddr"))
        return (void *)(uintptr_t)&vkGetInstanceProcAddr;
    return NULL;
}
int dlclose(void *handle) { (void)handle; return 0; }
char *dlerror(void) { return (char *)"no dynamic loader on Horizon"; }

/* ---- backtrace: frame-pointer walk; addresses are symbolised offline from the link map -------- */
int backtrace(void **buffer, int size)
{
    const uintptr_t *fp = __builtin_frame_address(0);
    int n = 0;
    while (n < size && fp && ((uintptr_t)fp & 15) == 0) {
        const uintptr_t next = fp[0], lr = fp[1];
        if (!lr) break;
        buffer[n++] = (void *)lr;
        if (next <= (uintptr_t)fp || next - (uintptr_t)fp > 0x100000) break;   /* stacks grow down */
        fp = (const uintptr_t *)next;
    }
    return n;
}
void backtrace_symbols_fd(void *const *buffer, int size, int fd)
{
    char line[32];
    for (int i = 0; i < size; ++i) {
        const int len = snprintf(line, sizeof line, "%p\n", buffer[i]);
        if (len > 0) write(fd, line, (size_t)len);
    }
}
char **backtrace_symbols(void *const *buffer, int size)
{
    char **out = malloc((size_t)size * (sizeof(char *) + 20));
    if (!out) return NULL;
    for (int i = 0; i < size; ++i) {
        out[i] = (char *)(out + size) + i * 20;
        snprintf(out[i], 20, "%p", buffer[i]);
    }
    return out;
}

/* ---- processes: one process, no children ------------------------------------------------------ */
int execvp(const char *file, char *const argv[]) { (void)file; (void)argv; errno = ENOSYS; return -1; }
pid_t waitpid(pid_t pid, int *status, int options) { (void)pid; (void)status; (void)options; errno = ECHILD; return -1; }
int getrusage(int who, struct rusage *usage)
{
    (void)who;
    if (usage) memset(usage, 0, sizeof *usage);   /* no CPU-time accounting: reads as zero */
    return 0;
}
long syscall(long number, ...)
{
    if (number == SYS_gettid) {
        u64 id = 0;
        svcGetThreadId(&id, CUR_THREAD_HANDLE);
        return (long)id;
    }
    errno = ENOSYS;
    return -1;
}

/* ---- memory ----------------------------------------------------------------------------------- */
int getpagesize(void) { return 0x1000; }
long sysconf(int name)
{
    switch (name) {
    case _SC_PAGESIZE: return 0x1000;
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN: {
        /* the cores this process may use (3 for an ordinary application: core 3 is the system's) */
        u64 mask = 0;
        if (R_SUCCEEDED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) && mask)
            return __builtin_popcountll(mask);
        return 3;
    }
    case _SC_PHYS_PAGES: {
        u64 total = 0;
        if (R_SUCCEEDED(svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0)) && total)
            return (long)(total / 0x1000);
        return 0x80000000l / 0x1000;
    }
    default: errno = EINVAL; return -1;
    }
}
int posix_memalign(void **out, size_t alignment, size_t size)
{
    void *p;
    if (alignment < sizeof(void *) || (alignment & (alignment - 1))) return EINVAL;
    p = memalign(alignment, size ? size : 1);
    if (!p) return ENOMEM;
    *out = p;
    return 0;
}

/* ---- time -------------------------------------------------------------------------------------- */
time_t timegm(struct tm *tm)
{
    /* days from civil (proleptic Gregorian), UTC, no time-zone state touched */
    int y = tm->tm_year + 1900, m = tm->tm_mon + 1;
    long long era, days;
    unsigned yoe, doy, doe;
    y += (m - 1) / 12; m = (m - 1) % 12 + 1;
    if (m <= 0) { m += 12; --y; }
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);
    doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + tm->tm_mday - 1);
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    days = era * 146097 + (long long)doe - 719468;
    return (time_t)(days * 86400 + tm->tm_hour * 3600LL + tm->tm_min * 60LL + tm->tm_sec);
}
int timespec_get(struct timespec *ts, int base)
{
    if (base != 1 /* TIME_UTC */ || clock_gettime(CLOCK_REALTIME, ts) != 0) return 0;
    return base;
}
int clock_nanosleep(clockid_t clock, int flags, const struct timespec *request, struct timespec *remain)
{
    struct timespec now, delta;
    if (remain) { remain->tv_sec = 0; remain->tv_nsec = 0; }
    if (!(flags & 4 /* TIMER_ABSTIME */)) return nanosleep(request, NULL) == 0 ? 0 : errno;
    if (clock_gettime(clock, &now) != 0) return EINVAL;
    delta.tv_sec = request->tv_sec - now.tv_sec;
    delta.tv_nsec = request->tv_nsec - now.tv_nsec;
    if (delta.tv_nsec < 0) { delta.tv_nsec += 1000000000L; --delta.tv_sec; }
    if (delta.tv_sec < 0) return 0;   /* already past */
    return nanosleep(&delta, NULL) == 0 ? 0 : errno;
}
int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *abstime)
{
    /* newlib's pthread mutex has no timed wait: try, then sleep in short steps until the deadline
     * (CLOCK_REALTIME, as POSIX says). The engine uses this for lv2 mutex timeouts. */
    for (;;) {
        struct timespec now;
        const struct timespec step = {0, 200000};   /* 200 us */
        const int rc = pthread_mutex_trylock(m);
        if (rc != EBUSY) return rc;
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > abstime->tv_sec || (now.tv_sec == abstime->tv_sec && now.tv_nsec >= abstime->tv_nsec))
            return ETIMEDOUT;
        nanosleep(&step, NULL);
    }
}

/* Fast movies (HZ_FAST_MOVIES=1 in env.txt). The game's cutscene movies are Bink files decoded
 * by SPU code on one thread: about 5 frames a second on this console, so the opening alone
 * takes eight minutes. The disc carries a 4 second placeholder movie, which the PC port's test
 * runs put in place of every cutscene movie through symlinks (tools/linux_run.py fastmovies).
 * The card's file system has no links and the engine's mod loader only replaces .xpps files, so
 * the same swap is made here, where the engine opens and stats host files (linked with
 * --wrap=open and --wrap=stat): a path .../movies/cutscene/<name>.bik is answered with the
 * placeholder's. Nothing on the card changes; the story movies are simply not shown. */
#define HZ_MOVIE_DIR "/movies/cutscene/"
#define HZ_MOVIE_PLACEHOLDER "sdmc:/switch/infamous2-port/game/disc/PS3_GAME/USRDIR/movies/cutscene/cut_scene_place_holder.bik"
uint64_t hz_fast_movie_opens;
static const char *hz_movie_path(const char *path)
{
    static int on = -1;
    const char *dir, *name;
    size_t length;
    if (on < 0) { const char *e = getenv("HZ_FAST_MOVIES"); on = e && e[0] == '1'; }
    if (!on || !path) return path;
    dir = strstr(path, HZ_MOVIE_DIR);
    if (!dir) return path;
    name = dir + sizeof HZ_MOVIE_DIR - 1;
    length = strlen(name);
    if (length < 5 || strchr(name, '/') || strcasecmp(name + length - 4, ".bik") != 0) return path;
    if (strcasecmp(name, "cut_scene_place_holder.bik") == 0) return path;
    return HZ_MOVIE_PLACEHOLDER;
}
int __real_open(const char *path, int flags, ...);
int __wrap_open(const char *path, int flags, ...)
{
    const char *use = hz_movie_path(path);
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap); }
    if (use != path) {
        if (__atomic_fetch_add(&hz_fast_movie_opens, 1, __ATOMIC_RELAXED) < 64)
            fprintf(stderr, "[hz] fast movies: %s -> the 4 s placeholder\n", strrchr(path, '/') + 1);
    }
    return __real_open(use, flags, mode);
}
int __real_stat(const char *path, struct stat *out);
int __wrap_stat(const char *path, struct stat *out)
{
    return __real_stat(hz_movie_path(path), out);
}

/* rename() onto a file that exists. POSIX replaces the destination; the card's file system
 * refuses. The engine saves its pipeline cache and pipeline recipes by writing a temporary file
 * and renaming it over the old one (rsx_vulkan_backend.c vk_sc_write_file), so on the console
 * the first run's 6 KB pipelines.bin was never replaced: every run logged "shader cache: cannot
 * write ...pipelines.bin" and built all ~1,000 pipelines again while draws waited for them
 * (console runs 7 to 12). Linked with --wrap=rename: on a failure with the destination present,
 * the destination is removed and the rename tried once more. The moment between the two is not
 * atomic; a crash there loses a cache file, which the engine rebuilds. */
uint64_t hz_rename_replaced;
int __real_rename(const char *from, const char *to);
int __wrap_rename(const char *from, const char *to)
{
    struct stat st;
    int rc = __real_rename(from, to);
    if (rc == 0) return 0;
    if (__real_stat(to, &st) != 0 || __real_stat(from, &st) != 0) return rc;
    if (remove(to) != 0) return rc;
    rc = __real_rename(from, to);
    if (rc == 0) __atomic_fetch_add(&hz_rename_replaced, 1, __ATOMIC_RELAXED);
    return rc;
}
