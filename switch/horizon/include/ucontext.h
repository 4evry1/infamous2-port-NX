/* Horizon layer: <ucontext.h>. Types so the engine's fiber and fault-frame code compiles.
 * getcontext/makecontext/swapcontext live in horizon/src/hz_ucontext.c. */
#ifndef HZ_UCONTEXT_H
#define HZ_UCONTEXT_H
#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct hz_mcontext {
    uint64_t fault_address;
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
    uint64_t fpregs[2 * 32];   /* v0..v31 */
    uint32_t fpsr, fpcr;
} mcontext_t;
typedef struct ucontext_t {
    unsigned long uc_flags;
    struct ucontext_t *uc_link;
    stack_t uc_stack;
    mcontext_t uc_mcontext;
} ucontext_t;
int getcontext(ucontext_t *ucp);
int setcontext(const ucontext_t *ucp);
void makecontext(ucontext_t *ucp, void (*func)(void), int argc, ...);
int swapcontext(ucontext_t *oucp, const ucontext_t *ucp);
#ifdef __cplusplus
}
#endif
#endif
