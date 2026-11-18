#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
typedef int PcvModuleId;
typedef int PcvModule;
typedef struct { int open; } ClkrstSession;
#define R_SUCCEEDED(r) ((r) == 0)
#define R_FAILED(r) ((r) != 0)
static int fail_init, fail_open, fail_get, fail_pcv_init, fail_pcv_get;
static int service_refs, session_refs, pcv_refs, pcv_queries;
static int clkrstInitialize(void) { if (fail_init) return 1; ++service_refs; return 0; }
static void clkrstExit(void) { assert(service_refs > 0); --service_refs; }
static int clkrstOpenSession(ClkrstSession *s, PcvModuleId id, int version) {
    assert(id == 42 && version == 3 && service_refs == 1);
    if (fail_open) return 1;
    s->open = 1; ++session_refs; return 0;
}
static int clkrstGetClockRate(ClkrstSession *s, u32 *hz) {
    assert(s->open); *hz = 921000000; return fail_get;
}
static void clkrstCloseSession(ClkrstSession *s) { assert(s->open); s->open = 0; --session_refs; }
static int pcvInitialize(void) { if (fail_pcv_init) return 1; ++pcv_refs; return 0; }
static void pcvExit(void) { assert(pcv_refs > 0); --pcv_refs; }
static int pcvGetClockRate(PcvModule id, u32 *hz) {
    assert(id == 7 && pcv_refs == 1); ++pcv_queries; *hz = 800000000; return fail_pcv_get;
}
#include "production.inc"
int main(void) {
    for (int mask = 0; mask < 32; ++mask) {
        fail_init = mask & 1; fail_open = mask & 2; fail_get = mask & 4;
        fail_pcv_init = mask & 8; fail_pcv_get = mask & 16; pcv_queries = 0;
        const int primary_ok = !fail_init && !fail_open && !fail_get;
        const unsigned expected = primary_ok ? 921 : (!fail_pcv_init && !fail_pcv_get ? 800 : 0);
        assert(module_clock_mhz(42, 7) == expected);
        assert(service_refs == 0 && session_refs == 0 && pcv_refs == 0);
        assert(pcv_queries == (!primary_ok && !fail_pcv_init));
    }
    puts("PASS: all32 clock-read success/failure combinations balance services and sessions");
}
