/* Horizon layer: <sys/syscall.h>. Only gettid is meaningful; see horizon/src/hz_misc.c. */
#ifndef HZ_SYS_SYSCALL_H
#define HZ_SYS_SYSCALL_H
#ifdef __cplusplus
extern "C" {
#endif
#define SYS_gettid 178
long syscall(long number, ...);
#ifdef __cplusplus
}
#endif
#endif
