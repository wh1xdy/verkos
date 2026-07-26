/* stam — threads, and a scheduler that takes the CPU back.
 *
 * Round robin, one queue, no priorities. That is not a placeholder for
 * something cleverer so much as the honest shape of the problem right now:
 * every thread here is kernel code we wrote, and there is nothing yet to be
 * unfair to. Priorities become meaningful when threads belong to different
 * people.
 *
 * What matters is that the switch happens from the timer interrupt, without
 * the running thread agreeing to it. A thread that loops forever doing
 * arithmetic still loses the CPU. That is the whole difference between a
 * program that runs other programs and an operating system.
 */
#include "sched.h"
#include "mmu.h"
#include "console.h"
#include "mmu.h"

extern void cpu_switch(uint64_t *save_sp_here, uint64_t resume_sp);
extern uint64_t thread_entry_point(void);

#define MAX_THREADS 16
#define STACK_PAGES 2

static thread_t threads[MAX_THREADS];
static int n_threads;
static thread_t *current;
static int started;

thread_t *thread_current(void) { return current; }
int thread_count(void) { return n_threads; }
thread_t *thread_at(int i) { return (i >= 0 && i < n_threads) ? &threads[i] : 0; }

/* The frame cpu_switch will pop the first time this thread is resumed. The
 * registers it restores become the thread's starting state, so the entry point
 * goes in x19, its argument in x20, and the return address is the trampoline. */
thread_t *thread_create(const char *name, void (*fn)(void *), void *arg) {
    if (n_threads >= MAX_THREADS) return 0;
    unsigned char *stack = alloc_pages(STACK_PAGES);
    if (!stack) return 0;

    thread_t *t = &threads[n_threads];
    t->id = n_threads;
    t->name = name;
    t->state = THREAD_RUNNABLE;
    t->slices = 0;
    t->space = 0;
    t->stack_low = (uint64_t)(uintptr_t)stack;

    uint64_t top = (uint64_t)(uintptr_t)stack + STACK_PAGES * PAGE_SIZE;
    uint64_t *frame = (uint64_t *)(uintptr_t)(top - 96);
    for (int i = 0; i < 12; i++) frame[i] = 0;
    frame[0]  = (uint64_t)(uintptr_t)fn;    /* x19 */
    frame[1]  = (uint64_t)(uintptr_t)arg;   /* x20 */
    frame[11] = thread_entry_point();       /* x30: where cpu_switch's ret goes */
    t->sp = (uint64_t)(uintptr_t)frame;

    n_threads++;
    return t;
}

extern uint64_t ret_to_user_addr(void);

/* Build a thread that is a copy of one already in the middle of a system call.
 *
 * The child never runs thread_create's trampoline: its kernel stack is made to
 * look exactly like the parent's does right now, so the first thing it does
 * when scheduled is finish the system call the parent is still inside. That is
 * what makes fork return twice — once in each process, from the same
 * instruction, with different values.
 */
thread_t *thread_fork(const void *parent_frame, unsigned frame_bytes, uint64_t space) {
    if (n_threads >= MAX_THREADS) return 0;
    unsigned char *stack = alloc_pages(STACK_PAGES);
    if (!stack) return 0;

    thread_t *t = &threads[n_threads];
    t->id = n_threads;
    t->name = "child";
    t->state = THREAD_RUNNABLE;
    t->slices = 0;
    t->space = space;
    t->stack_low = (uint64_t)(uintptr_t)stack;

    uint64_t top = (uint64_t)(uintptr_t)stack + STACK_PAGES * PAGE_SIZE;
    unsigned char *frame = (unsigned char *)(uintptr_t)(top - frame_bytes);
    const unsigned char *src = parent_frame;
    for (unsigned i = 0; i < frame_bytes; i++) frame[i] = src[i];
    ((uint64_t *)frame)[0] = 0;               /* x0: what fork returns to the child */

    uint64_t *sw = (uint64_t *)(uintptr_t)((uint64_t)(uintptr_t)frame - 96);
    for (int i = 0; i < 12; i++) sw[i] = 0;
    sw[11] = ret_to_user_addr();              /* x30: where cpu_switch returns */
    t->sp = (uint64_t)(uintptr_t)sw;

    n_threads++;
    return t;
}

/* Adopt the thread of execution we are already on — the one the boot code
 * started — so there is something to switch *away from*. Its stack is the one
 * the linker reserved, not one we allocated. */
void sched_init(const char *name) {
    thread_t *t = &threads[n_threads];
    t->id = n_threads;
    t->name = name;
    t->state = THREAD_RUNNABLE;
    t->slices = 0;
    t->space = 0;
    t->sp = 0;
    t->stack_low = 0;
    current = t;
    n_threads++;
    started = 1;
}

static thread_t *next_runnable(thread_t *after) {
    int start = after ? after->id : 0;
    for (int i = 1; i <= n_threads; i++) {
        thread_t *t = &threads[(start + i) % n_threads];
        if (t->state == THREAD_RUNNABLE) return t;
    }
    return 0;
}

void schedule(void) {
    if (!started || n_threads < 2) return;
    thread_t *prev = current;
    thread_t *next = next_runnable(prev);
    if (!next || next == prev) return;
    current = next;
    next->slices++;
    /* The low half belongs to whoever is about to run. Kernel-only threads
     * keep whatever was there; they never look at it. */
    if (next->space && next->space != prev->space) mmu_switch_user(next->space);
    cpu_switch(&prev->sp, next->sp);
    /* Execution resumes here when something schedules `prev` again — possibly
     * a long time later, and on a different thread's behalf. */
}

void thread_yield(void) { schedule(); }

void thread_exit(void) {
    current->state = THREAD_FINISHED;
    for (;;) schedule();     /* never picked again; schedule() moves on */
}

int threads_alive(void) {
    int n = 0;
    for (int i = 0; i < n_threads; i++)
        if (threads[i].state == THREAD_RUNNABLE) n++;
    return n;
}
