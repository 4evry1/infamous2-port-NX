/* Horizon layer: getcontext / setcontext / swapcontext / makecontext for arm64 (cellFiber).
 * Cooperative switches only: the callee-saved state of the AAPCS64 (x19..x30, sp, d8..d15) is
 * saved and restored; x0..x7 are loaded on entry so makecontext's arguments arrive. x18 (the
 * platform register) is never touched. No signal mask (there are no signals). */
#include <ucontext.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* the offsets the assembly below uses */
_Static_assert(offsetof(ucontext_t, uc_link) == 8, "uc_link");
_Static_assert(offsetof(ucontext_t, uc_mcontext.regs) == 48, "regs");
_Static_assert(offsetof(ucontext_t, uc_mcontext.sp) == 296, "sp");
_Static_assert(offsetof(ucontext_t, uc_mcontext.pc) == 304, "pc");
_Static_assert(offsetof(ucontext_t, uc_mcontext.fpregs) == 320, "fpregs");

__asm__(
".text\n"
".macro HZ_SAVE ctx\n"
"    stp x19, x20, [\\ctx, #48 + 19*8]\n"
"    stp x21, x22, [\\ctx, #48 + 21*8]\n"
"    stp x23, x24, [\\ctx, #48 + 23*8]\n"
"    stp x25, x26, [\\ctx, #48 + 25*8]\n"
"    stp x27, x28, [\\ctx, #48 + 27*8]\n"
"    stp x29, x30, [\\ctx, #48 + 29*8]\n"
"    mov x9, sp\n"
"    str x9,  [\\ctx, #296]\n"
"    str x30, [\\ctx, #304]\n"          /* resumes at the caller's return address */
"    str xzr, [\\ctx, #48]\n"           /* ...where the call then returns 0 */
"    str d8,  [\\ctx, #320 + 8*16]\n"
"    str d9,  [\\ctx, #320 + 9*16]\n"
"    str d10, [\\ctx, #320 + 10*16]\n"
"    str d11, [\\ctx, #320 + 11*16]\n"
"    str d12, [\\ctx, #320 + 12*16]\n"
"    str d13, [\\ctx, #320 + 13*16]\n"
"    str d14, [\\ctx, #320 + 14*16]\n"
"    str d15, [\\ctx, #320 + 15*16]\n"
".endm\n"
".global getcontext\n.type getcontext, %function\n"
"getcontext:\n"
"    HZ_SAVE x0\n"
"    mov w0, #0\n"
"    ret\n"
".global setcontext\n.type setcontext, %function\n"
"setcontext:\n"
"    mov x16, x0\n"
"    ldr d8,  [x16, #320 + 8*16]\n"
"    ldr d9,  [x16, #320 + 9*16]\n"
"    ldr d10, [x16, #320 + 10*16]\n"
"    ldr d11, [x16, #320 + 11*16]\n"
"    ldr d12, [x16, #320 + 12*16]\n"
"    ldr d13, [x16, #320 + 13*16]\n"
"    ldr d14, [x16, #320 + 14*16]\n"
"    ldr d15, [x16, #320 + 15*16]\n"
"    ldp x19, x20, [x16, #48 + 19*8]\n"
"    ldp x21, x22, [x16, #48 + 21*8]\n"
"    ldp x23, x24, [x16, #48 + 23*8]\n"
"    ldp x25, x26, [x16, #48 + 25*8]\n"
"    ldp x27, x28, [x16, #48 + 27*8]\n"
"    ldp x29, x30, [x16, #48 + 29*8]\n"
"    ldr x9, [x16, #296]\n"
"    mov sp, x9\n"
"    ldr x17, [x16, #304]\n"
"    ldp x0, x1, [x16, #48 + 0*8]\n"
"    ldp x2, x3, [x16, #48 + 2*8]\n"
"    ldp x4, x5, [x16, #48 + 4*8]\n"
"    ldp x6, x7, [x16, #48 + 6*8]\n"
"    br x17\n"
".global swapcontext\n.type swapcontext, %function\n"
"swapcontext:\n"
"    HZ_SAVE x0\n"
"    mov x0, x1\n"
"    b setcontext\n"
/* first entry of a makecontext context: x19 = the function, x20 = uc_link */
".global hz_context_entry\n.type hz_context_entry, %function\n"
"hz_context_entry:\n"
"    blr x19\n"
"    mov x0, x20\n"
"    b hz_context_finished\n"
);

void hz_context_entry(void);

/* The context's function returned: go on in uc_link, or end the thread when there is none. */
void hz_context_finished(ucontext_t *link)
{
    if (link) setcontext(link);
    pthread_exit(NULL);
}

void makecontext(ucontext_t *ucp, void (*func)(void), int argc, ...)
{
    va_list args;
    uintptr_t top = ((uintptr_t)ucp->uc_stack.ss_sp + ucp->uc_stack.ss_size) & ~(uintptr_t)15;
    va_start(args, argc);
    for (int i = 0; i < 8; ++i)   /* int arguments, as POSIX has them; more than 8 are not passed */
        ucp->uc_mcontext.regs[i] = i < argc ? (uint64_t)(int64_t)va_arg(args, int) : 0;
    va_end(args);
    ucp->uc_mcontext.regs[19] = (uint64_t)(uintptr_t)func;
    ucp->uc_mcontext.regs[20] = (uint64_t)(uintptr_t)ucp->uc_link;
    ucp->uc_mcontext.regs[29] = 0;
    ucp->uc_mcontext.regs[30] = 0;
    ucp->uc_mcontext.sp = top;
    ucp->uc_mcontext.pc = (uint64_t)(uintptr_t)hz_context_entry;
}
