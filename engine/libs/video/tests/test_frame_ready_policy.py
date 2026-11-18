"""Compile the actual Switch frame-clock wait branch with observable boundary calls."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "work/port-switch/main.cpp").read_text()
option_start = source.index('    const char* ready_skip_env = getenv("PS3RECOMP_GCM_READY_SKIP");')
option_end = source.index("#endif", option_start)
wait_start = source.index("#elif defined(__SWITCH__)\n") + len("#elif defined(__SWITCH__)\n")
wait_end = source.index("#else", wait_start)
options = source[option_start:option_end]
wait = source[wait_start:wait_end]
gcm = (root / "engine/libs/video/cellGcmSys.c").read_text()
route_start = gcm.index("void cellGcm_route_sleep(")
route_sleep = gcm[route_start:gcm.index("\n}", route_start) + 2]
program = r"""
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
static int ready_result, ready_queries, sleeps;
static unsigned duration;
static int cellGcm_fifo_work_ready(void) { ++ready_queries; return ready_result; }
static int record_sleep(useconds_t us) { ++sleeps; duration = us; return 0; }
#define usleep record_sleep
static unsigned route_enabled;
static struct { unsigned check_ns, requested_us, wake_ns; int ready; } route_clock;
static unsigned ps3_ft_now(void) { return 1; }
static void cellGcm_route_enable(void) { ++route_enabled; }
ROUTE_SLEEP
static void frame_wait(int rsx_ok, int us) {
OPTIONS
WAIT
}
int main(void) {
    const char *settings[] = {NULL, "", "1", "0", "0-disabled"};
    const int durations[] = {1, 1000, 4000};
    unsigned cases = 0;
    for(unsigned route = 0; route < 2; ++route)
    for(unsigned i = 0; i < 5; ++i) {
        if(settings[i]) setenv("PS3RECOMP_GCM_READY_SKIP", settings[i], 1);
        else unsetenv("PS3RECOMP_GCM_READY_SKIP");
        for(int backend = 0; backend < 2; ++backend)
        for(int ready = 0; ready < 2; ++ready)
        for(unsigned d = 0; d < 3; ++d) {
            setenv("PS3RECOMP_ROUTE_TIMING", route ? "1" : "0", 1);
            route_enabled = route_clock.wake_ns = 0;
            ready_result = ready; ready_queries = sleeps = 0; duration = 0;
            frame_wait(backend, durations[d]);
            int enabled = i < 3;
            assert(route_enabled == route && route_clock.wake_ns == route);
            assert(ready_queries == (backend && enabled));
            assert(sleeps == !(backend && enabled && ready));
            if(sleeps) assert(duration == (unsigned)durations[d]);
            ++cases;
        }
    }
    printf("PASS: %u actual frame-wait branch cases; disabled/backend-down short circuit and exact idle durations\n", cases);
}
""".replace("ROUTE_SLEEP", route_sleep).replace("OPTIONS", options).replace("WAIT", wait)
with tempfile.TemporaryDirectory(prefix="frame-ready-policy-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(program)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu17", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
