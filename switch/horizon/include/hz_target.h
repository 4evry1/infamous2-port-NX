#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum hz_target_phase {
    HZ_WAIT, HZ_WAKE, HZ_TICK, HZ_TICK_DONE, HZ_PRESENT, HZ_PRESENT_DONE,
    HZ_DRAIN, HZ_DRAIN_DONE, HZ_PUMP, HZ_PUMP_DONE, HZ_FRAME, HZ_FLIP_CHECK, HZ_FLIP_CHECK_DONE
};
void hz_target_configure(const char *draw_setting);
void hz_target_frame(unsigned frame, unsigned draws);
void hz_target_mark(enum hz_target_phase phase);
void hz_target_poll(const char *directory, uintptr_t code_ref);
#ifdef __cplusplus
}
#endif
