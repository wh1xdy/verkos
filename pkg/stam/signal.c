/* stam — running a signal handler.
 *
 * Until now a signal was a way for a process to die: raised, noticed the next
 * time the kernel was in a position to act, and turned into an exit status
 * (D-075). That is the whole of what Ctrl-C needs, and it is not what a program
 * needs — a shell wants to be told its child was interrupted without being
 * interrupted itself, and a program that holds something wants to put it down
 * before it goes.
 *
 * Catching a signal means running a function in the program at a moment the
 * program did not choose, and then carrying on from exactly where it was. So
 * the interrupted state has to be written somewhere the handler cannot
 * casually destroy, and something has to bring it back. Both halves are here.
 *
 * ---- where the state goes ----
 *
 * On the program's own stack, below where it was. Not in the kernel: a handler
 * may raise the same signal again, or call something that blocks, and a single
 * per-process saved frame turns the second delivery into a lost first one. The
 * stack already nests correctly and the program already owns it.
 *
 * ---- how it gets back ----
 *
 * The handler returns to an address the kernel put in x30, which must be a few
 * instructions that call rt_sigreturn. Linux keeps that trampoline in the vDSO;
 * there is no vDSO here, so the program supplies it — the same arrangement
 * Linux uses on the architectures where SA_RESTORER is still how it works. A
 * sigaction without one is refused rather than accepted and left to jump
 * somewhere arbitrary when the handler returns.
 */
#include "sched.h"
#include "vm.h"
#include <stdint.h>

typedef struct {
    uint64_t x[30];
    uint64_t lr;
    uint64_t elr;
    uint64_t spsr;
    uint64_t sp_el0;
} regs_t;

/* What sig_deliver writes on the user stack and sig_return reads back.
 *
 * The magic is not decoration. sigreturn is an ordinary system call and any
 * program may make it with any stack pointer at all; without a marker the
 * kernel would load 34 registers out of whatever happened to be there. It does
 * not make the call safe — a program can write the magic itself — which is why
 * nothing below trusts the contents either. */
#define SIGFRAME_MAGIC  0x5354414d53494731ull   /* "STAMSIG1" */

typedef struct {
    uint64_t magic;
    uint64_t blocked;       /* what was held off before this handler ran */
    regs_t   saved;
} sigframe_t;

/* The running process. sched.c keeps `current` to itself, which is worth
 * keeping that way: this file has no business changing who is running. */
#define current thread_current()

int sig_action(int sig, uint64_t handler, uint64_t restorer, uint64_t mask) {
    if (sig <= 0 || sig >= NSIG) return -22;                  /* -EINVAL */
    /* Neither can be caught. A program that could catch SIGKILL would be a
     * program that cannot be stopped. */
    if (sig == SIGKILL) return -22;
    if (handler > 1) {                       /* 0 = default, 1 = ignore */
        if (!vm_user_ok(handler, 4, 0)) return -14;           /* -EFAULT */
        /* Refused here rather than discovered when the handler returns: a
         * handler that returns to zero is a program dying at an address that
         * has nothing to do with the mistake. */
        if (!restorer || !vm_user_ok(restorer, 4, 0)) return -14;
    }
    current->sigact[sig].handler  = handler;
    current->sigact[sig].restorer = restorer;
    current->sigact[sig].mask     = mask;
    return 0;
}

void sig_exec_reset(void) {
    for (int s = 1; s < NSIG; s++)
        if (current->sigact[s].handler > 1) {   /* caught: the address is gone */
            current->sigact[s].handler = 0;
            current->sigact[s].restorer = 0;
            current->sigact[s].mask = 0;
        }
    current->blocked = 0;
}

int sig_deliver(int sig, void *frameptr) {
    regs_t *f = frameptr;
    uint64_t h = current->sigact[sig].handler;

    /* Ignored: consume it and let the program carry on, having noticed
     * nothing. Told apart from "no handler" because the two have opposite
     * outcomes and the caller cannot see the difference. */
    if (h == 1) { current->pending &= ~(1u << sig); return 1; }
    if (h == 0) return 0;                       /* default: the caller kills */

    /* Below where the program was, aligned: AArch64 faults on a stack pointer
     * that is not a multiple of 16 at the moment it is used. */
    uint64_t sp = (f->sp_el0 - sizeof(sigframe_t)) & ~15ull;

    /* Its own stack, and it must be writable — a program whose stack pointer
     * has wandered gets an error, not a kernel writing wherever it points. */
    if (!vm_user_ok(sp, sizeof(sigframe_t), 1)) return 0;

    sigframe_t *sf = (sigframe_t *)(uintptr_t)sp;
    sf->magic   = SIGFRAME_MAGIC;
    sf->blocked = current->blocked;
    sf->saved   = *f;

    /* While the handler runs, this signal and whatever the program asked for
     * are held off. Without the signal itself the second Ctrl-C arrives on top
     * of the first handler and the stack grows one frame per keystroke. */
    current->blocked |= current->sigact[sig].mask | (1ull << sig);
    current->pending &= ~(1u << sig);

    f->sp_el0 = sp;
    f->elr    = h;                              /* into the handler          */
    f->lr     = current->sigact[sig].restorer;  /* and out through sigreturn */
    f->x[0]   = (uint64_t)sig;
    return 1;
}

uint64_t sig_return(void *frameptr) {
    regs_t *f = frameptr;
    uint64_t sp = f->sp_el0;

    if (!vm_user_ok(sp, sizeof(sigframe_t), 0)) return (uint64_t)-14;
    sigframe_t *sf = (sigframe_t *)(uintptr_t)sp;
    if (sf->magic != SIGFRAME_MAGIC) return (uint64_t)-14;

    regs_t saved = sf->saved;

    /* Everything except the processor state comes back verbatim; the program
     * is entitled to have edited any of it, and a handler that changes its own
     * return address is a legitimate thing to do.
     *
     * SPSR is the exception. It says which exception level to return to, and
     * it is on a page the program can write, so restoring it as found is a
     * program choosing to come back at EL1 — the kernel handing over the
     * machine because it read a number out of userspace. Only the condition
     * flags survive; the rest is rebuilt as EL0t with interrupts enabled,
     * which is the only state a program can be resumed in anyway. */
    uint64_t nzcv = saved.spsr & 0xf0000000ull;
    saved.spsr = nzcv;

    /* Nor its entry point: ELR is where execution resumes, and an address in
     * the kernel's half would be resumed with EL0's permissions, which faults
     * — but on a machine that had not yet checked. */
    if (!vm_user_ok(saved.elr, 4, 0)) return (uint64_t)-14;

    uint64_t blocked = sf->blocked;
    *f = saved;
    current->blocked = blocked;

    /* The value the interrupted code had in x0. handle_user_sync writes this
     * return value into the frame's x0, so returning anything else would undo
     * the restore in the register that matters most. */
    return f->x[0];
}
