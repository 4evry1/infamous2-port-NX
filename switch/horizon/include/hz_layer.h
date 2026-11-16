/* Horizon layer: what its own sources share (not seen by the engine). */
#ifndef HZ_LAYER_H
#define HZ_LAYER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define HZ_PRECOMMIT_MAX 16
struct hz_precommit_range { uint32_t guest; uint32_t length; };
extern struct hz_precommit_range hz_mman_precommit[HZ_PRECOMMIT_MAX];
extern uint64_t hz_mman_precommit_failed;
extern uint64_t hz_mman_commit_cap;
extern uint64_t hz_mman_stats[3];        /* committed bytes, failed slab commits, last kernel result */
int hz_mman_commit(void *address, size_t length);
int hz_mman_query(const void *address, int *committed);
extern size_t hz_stack_cap_bytes;
extern uint64_t hz_stack_clamped_count;
extern int hz_core_mode;
extern uint64_t hz_core_mask;
extern uint64_t hz_core_threads[4];
extern int hz_thread_priority;
extern uint64_t hz_affinity_mask;
extern uint64_t hz_sched_failures;
extern int hz_lock_spins, hz_lock_yields;
extern uint64_t hz_lock_queued;
extern uint64_t hz_pthread_stats[3];     /* pthreads created, detached ones reaped, failed creations */
extern uint64_t hz_audio_stats[4];       /* buffers played, frames dropped, append failures, started */
#ifdef __cplusplus
}
#endif
#endif
