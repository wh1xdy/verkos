/* stam — what a program may ask the kernel for.
 *
 * Linux's aarch64 numbers, deliberately: 64 is write, 93 is exit, 172 is
 * getpid. Inventing our own would be a decision we could only ever undo, and
 * the numbering is the cheapest part of the ABI to agree with.
 *
 * Three calls is not a system-call table so much as proof that the mechanism
 * works in both directions: a call that has an effect, a call that returns a
 * value, and a call that does not return at all.
 */
#include "console.h"
#include "sched.h"
#include "vm.h"
#include "mmu.h"
#include <stdint.h>

typedef struct {
    uint64_t x[30];
    uint64_t lr;
    uint64_t elr;
    uint64_t spsr;
} regs_t;

extern void exception_report(unsigned long kind, regs_t *r);

#define SYS_CLONE  220        /* aarch64 Linux has no fork; clone does both */
#define SYS_WRITE   64
#define SYS_EXIT    93
#define SYS_GETPID 172

static uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len) {
    if (fd != 1 && fd != 2) return (uint64_t)-9;        /* -EBADF */
    /* The buffer address comes from a program we do not trust. Nothing here
     * validates it yet, which is honest to note and wrong to leave: at the
     * moment the only program is one we wrote and mapped ourselves. Checking
     * it needs a notion of which pages belong to which process, and that
     * arrives with the second process. */
    const char *p = (const char *)(uintptr_t)buf;
    for (uint64_t i = 0; i < len; i++) kputc(p[i]);
    return len;
}

/* fork. The frame is passed in because the child needs a copy of it — this is
 * the one system call whose implementation is about the shape of the stack it
 * was called on rather than about its arguments. */
static uint64_t do_fork(void *frame) {
    uint64_t child_space = vm_copy_space(mmu_current_user_table_kva());
    if (!child_space) { kprintf("  [kernel] fork: no memory for a copy\n"); return (uint64_t)-12; }
    thread_t *child = thread_fork(frame, 272, child_space);
    if (!child) { kprintf("  [kernel] fork: no room for another process\n"); return (uint64_t)-11; }
    return (uint64_t)child->id;
}

static uint64_t do_syscall(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, void *frame) {
    switch (nr) {
    case SYS_CLONE: return do_fork(frame);
    case SYS_WRITE:  return sys_write(a, b, c);
    case SYS_GETPID: return (uint64_t)thread_current()->id;
    case SYS_EXIT:
        kprintf("  [kernel] the program exited with status %llu\n",
                (unsigned long long)a);
        thread_exit();                       /* does not return */
        return 0;
    default:
        kprintf("  [kernel] unknown system call %llu — returning -ENOSYS\n",
                (unsigned long long)nr);
        return (uint64_t)-38;                /* -ENOSYS */
    }
}

/* Everything synchronous that arrives from EL0 comes here: system calls, but
 * also a program's own faults. Only the first is expected. */
void handle_user_sync(regs_t *r) {
    uint64_t esr;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    if ((esr >> 26) == 0x15) {              /* SVC from AArch64 */
        r->x[0] = do_syscall(r->x[8], r->x[0], r->x[1], r->x[2], r);
        return;
    }
    /* A program that faults is killed; the machine is not. This is the point
     * of the whole address-space exercise — a program cannot take the system
     * down by being wrong, only itself. */
    exception_report(8, r);
    kprintf("  [kernel] killing the program; the kernel continues.\n");
    thread_exit();
}

/* Drop to EL0 and start running. SPSR of zero means: exception level 0, using
 * SP_EL0, with DAIF clear — interrupts enabled, so the timer can preempt a user
 * program exactly as it preempts a kernel thread. */
void enter_user(uint64_t entry, uint64_t stack_top) {
    __asm__ volatile(
        "msr sp_el0, %1\n"
        "msr spsr_el1, xzr\n"
        "msr elr_el1, %0\n"
        "eret\n"
        :: "r"(entry), "r"(stack_top) : "memory");
    __builtin_unreachable();
}
