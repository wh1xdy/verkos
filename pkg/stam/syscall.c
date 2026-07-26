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
#include "cpio.h"
#include "elf.h"
#include "bootinfo.h"
#include <stdint.h>

typedef struct {
    uint64_t x[30];
    uint64_t lr;
    uint64_t elr;
    uint64_t spsr;
    uint64_t sp_el0;      /* saved with the rest: see vectors.S */
} regs_t;

extern void exception_report(unsigned long kind, regs_t *r);

#define SYS_WAIT4  260
#define SYS_EXECVE 221
#define SYS_CLONE  220        /* aarch64 Linux has no fork; clone does both */
#define SYS_READ    63
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

/* Where the initrd is, so exec has somewhere to look. Set once at boot. */
static uint64_t archive_addr, archive_size;
void syscall_set_archive(uint64_t addr, uint64_t size) {
    archive_addr = addr; archive_size = size;
}

typedef struct { uint64_t x[30]; uint64_t lr, elr, spsr, sp_el0; } frame_t;

/* exec: keep the process, replace everything in it.
 *
 * The trick is that this does not jump anywhere. It edits the exception frame
 * the system call will return through — the address to resume at, and the
 * stack — so the ordinary return to EL0 lands in the new program instead of
 * after the svc in the old one. The process id, the kernel stack and the place
 * in the scheduler all stay exactly as they were, which is what makes it the
 * same process running something else.
 *
 * The old address space is not reclaimed. Nothing here frees pages yet; that
 * is a real leak and it is written down rather than hidden.
 */
static uint64_t do_execve(const char *path, void *frameptr) {
    if (!archive_size) return (uint64_t)-2;                 /* -ENOENT */

    /* Copy the name out of the caller's memory before touching the address
     * space. The moment the new one is installed the old string is gone, and
     * the message that follows would print whatever now lives at that address
     * — which it did, on the first attempt. */
    char name[64];
    unsigned i = 0;
    for (; i < sizeof name - 1 && path && path[i]; i++) name[i] = path[i];
    name[i] = 0;

    uint64_t file = 0, len = 0;
    if (cpio_find(archive_addr, archive_size, name, &file, &len) != 0) {
        kprintf("  [kernel] exec: no '%s' in the initrd\n", name);
        return (uint64_t)-2;
    }
    uint64_t space = vm_new_space();
    if (!space) return (uint64_t)-12;
    uint64_t saved = mmu_current_user_table_kva();
    mmu_switch_user(space);

    uint64_t entry = 0;
    if (elf_load(file, len, &entry) != 0) {
        mmu_switch_user(saved);
        return (uint64_t)-8;                                /* -ENOEXEC */
    }
    void *stack = alloc_page();
    uint64_t stack_va = 0x7f00100000ul;
    if (!stack || map_user_page(stack_va, (uint64_t)(uintptr_t)stack, 0)) {
        mmu_switch_user(saved);
        return (uint64_t)-12;
    }
    thread_current()->space = space;

    frame_t *f = frameptr;
    f->elr    = entry;               /* return to the new program, not the old */
    f->spsr   = 0;                   /* EL0t, interrupts on                    */
    f->sp_el0 = stack_va + PAGE_SIZE;
    for (int i = 0; i < 30; i++) f->x[i] = 0;
    if (kchatty())
        kprintf("  [kernel] process %llu is now running %s\n",
                (unsigned long long)thread_current()->id, name);
    return 0;
}

/* A line from the console, with the editing a person expects: characters are
 * echoed as they arrive, backspace removes one, and the line is delivered when
 * Return is pressed. Doing this in the kernel is what a terminal driver is
 * for — a program should not have to know that a backspace is three bytes to
 * undo on screen.
 *
 * It waits by yielding, because nothing wakes a thread on input yet. */
static uint64_t sys_read(uint64_t fd, uint64_t buf, uint64_t len) {
    if (fd != 0) return (uint64_t)-9;                        /* -EBADF */
    if (!len) return 0;
    /* Somebody is reading from the console, so somebody is sitting at it. */
    kquiet(1);
    char *out = (char *)(uintptr_t)buf;
    uint64_t n = 0;
    for (;;) {
        int c = kgetc();
        if (c < 0) { thread_yield(); continue; }
        if (c == '\r' || c == '\n') { kputc('\n'); out[n++] = '\n'; return n; }
        if (c == 0x7f || c == 8) {           /* backspace */
            if (n) { n--; kputs("\b \b"); }
            continue;
        }
        if (c < 32 || c > 126) continue;     /* ignore what we cannot show */
        if (n + 1 >= len) continue;          /* full: stop accepting, keep the line */
        kputc((char)c);
        out[n++] = (char)c;
    }
}

static uint64_t do_syscall(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, void *frame) {
    switch (nr) {
    case SYS_READ:  return sys_read(a, b, c);
    case SYS_CLONE:  return do_fork(frame);
    case SYS_EXECVE: return do_execve((const char *)(uintptr_t)a, frame);
    case SYS_WRITE:  return sys_write(a, b, c);
    case SYS_GETPID: return (uint64_t)thread_current()->id;
    case SYS_WAIT4: {
        int st = 0;
        int id = thread_wait(&st);
        if (id < 0) return (uint64_t)-10;                    /* -ECHILD */
        /* Linux packs the status; a plain exit code sits in the second byte. */
        if (b) *(int *)(uintptr_t)b = (st & 0xff) << 8;
        return (uint64_t)id;
    }
    case SYS_EXIT:
        thread_exit((int)a);                 /* does not return */
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
    thread_exit(-1);
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
