/* Horizon layer: <signal.h> over newlib's. Horizon delivers no POSIX signals, so the engine's
 * handlers (fault reporters, the thread-park signal, write-watch) can be registered but never run:
 * sigaction() here always fails with ENOSYS and callers take their "could not install" path.
 * newlib's struct sigaction has no sa_sigaction and its siginfo_t no si_addr; both are supplied
 * here so that code compiles unchanged. */
#ifndef HZ_SIGNAL_H
#define HZ_SIGNAL_H
#include_next <signal.h>
#ifdef __cplusplus
extern "C" {
#endif
#ifndef SA_SIGINFO
#  define SA_SIGINFO 0x2
#endif
#ifndef SA_RESTART
#  define SA_RESTART 0x10000000
#endif
#ifndef SA_ONSTACK
#  define SA_ONSTACK 0x08000000
#endif
#ifndef SA_NODEFER
#  define SA_NODEFER 0x40000000
#endif
#ifndef SEGV_MAPERR
#  define SEGV_MAPERR 1
#  define SEGV_ACCERR 2
#endif
#ifndef BUS_ADRALN
#  define BUS_ADRALN 1
#  define BUS_ADRERR 2
#  define BUS_OBJERR 3
#endif
#ifndef ILL_ILLOPC
#  define ILL_ILLOPC 1
#  define ILL_ILLOPN 2
#  define ILL_ILLADR 3
#  define ILL_ILLTRP 4
#  define ILL_PRVOPC 5
#  define ILL_PRVREG 6
#  define ILL_COPROC 7
#  define ILL_BADSTK 8
#endif
#ifndef FPE_INTDIV
#  define FPE_INTDIV 1
#  define FPE_INTOVF 2
#  define FPE_FLTDIV 3
#  define FPE_FLTOVF 4
#  define FPE_FLTUND 5
#  define FPE_FLTRES 6
#  define FPE_FLTINV 7
#  define FPE_FLTSUB 8
#endif
/* the faulting address: newlib's siginfo_t carries only si_value */
#define si_addr si_value.sival_ptr

struct hz_sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t *, void *);
    };
    sigset_t sa_mask;
    int sa_flags;
};
int hz_sigaction(int sig, const struct hz_sigaction *act, struct hz_sigaction *old);
/* both the struct tag and the function: `struct sigaction sa; sigaction(SIGSEGV, &sa, 0);` */
#define sigaction hz_sigaction
#ifdef __cplusplus
}
#endif
#endif
