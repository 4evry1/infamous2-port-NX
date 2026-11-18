"""Exercise the Horizon fault handler before any background drain or exit hooks can run."""
from pathlib import Path
import subprocess
import tempfile

src = Path(__file__).resolve().parents[1] / "horizon/src"
entry = (src / "hz_entry.c").read_text()
handler = entry[entry.index("static int s_fault_fd"):entry.index("void hz_thread_report(")]
host = r"""
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include "hz_log_batch.h"
#define HZ_APP "fixture"
typedef struct { uintptr_t x; } reg;
typedef struct { unsigned error_desc; reg pc,lr,far,sp,fp,cpu_gprs[29]; } ThreadExceptionDump;
static uint64_t hz_mman_stats[3];
static int sync_fd = -1;
static int hz_mman_query(const void *p, int *committed) { (void)p; *committed = 1; return 1; }
static void text_console_refresh(void) {}
static void svcSleepThread(uint64_t ns) { (void)ns; _exit(sync_fd >= 0 ? 42 : 1); }
int __real_fsync(int fd);
int __wrap_fsync(int fd) { int rc = __real_fsync(fd); if (!rc) sync_fd = fd; return rc; }
""" + handler + r"""
int main(int argc, char **argv) {
    (void)argc;
    s_fault_fd = open("fault.log", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(s_fault_fd >= 0);
    if (atoi(argv[1])) {
        int fd = open("routine.log", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        assert(fd >= 0);
        hz_log_sink *sink = hz_log_create(fd);
        assert(sink);
        hz_log_append(sink, "still in RAM", 12);
    }
    ThreadExceptionDump ctx = {.error_desc=7, .pc={0x1234}, .lr={0x5678}};
    __libnx_exception_handler(&ctx);
}
"""
with tempfile.TemporaryDirectory() as tmp:
    folder = Path(tmp)
    exe = folder / "fault-test"
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-pthread", "-I", src,
                    "-x", "c", "-", src / "hz_log_batch.c", "-Wl,--wrap=fsync", "-o", exe],
                   input=host, text=True, check=True)
    for logged in (0, 1):
        run = subprocess.run([exe, str(logged)], cwd=folder, stdout=subprocess.DEVNULL)
        assert run.returncode == 42
        fault = (folder / "fault.log").read_text()
        assert "FAULT type 0x7" in fault and "0000000000001234" in fault and "x28" in fault
        assert "Press HOME" in fault
        routine = folder / "routine.log"
        if logged:
            assert routine.read_bytes() == b""
        else:
            assert not routine.exists()
print("Fault report written and synced before termination in both modes")
