/* Horizon layer: the Switch entry of the inFAMOUS 2 port (linked with --wrap=main).
 *
 * The port's own main() expects a desktop: an EBOOT path on the command line, its settings in
 * environment variables, a terminal for its output and a main thread with a deep stack. Here:
 *   - the folder is fixed: sdmc:/switch/infamous2-port (EBOOT.elf, game/, logs/, env.txt);
 *   - the environment play.sh would set is set here, then env.txt (KEY=VALUE lines) overrides it;
 *   - stdout and stderr go to logs/stdout.log and logs/stderr.log on the card;
 *   - the screen shows a status console the whole time (never a silent black screen): elapsed
 *     time, a spinner, log sizes, guest memory, and the result or the fault when the run ends;
 *   - the game's main() runs on its own thread with a large stack; this thread keeps the screen.
 * Nothing here draws the game: the Vulkan surface is a later step. */
#include <hz_layer.h>
#include <hz_target.h>
#include "hz_log_batch.h"
#include <switch.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HZ_APP "sdmc:/switch/infamous2-port"
#include <hz_build.h>
#include <hz_fps_hud.h>

int __real_main(int argc, char **argv);
int __real_pthread_attr_setstacksize(pthread_attr_t *attr, size_t size);

/* ---- the game's own output, live: every line it prints goes to its log file AND into a ring of
 *      the most recent lines, which the status screen shows under the status line -------------- */
#include <reent.h>

/* CPU residency of this thread across all cores, for sampled draw diagnostics. */
uint64_t hz_draw_thread_cpu_ns(void)
{
    u64 ticks;
    const u32 info = hosversionAtLeast(13, 0, 0) ? InfoType_ThreadTickCount : InfoType_ThreadTickCountDeprecated;
    if (R_FAILED(svcGetInfo(&ticks, info, CUR_THREAD_HANDLE, TickCountInfo_Total)))
        return UINT64_MAX;
    return armTicksToNs(ticks);
}

enum { HZ_RING_LINES = 64, HZ_RING_WIDTH = 100, HZ_SCREEN_COLUMNS = 79, HZ_SCREEN_LOG_ROWS = 31 };
static char s_ring[HZ_RING_LINES][HZ_RING_WIDTH];
static volatile uint32_t s_ring_count;       /* lines ever stored; the newest is (count - 1) % LINES */
static char s_ring_pending[HZ_RING_WIDTH];
static uint32_t s_ring_pending_length;
static Mutex s_ring_lock;
static volatile uint64_t s_log_bytes;
static void ring_add(const char *text, size_t length)
{
    mutexLock(&s_ring_lock);
    for (size_t i = 0; i < length; ++i) {
        const char c = text[i];
        if (c == '\n' || c == '\r') {
            if (s_ring_pending_length) {
                char *slot = s_ring[s_ring_count % HZ_RING_LINES];
                memcpy(slot, s_ring_pending, s_ring_pending_length);
                slot[s_ring_pending_length] = 0;
                s_ring_pending_length = 0;
                __atomic_store_n(&s_ring_count, s_ring_count + 1, __ATOMIC_RELEASE);
            }
        } else if (s_ring_pending_length < HZ_RING_WIDTH - 1) {
            s_ring_pending[s_ring_pending_length++] = (c >= 32 && c < 127) ? c : ' ';
        }
    }
    mutexUnlock(&s_ring_lock);
}
/* The no-logs variant (the same program started from a file with "quiet" in its name, or
 * HZ_QUIET=1 in env.txt): no log files, no play log, no periodic status lines, no fsync. For
 * comparing speed with and without logging. A fault is still written to logs/fault.log. */
static int s_quiet;
/* PS3_GAME_MAX_DT_MS=<ms> (hz-0.29; the old tree had it in work/port-switch/game_clock.c, v0.5.2 does not):
 * the longest step the game's clock hands the simulation. func_00089610 clamps each step to 1/15 s
 * (the float at 0x009A0AD4, used while the byte at 0xA7F556 is set: every gameplay frame measured)
 * or 1/30 s (0x009A0ADC), so below 15 fps the game runs slower than real time. At 9 fps that leaves
 * gameplay-time animation behind the cutscene clock (run 28: the "yo-yo" in live cutscenes, a guess
 * to test). Both floats are replaced, once, only while they still hold the original values; the
 * main loop calls this every 100 ms until then. 34..1000 ms; unset or out of range = the game's
 * own clamps. Run 18 used 250 ms: the camera went wild. */
extern uint8_t *vm_base;
static void hz_game_step_patch(void)
{
    static int state;                       /* 0 = not read yet, 1 = waiting for the values, 2 = done or off */
    static uint32_t bits;
    if (state == 2) return;
    if (state == 0) {
        const char *e = getenv("PS3_GAME_MAX_DT_MS");
        const double ms = e ? atof(e) : 0.0;
        state = 2;
        if (ms >= 34.0 && ms <= 1000.0) { const float s = (float)(ms / 1000.0); memcpy(&bits, &s, 4); state = 1; }
        if (state == 2) return;
    }
    if (!vm_base) return;
    {
        static const uint32_t at[2] = {0x009A0AD4u, 0x009A0ADCu};
        static const uint32_t orig[2] = {0x3D888889u, 0x3D088889u};   /* 1/15 and 1/30, big-endian floats */
        for (int i = 0; i < 2; ++i) {
            const uint8_t *p = vm_base + at[i];
            if (((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]) != orig[i]) return;
        }
        for (int i = 0; i < 2; ++i) {
            uint8_t *p = vm_base + at[i];
            p[0] = (uint8_t)(bits >> 24); p[1] = (uint8_t)(bits >> 16); p[2] = (uint8_t)(bits >> 8); p[3] = (uint8_t)bits;
        }
    }
    {
        float shown; memcpy(&shown, &bits, 4);
        fprintf(stderr, "[hz] longest game step raised from 1/15 s (1/30 s) to %.0f ms (PS3_GAME_MAX_DT_MS)\n", shown * 1000.0f);
    }
    state = 2;
}
/* HZ_DETAIL_STEP=<1..20> (hz-0.44): the game's own detail ladder, used as a floor. inFAMOUS 2 keeps a
 * table of 21 detail steps at 0x009D7224 (20 bytes each: LOD radius scale 1.0..0.1, a count 3..0 (most
 * likely shadow splits - 1), a far distance 1e6..500, a screen-size cull 0..0.2, feature-off bits). The u32
 * at 0x00A6E8C0 picks the step; the game raises it itself when a frame's RSX commands pass 5 MiB, and
 * starts at 0 (full detail). Steps 0..N-1 are overwritten with step N, so the game never draws more than
 * step N allows and can still go lower. Static analysis (draw-distance agent, 2026-11-16): f0 scales the
 * job 210 LOD radii (400..15000 cm), f3 is job 210's screen-size cull. Written once, only while the
 * table still holds the disc's bytes; the game never writes it. The step in use is logged when it changes.
 * Unset or 0: the game's table. */
static void hz_detail_patch(void)
{
    static int state, step;
    static uint32_t last_idx = 0xFFFFFFFFu;
    enum { TAB = 0x009D7224u, IDX = 0x00A6E8C0u, ENT = 20 };
    static const uint8_t orig0[ENT] = { 0x3F, 0x80, 0, 0, 0, 0, 0, 3, 0x49, 0x74, 0x24, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    static const uint8_t orig20[ENT] = { 0x3D, 0xCC, 0xCC, 0xCD, 0, 0, 0, 0, 0x43, 0xFA, 0, 0, 0x3E, 0x4C, 0xCC, 0xCD, 0, 0, 0x03, 0xE0 };
    if (!vm_base) return;
    if (state == 0) {
        const char *e = getenv("HZ_DETAIL_STEP");
        step = e ? atoi(e) : 0;
        state = step >= 1 && step <= 20 ? 1 : 2;
    }
    if (state == 1) {
        uint8_t *t = vm_base + TAB;
        if (memcmp(t, orig0, ENT) || memcmp(t + 20 * ENT, orig20, ENT)) return;   /* not loaded yet, or another EBOOT */
        for (int i = 0; i < step; ++i) memcpy(t + i * ENT, t + step * ENT, ENT);
        {
            float f0, f2, f3; uint32_t w1;
            const uint8_t *s = t + step * ENT;
            uint32_t b0 = (uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8 | s[3];
            uint32_t b2 = (uint32_t)s[8] << 24 | (uint32_t)s[9] << 16 | (uint32_t)s[10] << 8 | s[11];
            uint32_t b3 = (uint32_t)s[12] << 24 | (uint32_t)s[13] << 16 | (uint32_t)s[14] << 8 | s[15];
            w1 = (uint32_t)s[4] << 24 | (uint32_t)s[5] << 16 | (uint32_t)s[6] << 8 | s[7];
            memcpy(&f0, &b0, 4); memcpy(&f2, &b2, 4); memcpy(&f3, &b3, 4);
            fprintf(stderr, "[hz] detail floor: steps 0..%d now step %d (LOD radius x%.2f, count %u, far %.0f, size cull %.2f) (HZ_DETAIL_STEP)\n",
                    step - 1, step, f0, w1, f2, f3);
        }
        state = 2;
    }
    {
        const uint8_t *p = vm_base + IDX;
        const uint32_t idx = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
        if (idx != last_idx && idx <= 20) {
            static int shown;
            if (shown < 40) { shown++; fprintf(stderr, "[hz] detail step in use: %u\n", (unsigned)idx); }
            last_idx = idx;
        }
    }
}
/* Pictures handed to the screen: counted by the wrap of libnx's nwindowQueueBuffer, which the
 * Vulkan driver's swapchain calls once per present (linked with --wrap=nwindowQueueBuffer). */
static uint64_t hz_presents;
Result __real_nwindowQueueBuffer(NWindow *nw, s32 slot, const NvMultiFence *fence);
Result __wrap_nwindowQueueBuffer(NWindow *nw, s32 slot, const NvMultiFence *fence)
{
    Result rc = __real_nwindowQueueBuffer(nw, slot, fence);
    if (R_SUCCEEDED(rc) && nw == nwindowGetDefault())
        __atomic_fetch_add(&hz_presents, 1, __ATOMIC_RELAXED);
    return rc;
}

uint64_t hz_output_frame_count(void)
{
    return __atomic_load_n(&hz_presents, __ATOMIC_RELAXED);
}
static unsigned s_profile_at[8];
static int s_profile_at_count;
static volatile int s_screen_owned;
enum { LOG_OUT, LOG_ERR, LOG_PLAY, LOG_COUNT };
static hz_log_sink *s_logs[LOG_COUNT];
static FILE *s_log_stdout;
static Thread s_writer;
static UEvent s_stop_writer;
static int s_writer_started;

/* Renderer bridge. The launcher opens the configured path before the guest starts. */
int hz_playlog_available(void) { return s_logs[LOG_PLAY] != NULL; }
void hz_playlog_append(const char *text, size_t length)
{
    hz_log_append(s_logs[LOG_PLAY], text, length);
}

static void flush_logs(void)
{
    for (int i = 0; i < LOG_COUNT; ++i) hz_log_flush(s_logs[i]);
}

static void log_writer(void *unused)
{
    (void)unused;
    for (;;) {
        Result rc = waitSingle(waiterForUEvent(&s_stop_writer), 2000000000ull);
        if (R_SUCCEEDED(rc)) break;
        flush_logs();
    }
}

static void finish_logs(void)
{
    if (s_writer_started) {
        ueventSignal(&s_stop_writer);
        threadWaitForExit(&s_writer);
        threadClose(&s_writer);
        s_writer_started = 0;
    }
    fflush(stdout);
    fflush(stderr);
    flush_logs();
    /* Other game threads may still unwind. Sinks and descriptors live until process exit. */
}

static void start_log_writer(void)
{
    /* Avoid counting the diagnostic writer as a guest thread or changing guest core assignment. */
    extern Result __real_threadCreate(Thread *, ThreadFunc, void *, void *, size_t, int, int);
    ueventCreate(&s_stop_writer, false);
    if (R_SUCCEEDED(__real_threadCreate(&s_writer, log_writer, NULL, NULL, 0x8000, 59, -2))) {
        if (R_SUCCEEDED(threadStart(&s_writer))) s_writer_started = 1;
        else threadClose(&s_writer);
    }
    atexit(finish_logs);
}

static int open_log_sink(int channel, const char *path)
{
    if (s_quiet || !path || !*path) return -1;
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0666);
    if (fd >= 0) {
        s_logs[channel] = hz_log_create(fd);
        if (!s_logs[channel]) { close(fd); return -1; }
    }
    return fd;
}

static _ssize_t stream_write(struct _reent *reent, void *cookie, const char *text, _READ_WRITE_BUFSIZE_TYPE length)
{
    (void)reent;
    if (s_quiet) {
        /* the no-logs build: nothing goes to the card; the text screen still shows the boot */
        if (length > 0 && s_screen_owned) ring_add(text, (size_t)length);
        return length;
    }
    if (length > 0) {
        ring_add(text, (size_t)length);
        hz_log_append(s_logs[cookie == s_log_stdout ? LOG_OUT : LOG_ERR], text, (size_t)length);
        __atomic_fetch_add(&s_log_bytes, (uint64_t)length, __ATOMIC_RELAXED);
    }
    return length;
}
/* Point a standard stream at a log file on its own descriptor, with the ring in front of it.
 * Not freopen(): it closes descriptor 1 and the file then gets that number back, so everything
 * written "to the screen" landed in stdout.log and the screen stayed black (hz-0.1, hz-0.2). */
static void stream_to_log(FILE *stream, int fd)
{
    fflush(stream);
    if (fd > 2) stream->_file = (short)fd;
    stream->_cookie = stream;
    stream->_write = stream_write;
    setvbuf(stream, NULL, _IOLBF, 0);
}

/* ---- the screen: fd 1 stays libnx's console after stdout itself is reopened on a file -------- */
static int s_text_console_enabled = 1;
static volatile int s_screen_owned;   /* see hz_screen_handover */

static void text_console_start(void)
{
    if (!s_text_console_enabled || s_screen_owned) return;
    consoleInit(NULL);
    s_screen_owned = 1;
}

static void text_console_refresh(void)
{
    if (s_screen_owned) consoleUpdate(NULL);
}

static void text_console_stop(void)
{
    if (!s_screen_owned) return;
    consoleExit(NULL);
    s_screen_owned = 0;
}
volatile int hz_screen_request;           /* 1 = the renderer wants the screen, 2 = it gave it back */
/* Called by the renderer's window shim (any thread): ask the main thread and wait until it did it. */
void hz_screen_handover(int request)
{
    __atomic_store_n(&hz_screen_request, request, __ATOMIC_RELEASE);
    for (int i = 0; i < 400 && __atomic_load_n(&hz_screen_request, __ATOMIC_ACQUIRE); ++i)
        svcSleepThread(5000000ull);   /* at most 2 s: the main thread polls every 100 ms */
}
static void screen(const char *format, ...)
{
    char line[256];
    va_list args;
    int len;
    if (!s_screen_owned) return;
    va_start(args, format);
    len = vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (len > 0) write(STDOUT_FILENO, line, (size_t)(len < (int)sizeof line ? len : (int)sizeof line - 1));
}

/* ---- environment ----------------------------------------------------------------------------- */
static void default_environment(void)
{
    static const char *const defaults[][2] = {
        {"PS3_VFS_ROOT", HZ_APP "/game/disc"},
        {"PS3_HDD0_ROOT", HZ_APP "/game/dev_hdd0"},
        {"PS3_DEV_FLASH", HZ_APP "/game/dev_flash"},
        {"PS3_GAMEDATA_ROOT", HZ_APP "/game/gamedata"},
        {"PS3_TITLE", "inFAMOUS 2"},
        {"GCM_SEMA_ACQUIRE", "1"}, {"GCM_SEMA_ACQUIRE_MS", "200"},
        /* 5000 (2026-11-15): with 1000 the title's park patches (900-940 ms, runs 41-43) were within
         * 100 ms of the resync, and the no-logs build lost the picture at the game */
        {"GCM_PARK_GRACE_MS", "5000"},
        {"PS3RECOMP_ENG_GPU_LIGHT", "07000B80:07384B80"}, {"PS3RECOMP_ENG_GPU_SSAO", "0F800000"},
        {"PS3_VERBOSE", "0"}, {"PS3_OVERLAY", "0"}, {"HEADLESS", "1"},
        {"PS3RECOMP_RENDER_SCALE", "1.0"}, {"PS3RECOMP_AA", "off"}, {"PS3RECOMP_SHARPEN", "0"},
        {"PS3RECOMP_SOFT_SHADOWS", "0"}, {"PS3RECOMP_SSAO", "0"}, {"PS3RECOMP_VK_FIF", "1"},
        /* texture cache: 512 entries evicted live textures thousands of times (console run 4) */
        {"PS3RECOMP_VK_ARENA_GROW", "0"}, {"PS3RECOMP_ENG_TEXCACHE", "2048"},
        /* the picture: the backend's "window" is the Switch screen (hz_window.c) */
        /* present "immediate": with "fifo" vkQueuePresentKHR took 20-110 ms a frame on the console
         * (run 5); on Horizon an immediate present does not tear, it only stops the queue blocking */
        {"PS3RECOMP_VK_WINDOW", "1"}, {"PS3RECOMP_VK_PRESENT", "immediate"},
        {"PS3RECOMP_PLAYLOG", HZ_APP "/logs/playlog.tsv"},
        /* no sound on the console (run 5): log every 2 s whether the game's audio ring holds data,
         * to tell "the game writes silence" from "the output device plays nothing" */
        {"AUDIO_PEAK", "1"},
        /* the renderer: NVK linked in, offscreen (no PS3RECOMP_VK_WINDOW: no surface yet), the
         * game's own shader programs through glslang, translated shaders kept on the card */
        {"PS3RECOMP_VK_GUEST_PROGRAMS", "1"},
        {"PS3RECOMP_VK_SHADER_CACHE", HZ_APP "/shader_cache"},
        {"PS3RECOMP_FPS_CAP", "30"},
        /* upstream v0.5.2's new switches (ps3recomp u052 build), for a 3-4 core ARM console:
         * PS3_JOB_WORKERS=5: the game's own five SPU job workers. "auto" would pick 5 too (6 wants
         * 10 physical cores), pinned so a wrong core count can never start a sixth worker here.
         * PS3RECOMP_LOWEND=0: the automatic small-card settings. NVK reports an integrated GPU, which
         * v0.5.2 treats as "low-end" and would then switch FBFETCH, FX_RES, the texture-cache cap
         * and the VRAM reaction on, unmeasured on the console; hz-0.22's render settings are kept.
         * Left at their defaults (not compiled here, or the same as the console already does):
         * PS3_USLEEP_WAIT, PS3_SEM_WAIT_LONG (Windows branches of sys_timer.c / sys_semaphore.c),
         * PS3_SPU_GROUP_MAX (6 = lv2's limit), PS3_CORE_SCARCE* (ps3_host_cores() is the real count
         * on the Switch, so "scarce" is true below 8 cores: right for 3-4). env.txt overrides all. */
        {"PS3_JOB_WORKERS", "5"}, {"PS3RECOMP_LOWEND", "0"},
    };
    for (size_t i = 0; i < sizeof defaults / sizeof defaults[0]; ++i) setenv(defaults[i][0], defaults[i][1], 1);
}
static int file_environment(void)   /* env.txt: KEY=VALUE per line, # comments; returns how many were set */
{
    char line[512];
    int count = 0;
    FILE *f = fopen(HZ_APP "/env.txt", "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char *eq, *end = line + strlen(line);
        while (end > line && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
        if (!line[0] || line[0] == '#' || !(eq = strchr(line, '='))) continue;
        *eq = 0;
        if (setenv(line, eq + 1, 1) == 0) ++count;
    }
    fclose(f);
    return count;
}
/* Settings are in place before any other static constructor runs. Parts of the engine read
 * theirs from a constructor (the SPU job recorder's PS3_JOBSTAT, the speculative float cap, the
 * Edge stage hooks): console run 12 had PS3_JOBSTAT=1 in env.txt and printed no statistics,
 * because main() loaded the file after those had already looked. libnx mounts the card before
 * it runs constructors, and priority 102 comes right after the thread pointer set-up (101). */
static int s_env_from_file = -1;
__attribute__((constructor(102))) static void hz_environment_early(void)
{
    default_environment();
    s_env_from_file = file_environment();
}
static unsigned long long env_number(const char *name, unsigned long long fallback)
{
    const char *value = getenv(name);
    return value && *value ? strtoull(value, NULL, 0) : fallback;
}

/* ---- faults: say where, on the screen and in the log, instead of the system's crash screen ----- */
alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof __nx_exception_stack;
static int s_fault_fd = -1;
static volatile int s_faulted;
static void fault_line(const char *format, ...)
{
    char line[200];
    va_list args;
    int len;
    va_start(args, format);
    len = vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (len <= 0) return;
    if (len >= (int)sizeof line) len = (int)sizeof line - 1;
    write(STDOUT_FILENO, line, (size_t)len);
    if (s_fault_fd >= 0) write(s_fault_fd, line, (size_t)len);
}
void __libnx_exception_handler(ThreadExceptionDump *ctx)
{
    int committed = 0;
    const int in_window = hz_mman_query((const void *)ctx->far.x, &committed);
    s_faulted = 1;
    /* the fault block takes rows 11..26 of the screen; the log lines below it stay readable */
    for (int row = 11; row <= 26; ++row) {
        char move[16];
        const int len = snprintf(move, sizeof move, "\x1b[%d;1H\x1b[K", row);
        write(STDOUT_FILENO, move, (size_t)len);
    }
    write(STDOUT_FILENO, "\x1b[10;1H", 7);
    fault_line("\n*** FAULT type 0x%x  pc %016lx  lr %016lx  far %016lx\n", ctx->error_desc,
               (unsigned long)ctx->pc.x, (unsigned long)ctx->lr.x, (unsigned long)ctx->far.x);
    fault_line("    sp %016lx  code base ref %016lx (address of the Switch entry, see the link map)\n",
               (unsigned long)ctx->sp.x, (unsigned long)(uintptr_t)&__libnx_exception_handler);
    if (in_window)
        fault_line("    the address is in the guest window: %s\n",
                   committed ? "memory is behind it (a real protection fault)" : "NO memory behind it (range not committed)");
    for (int i = 0; i < 28; i += 4)
        fault_line("    x%-2d %016lx %016lx %016lx %016lx\n", i, (unsigned long)ctx->cpu_gprs[i].x,
                   (unsigned long)ctx->cpu_gprs[i + 1].x, (unsigned long)ctx->cpu_gprs[i + 2].x,
                   (unsigned long)ctx->cpu_gprs[i + 3].x);
    fault_line("    x28 %016lx  fp %016lx\n", (unsigned long)ctx->cpu_gprs[28].x, (unsigned long)ctx->fp.x);
    fault_line("    guest memory %llu MiB, failed slab commits %llu (kernel result 0x%llx)\n",
               (unsigned long long)(hz_mman_stats[0] >> 20), (unsigned long long)hz_mman_stats[1],
               (unsigned long long)hz_mman_stats[2]);
    fault_line("*** stopped. Logs are in " HZ_APP "/logs. Press HOME to leave.\n");
    if (s_fault_fd >= 0) fsync(s_fault_fd);
    text_console_refresh();
    for (;;) svcSleepThread(1000000000ull);
}

void hz_thread_report(void (*line)(const char *format, ...));
long hz_spu_float_selftest(long vectors, long *checked);
extern long hz_spu_spec_ok;
uint64_t hz_profile_run(const char *path, unsigned seconds, unsigned rate, uintptr_t code_ref);
static void log_line(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

/* ---- the game thread --------------------------------------------------------------------------- */
static volatile int s_game_done;
static int s_game_result;
static void *game_thread(void *unused)
{
    static char name[] = "Infamous2Recomp", elf[] = HZ_APP "/EBOOT.elf";
    char *argv[] = {name, elf, NULL};
    (void)unused;
    s_game_result = __real_main(2, argv);
    __atomic_store_n(&s_game_done, 1, __ATOMIC_RELEASE);
    return NULL;
}
static long long file_bytes(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long long)st.st_size : -1;
}


/* Where the limit is, measured instead of assumed (asked for after run 10):
 *  - the CPU clock the console really runs this program at (sys-clk works per title, so "the
 *    overclock is on" may not hold for a forwarder's title id);
 *  - how busy each core really is. The sampling profiler counts a thread as busy when it is
 *    runnable, which says nothing about cores; the kernel's idle tick counters do. Horizon only
 *    answers for the core the caller is on, so this thread visits each allowed core in turn. */
static unsigned module_clock_mhz(PcvModuleId module_id, PcvModule legacy_module)
{
    u32 hz = 0;
    if (R_SUCCEEDED(clkrstInitialize())) {
        ClkrstSession session;
        if (R_SUCCEEDED(clkrstOpenSession(&session, module_id, 3))) {
            const int read_ok = R_SUCCEEDED(clkrstGetClockRate(&session, &hz));
            clkrstCloseSession(&session);
            clkrstExit();
            if (read_ok) return (unsigned)(hz / 1000000u);
        } else {
            clkrstExit();
        }
    }
    hz = 0;
    if (R_SUCCEEDED(pcvInitialize())) {
        if (R_FAILED(pcvGetClockRate(legacy_module, &hz))) hz = 0;
        pcvExit();
    }
    return (unsigned)(hz / 1000000u);
}

static unsigned cpu_mhz(void)
{
    static int state;               /* 0 = not tried, 1 = clkrst, 2 = pcv, -1 = neither */
    static ClkrstSession session;
    u32 hz = 0;
    if (state == 0) {
        if (R_SUCCEEDED(clkrstInitialize()) && R_SUCCEEDED(clkrstOpenSession(&session, PcvModuleId_CpuBus, 3))) state = 1;
        else if (R_SUCCEEDED(pcvInitialize())) state = 2;
        else state = -1;
    }
    if (state == 1 && R_FAILED(clkrstGetClockRate(&session, &hz))) hz = 0;
    if (state == 2 && R_FAILED(pcvGetClockRate(PcvModule_CpuBus, &hz))) hz = 0;
    return (unsigned)(hz / 1000000u);
}
static u64 s_idle_ticks[4], s_idle_at[4];
static int s_core_busy[4] = { -1, -1, -1, -1 };     /* percent since the previous call, -1 = unknown */
static void core_usage_sample(void)
{
    for (int core = 0; core < 4; ++core) {
        u64 idle = 0, now;
        s_core_busy[core] = -1;
        if (!(hz_core_mask & (1ull << core))) continue;
        if (R_FAILED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core))) continue;
        now = armGetSystemTick();
        if (R_SUCCEEDED(svcGetInfo(&idle, InfoType_IdleTickCount, INVALID_HANDLE, (u64)-1)) && s_idle_at[core]) {
            const u64 span = now - s_idle_at[core], rested = idle - s_idle_ticks[core];
            s_core_busy[core] = span ? (int)(100 - (rested > span ? span : rested) * 100 / span) : -1;
        }
        s_idle_ticks[core] = idle;
        s_idle_at[core] = now;
    }
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, -2, (u32)hz_core_mask);
}
int __wrap_main(int argc, char **argv)
{
    static const char spinner[] = "|/-\\";
    pthread_attr_t attr;
    pthread_t game;
    PadState pad;
    u64 start, total_memory = 0, used_memory = 0;
    int spu_selftest_failed = 0;
    int from_file, tick = 0, fast_tick = 0, keep_tick = 0, stall_reports = 0;
    int combo_was = 0;

    mkdir(HZ_APP, 0777);
    mkdir(HZ_APP "/logs", 0777);
    mkdir(HZ_APP "/shader_cache", 0777);
    /* settings first: whether this is the no-logs variant decides how the streams are set up */
    if (s_env_from_file <= 0) hz_environment_early();   /* again, in case the card was not readable that early */
    from_file = s_env_from_file;
    {
        const char *self = argc > 0 && argv && argv[0] ? argv[0] : "";
        const char *name = strrchr(self, '/');
        name = name ? name + 1 : self;
        s_quiet = strstr(name, "quiet") != NULL || strstr(name, "nolog") != NULL;
        if (getenv("HZ_QUIET")) s_quiet = (int)env_number("HZ_QUIET", 0) != 0;
        if (s_quiet) {
            unsetenv("PS3RECOMP_PLAYLOG"); unsetenv("AUDIO_PEAK");
            /* Both launchers share env.txt; the route probes have no consumer in quiet mode. */
            unsetenv("PS3RECOMP_SHADOW_TIMING");
            unsetenv("PS3RECOMP_ROUTE_TIMING");
            setenv("PS3RECOMP_ENG_TEX_TIMING", "0", 0);
        }
    }
    /* The console keeps descriptors 1 and 2; the stdout and stderr STREAMS are pointed at log files
     * on descriptors of their own. Not freopen(): it closes descriptor 1 and the file then gets
     * that number back, so everything written "to the screen" landed in stdout.log and the screen
     * stayed black (console runs 1 and 2, hz-0.1 / hz-0.2). */
    s_text_console_enabled = env_number("HZ_TEXT_CONSOLE", 1) != 0;
    text_console_start();
    s_log_stdout = stdout;
    stream_to_log(stdout, open_log_sink(LOG_OUT, HZ_APP "/logs/stdout.log"));
    stream_to_log(stderr, open_log_sink(LOG_ERR, HZ_APP "/logs/stderr.log"));
    s_fault_fd = open(HZ_APP "/logs/fault.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (!s_quiet) {
        const char *playlog = getenv("PS3RECOMP_PLAYLOG");
        open_log_sink(LOG_PLAY, playlog);
        start_log_writer();
        screen("diagnostic writer %s, stdout %s, stderr %s, playlog %s\n",
               s_writer_started ? "background" : "status fallback",
               s_logs[LOG_OUT] ? "open" : "unavailable", s_logs[LOG_ERR] ? "open" : "unavailable",
               s_logs[LOG_PLAY] ? "open" : "off/unavailable");
        fprintf(stderr, "[hz-log] writer=%s stdout=%d stderr=%d playlog=%d capacity=%u per stream\n",
                s_writer_started ? "background" : "status", !!s_logs[LOG_OUT], !!s_logs[LOG_ERR],
                !!s_logs[LOG_PLAY], HZ_LOG_CAPACITY);
    }
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    /* any controller id: no REPORT fired in runs 14-23 (hz-0.18 .. hz-0.24) with only player 1 +
     * handheld read here, while the game's SDL input layer sets the controllers up its own way */
    padInitializeAny(&pad);
    {   /* HZ_PROFILE_AT=120,300: a REPORT and a 20 s profile at those seconds, no buttons needed */
        const char *at = getenv("HZ_PROFILE_AT");
        while (at && *at && s_profile_at_count < 8) {
            char *end;
            const unsigned long v = strtoul(at, &end, 10);
            if (end == at) break;
            s_profile_at[s_profile_at_count++] = (unsigned)v;
            at = *end ? end + 1 : end;
        }
    }

    hz_mman_commit_cap = env_number("HZ_GUEST_CAP_MB", 1792) << 20;
    /* 8 MiB: the deepest guest thread used 2.4 MiB of host stack in console run 7 (cap was 4) */
    hz_stack_cap_bytes = (size_t)(env_number("HZ_STACK_CAP_MB", 8) << 20);
    hz_core_mode = (int)env_number("HZ_CORE_MODE", 2);
    hz_thread_priority = (int)env_number("HZ_THREAD_PRIORITY", 59);
    hz_affinity_mask = env_number("HZ_AFFINITY_MASK", 0);
    hz_lock_spins = (int)env_number("HZ_LOCK_SPINS", 96);
    hz_lock_yields = (int)env_number("HZ_LOCK_YIELDS", 6);
    svcGetInfo(&hz_core_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    /* Guest ranges that get memory at once (no commit on first touch yet): the executable and its
     * data, the main stack and guest TLS, the lifted firmware, RSX local memory, thread stacks.
     * The sys_memory heap at 0x40000000 commits itself through vm_commit. */
    hz_mman_precommit[0] = (struct hz_precommit_range){0x00000000u, 0x11000000u};
    hz_mman_precommit[1] = (struct hz_precommit_range){0x30000000u, 0x00400000u};
    hz_mman_precommit[2] = (struct hz_precommit_range){0xC0000000u, 0x10000000u};
    hz_mman_precommit[3] = (struct hz_precommit_range){0xD0000000u, 0x02000000u};
    /* the engine's HLE-visible GCM window (cellGcmSys.c ppu_hle_inject_base: labels, control block,
     * offset tables). Console run 1 (hz-0.1) faulted on its first write, at guest 0x20002000. */
    hz_mman_precommit[4] = (struct hz_precommit_range){0x20000000u, 0x00200000u};

    svcGetInfo(&total_memory, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used_memory, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    screen("inFAMOUS 2 port, Switch boot probe " HZ_VERSION "%s\n", s_quiet ? "  [quiet; crash reporting active]" : "  [logging]");
    screen("folder  " HZ_APP "\n");
    screen("memory  %llu MiB for this process, %llu MiB used at start%s\n", (unsigned long long)(total_memory >> 20),
           (unsigned long long)(used_memory >> 20),
           total_memory < 0x60000000ull ? "  <-- APPLET MODE: too little, start through a game title" : "");
    screen("config  %d setting(s) from env.txt, guest cap %llu MiB, stack cap %llu MiB\n", from_file,
           (unsigned long long)(hz_mman_commit_cap >> 20), (unsigned long long)(hz_stack_cap_bytes >> 20));
    screen("cores   allowed mask 0x%llx (%d core(s))%s\n", (unsigned long long)hz_core_mask,
           __builtin_popcountll(hz_core_mask), (hz_core_mask & 8) ? ", core 3 unlocked" : ", core 3 not available (no forwarder)");
    fprintf(stderr, "[hz] core mask 0x%llx, core mode %d\n", (unsigned long long)hz_core_mask, hz_core_mode);
    {
        u64 program_id = 0;
        const Result result = svcGetInfo(&program_id, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0);
        if (R_SUCCEEDED(result))
            fprintf(stderr, "[hz] program id %016llX (sys-clk profile key)\n", (unsigned long long)program_id);
        else
            fprintf(stderr, "[hz] program id unavailable (result=0x%X)\n", (unsigned)result);
    }
    screen("clock   CPU %u MHz at start (1020 = stock, 1785 = sys-clk maximum; read again every 10 s in the log)\n", cpu_mhz());
    screen("EBOOT   %s\n", file_bytes(HZ_APP "/EBOOT.elf") > 0 ? "found" : "MISSING: " HZ_APP "/EBOOT.elf");
    screen("disc    %s\n", file_bytes(HZ_APP "/game/disc/PS3_GAME/PARAM.SFO") > 0 ? "found" : "not on the card (boot stops at the first disc file)");
    text_console_refresh();
    fprintf(stderr, "[hz] " HZ_VERSION " process memory %llu MiB, env.txt settings %d\n",
            (unsigned long long)(total_memory >> 20), from_file);
    /* The SPU float forms of this build against the exact ones, on this CPU (hz_spu_selftest.c).
     * A difference means wrong game arithmetic, so the game is not started on one. */
    {
        long checked = 0;
        const u64 t0 = armGetSystemTick();
        const long bad = hz_spu_float_selftest(env_number("HZ_SPU_SELFTEST_VECTORS", 200000), &checked);
        const unsigned ms = (unsigned)(armTicksToNs(armGetSystemTick() - t0) / 1000000ull);
        screen("spu     float self-test: %ld results checked in %u ms, %ld differ%s\n", checked, ms, bad,
               bad ? "  <-- BUILD IS WRONG, game not started" : " (good)");
        fprintf(stderr, "[hz] spu float self-test: %ld results checked in %u ms, %ld differ, %ld speculative vectors passed their verdict\n", checked, ms, bad, hz_spu_spec_ok);
        text_console_refresh();
        spu_selftest_failed = bad != 0;
    }
    {   /* hz-0.32: the NEON vmaddfp / vnmsubfp of ppu_vmx.h against their scalar forms (hz_vmx_selftest.c) */
        extern long hz_vmx_selftest(long vectors, long* checked);
        long checked = 0;
        const u64 t0 = armGetSystemTick();
        const long bad = hz_vmx_selftest(env_number("HZ_VMX_SELFTEST_VECTORS", 250000), &checked);
        const unsigned ms = (unsigned)(armTicksToNs(armGetSystemTick() - t0) / 1000000ull);
        screen("vmx     self-test: %ld lanes checked in %u ms, %ld differ%s\n", checked, ms, bad,
               bad ? "  <-- BUILD IS WRONG, game not started" : " (good)");
        fprintf(stderr, "[hz] vmx self-test: %ld lanes checked in %u ms, %ld differ\n", checked, ms, bad);
        text_console_refresh();
        if (bad) spu_selftest_failed = 1;
    }

    if (!s_quiet) hz_target_configure(getenv("HZ_TARGET_DRAWS"));
    pthread_attr_init(&attr);
    __real_pthread_attr_setstacksize(&attr, (size_t)(env_number("HZ_MAIN_STACK_MB", 16) << 20));
    if (spu_selftest_failed) {
        __atomic_store_n(&s_game_done, 1, __ATOMIC_RELEASE);
        s_game_result = -2;
    } else if (pthread_create(&game, &attr, game_thread, NULL) != 0) {
        screen("could not start the game thread (errno %d)\n", errno);
        __atomic_store_n(&s_game_done, 1, __ATOMIC_RELEASE);
        s_game_result = -1;
    }
    pthread_attr_destroy(&attr);

    start = armGetSystemTick();
    while (appletMainLoop()) {
        const unsigned seconds = (unsigned)(armTicksToNs(armGetSystemTick() - start) / 1000000000ull);
        const int done = __atomic_load_n(&s_game_done, __ATOMIC_ACQUIRE);
        const int request = __atomic_load_n(&hz_screen_request, __ATOMIC_ACQUIRE);
        padUpdate(&pad);
        /* The screen is one window: the text console has it until the renderer opens its surface
         * (hz_window.c asks, this thread lets go), and gets it back when that surface is dropped
         * or the game ends. All console calls stay on this thread. */
        if (request == 1) text_console_stop();
        if ((request == 2 || done) && !s_screen_owned) { text_console_start(); fast_tick = 0; }
        if (request) __atomic_store_n(&hz_screen_request, 0, __ATOMIC_RELEASE);
        if (done && !s_text_console_enabled) break;
        if (s_screen_owned && !s_faulted && (fast_tick++ % 5) == 0) {
            const uint32_t count = __atomic_load_n(&s_ring_count, __ATOMIC_ACQUIRE);
            fflush(stdout); fflush(stderr);
            /* row 11: the status line; rows 13..: the newest lines the game printed */
            screen("\x1b[11;1H%c %02u:%02u  log %llu KiB, %u lines  guest %llu MiB  threads/core %llu %llu %llu %llu  %s\x1b[K",
                   spinner[tick++ & 3], seconds / 60, seconds % 60,
                   (unsigned long long)(__atomic_load_n(&s_log_bytes, __ATOMIC_RELAXED) / 1024), (unsigned)count,
                   (unsigned long long)(hz_mman_stats[0] >> 20), (unsigned long long)hz_core_threads[0],
                   (unsigned long long)hz_core_threads[1], (unsigned long long)hz_core_threads[2],
                   (unsigned long long)hz_core_threads[3], done ? "ENDED" : "running");
            for (uint32_t row = 0; row < HZ_SCREEN_LOG_ROWS; ++row) {
                const uint32_t shown = count < HZ_SCREEN_LOG_ROWS ? count : HZ_SCREEN_LOG_ROWS;
                char line[HZ_RING_WIDTH];
                line[0] = 0;
                if (row < shown) {
                    mutexLock(&s_ring_lock);
                    memcpy(line, s_ring[(count - shown + row) % HZ_RING_LINES], sizeof line);
                    mutexUnlock(&s_ring_lock);
                    line[HZ_SCREEN_COLUMNS] = 0;
                }
                screen("\x1b[%u;1H%s\x1b[K", 13u + row, line);
            }
            if (done) {
                screen("\x1b[45;1Hmain() returned %d (precommit failures %llu, failed slabs %llu). Press + to exit.\x1b[K",
                       s_game_result, (unsigned long long)hz_mman_precommit_failed, (unsigned long long)hz_mman_stats[1]);
                text_console_refresh();
                while (appletMainLoop()) {
                    padUpdate(&pad);
                    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
                    text_console_refresh();
                    svcSleepThread(50000000ull);
                }
                break;
            }
        }
        if (s_screen_owned) text_console_refresh();
        hz_target_poll(HZ_APP "/logs", (uintptr_t)&__libnx_exception_handler);
        hz_game_step_patch();
        hz_detail_patch();
        if (!s_quiet && ++keep_tick % 20 == 0) {
            fflush(stdout); fflush(stderr);
            if (!s_writer_started) flush_logs();
            if (keep_tick % 100 == 0) {
                unsigned long long dropped = 0, errors = 0;
                for (int i = 0; i < LOG_COUNT; ++i) {
                    dropped += hz_log_dropped(s_logs[i]);
                    errors += hz_log_errors(s_logs[i]);
                }
                fprintf(stderr, "[hz-log] dropped_bytes=%llu io_errors=%llu\n", dropped, errors);
                core_usage_sample();
                fprintf(stderr, "[hz] t=%us CPU %u MHz, cores busy %d%% %d%% %d%% %d%% over the last 10 s\n", seconds, cpu_mhz(),
                        s_core_busy[0], s_core_busy[1], s_core_busy[2], s_core_busy[3]);
                fprintf(stderr, "[hz-clocks] t=%us GPU %u MHz, memory %u MHz (0=unavailable; read-only)\n", seconds,
                        module_clock_mhz(PcvModuleId_GPU, PcvModule_GPU),
                        module_clock_mhz(PcvModuleId_EMC, PcvModule_EMC));
            }
            if (keep_tick % 100 == 0)
                fprintf(stderr, "[hz] t=%us guest %llu MiB, locks queued %llu, stack clamps %llu, audio: %llu buffers played, %llu frames dropped, %llu append failures; threads %llu made, %llu reaped, %llu failed\n",
                        seconds, (unsigned long long)(hz_mman_stats[0] >> 20), (unsigned long long)hz_lock_queued,
                        (unsigned long long)hz_stack_clamped_count, (unsigned long long)hz_audio_stats[0],
                        (unsigned long long)hz_audio_stats[1], (unsigned long long)hz_audio_stats[2],
                        (unsigned long long)hz_pthread_stats[0], (unsigned long long)hz_pthread_stats[1], (unsigned long long)hz_pthread_stats[2]);
        }
        /* MINUS and PLUS held together: write where every thread is right now (the "it looks
         * stuck" button; at most 8 reports a run). The quiet-log trigger of hz-0.8 never fired:
         * something always prints. */
        {
            const u64 held = padGetButtons(&pad);
            const int combo = (held & HidNpadButton_Minus) && (held & HidNpadButton_Plus);
            int timed = 0;
            static u64 seen_logged;
            if (held && !(seen_logged & 1)) {   /* once: proof this loop sees the controller at all */
                seen_logged |= 1;
                fprintf(stderr, "[hz] pad: main loop sees buttons 0x%llx at t=%us\n", (unsigned long long)held, seconds);
            }
            if ((held & HidNpadButton_Minus) && !(seen_logged & 2)) {
                seen_logged |= 2;
                fprintf(stderr, "[hz] pad: MINUS seen at t=%us (held 0x%llx)\n", seconds, (unsigned long long)held);
            }
            for (int k = 0; k < s_profile_at_count; ++k)
                if (s_profile_at[k] && seconds >= s_profile_at[k]) { s_profile_at[k] = 0; timed = 1; }
            /* hz-0.31: a frame stall (no picture presented for HZ_STALL_MS, default 3000; 0 = off) takes
             * a REPORT and a 10 s profile by itself, at most 3 a run: the multi-second freezes of runs 24,
             * 26, 29 and 33 (GCM semaphore given up, SPUs idle) had never been caught with thread stacks. */
            int stalled = 0;
            {
                static uint64_t last_presents, last_change_tick;
                static int stall_reports_taken, stall_ms = -1, armed = 1;
                const uint64_t p = __atomic_load_n(&hz_presents, __ATOMIC_RELAXED);
                const uint64_t now = armGetSystemTick();
                if (stall_ms < 0) stall_ms = (int)env_number("HZ_STALL_MS", 3000);
                if (p != last_presents) { last_presents = p; last_change_tick = now; armed = 1; }
                else if (stall_ms > 0 && p > 100 && armed && !done && !s_screen_owned && stall_reports_taken < 3 &&
                         armTicksToNs(now - last_change_tick) >= (uint64_t)stall_ms * 1000000ull) {
                    stalled = 1; armed = 0; ++stall_reports_taken;   /* one report per stall */
                }
            }
            if (((combo && !combo_was) || timed || stalled) && stall_reports < 8 && !s_quiet) {
                ++stall_reports;
                fprintf(stderr, "[hz] REPORT %d at t=%us (%s): where the threads are (code ref %016lx)\n",
                        stall_reports, seconds, stalled ? "FRAME STALL: no picture for HZ_STALL_MS" : timed ? "HZ_PROFILE_AT" : "MINUS+PLUS",
                        (unsigned long)(uintptr_t)&__libnx_exception_handler);
                hz_thread_report(log_line);
                fflush(stderr); hz_log_flush(s_logs[LOG_ERR]);
                {   /* then 20 s of sampling at 100 Hz (10 s for a stall): where the time goes (tools: hz_profile.py) */
                    char path[96];
                    uint64_t taken;
                    snprintf(path, sizeof path, HZ_APP "/logs/profile-%d.bin", stall_reports);
                    taken = hz_profile_run(path, stalled ? 10 : 20, 100, (uintptr_t)&__libnx_exception_handler);
                    fprintf(stderr, "[hz] PROFILE %d: %llu samples in %s\n", stall_reports, (unsigned long long)taken, path);
                    fflush(stderr); hz_log_flush(s_logs[LOG_ERR]);
                }
            }
            combo_was = combo;
        }
        svcSleepThread(100000000ull);
    }
    fflush(stdout); fflush(stderr);
    if (!s_quiet) flush_logs();
    text_console_stop();
    return s_game_result;
}
