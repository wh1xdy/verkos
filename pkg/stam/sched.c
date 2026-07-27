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
#include "vm.h"
#include "console.h"
#include "lock.h"

extern void cpu_switch(uint64_t *save_sp_here, uint64_t resume_sp);
extern uint64_t thread_entry_point(void);

#define MAX_THREADS 16
#define STACK_PAGES 2

static thread_t threads[MAX_THREADS];
static int n_threads;            /* how many slots have ever been used        */
static int next_id;              /* process ids: monotonic, never reused      */
static thread_t *current;
static int started;

thread_t *thread_current(void) { return current; }
int thread_count(void) { return n_threads; }
thread_t *thread_at(int i) { return (i >= 0 && i < n_threads) ? &threads[i] : 0; }

/* A slot is not an id.
 *
 * These used to be the same number — `t->id = n_threads`, with the id doubling
 * as the array index — and a collected slot was never handed out again, despite
 * the comment in sched.h saying it may be. Sixteen slots for the whole life of
 * a boot meant a shell ran a dozen commands and then fork started refusing,
 * with nothing wrong except that the machine had run out of a resource it was
 * never returning.
 *
 * Reusing the slot while it still *is* the id would be worse than the leak.
 * `parent` holds an id, and an orphan asks "is my parent still here?" by
 * comparing it; hand the same number to a new process and an unrelated program
 * inherits somebody else's children. So the id became a counter of its own,
 * which costs one int and makes the two questions — which slot, which process —
 * separately answerable.
 *
 * A slot stays THREAD_FREE until its stack and saved registers are complete,
 * and only the last line of each creator makes it runnable. The old code got
 * this right by accident — it published the slot with `n_threads++` after
 * building the frame — and the accident has to be kept on purpose now that a
 * slot becomes visible the moment it is taken. schedule() runs from the timer
 * interrupt, so the window between "this slot is mine" and "this slot has a
 * stack" is a window in which a thread can be resumed on a stack pointer of
 * zero.
 */
static thread_t *alloc_slot(void) {
    for (int i = 0; i < n_threads; i++)
        if (threads[i].state == THREAD_FREE) return &threads[i];
    if (n_threads < MAX_THREADS) {
        thread_t *t = &threads[n_threads];
        t->state = THREAD_FREE;      /* not runnable until the caller says so */
        n_threads++;
        return t;
    }
    return 0;
}

/* Everything a slot carries from its previous occupant, cleared in one place so
 * a field added later cannot be forgotten in three. Deliberately leaves state
 * alone: the caller sets it, last. */
static void slot_init(thread_t *t, const char *name, uint64_t space) {
    t->id          = next_id++;
    t->name        = name;
    t->slices      = 0;
    t->space       = space;
    t->parent      = current ? current->id : -1;
    t->status      = 0;
    t->wait_reason = 0;
    t->wait_object = 0;
    t->stack_low   = 0;
    t->sp          = 0;
    for (int i = 0; i < NFD; i++) t->fd[i] = 0;
}

/* The frame cpu_switch will pop the first time this thread is resumed. The
 * registers it restores become the thread's starting state, so the entry point
 * goes in x19, its argument in x20, and the return address is the trampoline. */
thread_t *thread_create(const char *name, void (*fn)(void *), void *arg) {
    thread_t *t = alloc_slot();
    if (!t) return 0;
    unsigned char *stack = alloc_pages(STACK_PAGES);
    if (!stack) { t->state = THREAD_FREE; return 0; }

    slot_init(t, name, 0);
    t->stack_low = (uint64_t)(uintptr_t)stack;

    uint64_t top = (uint64_t)(uintptr_t)stack + STACK_PAGES * PAGE_SIZE;
    uint64_t *frame = (uint64_t *)(uintptr_t)(top - 96);
    for (int i = 0; i < 12; i++) frame[i] = 0;
    frame[0]  = (uint64_t)(uintptr_t)fn;    /* x19 */
    frame[1]  = (uint64_t)(uintptr_t)arg;   /* x20 */
    frame[11] = thread_entry_point();       /* x30: where cpu_switch's ret goes */
    t->sp = (uint64_t)(uintptr_t)frame;

    t->state = THREAD_RUNNABLE;             /* last: see alloc_slot */
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
    thread_t *t = alloc_slot();
    if (!t) return 0;
    unsigned char *stack = alloc_pages(STACK_PAGES);
    if (!stack) { t->state = THREAD_FREE; return 0; }

    slot_init(t, "child", space);
    /* The descriptors are duplicated and the objects behind them gain a
     * holder. That is what lets both halves of `a | b` close the end they do
     * not use without taking it from the other. */
    for (int i = 0; i < NFD; i++)
        t->fd[i] = current ? file_ref(current->fd[i]) : 0;
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

    t->state = THREAD_RUNNABLE;               /* last: see alloc_slot */
    return t;
}

/* Adopt the thread of execution we are already on — the one the boot code
 * started — so there is something to switch *away from*. Its stack is the one
 * the linker reserved, not one we allocated. */
void sched_init(const char *name) {
    thread_t *t = alloc_slot();
    slot_init(t, name, 0);
    t->parent = -1;              /* nobody started this one; the machine did */
    current = t;
    t->state = THREAD_RUNNABLE;
    started = 1;
}

/* Round robin over *slots*. This used to start from `after->id` and index the
 * array with it, which worked only while the id and the slot were the same
 * number. They are not any more, so the position comes from the pointer. */
static thread_t *next_runnable(thread_t *after) {
    int start = after ? (int)(after - threads) : 0;
    for (int i = 1; i <= n_threads; i++) {
        thread_t *t = &threads[(start + i) % n_threads];
        if (t->state == THREAD_RUNNABLE) return t;
    }
    return 0;
}

/* Choosing who runs next reads every slot's state, and the console interrupt
 * writes those states (thread_wake_console). A tick landing between "this one
 * is runnable" and "switch to it" could also arrive while `current` is halfway
 * updated, which is a switch to a thread the scheduler has already left.
 *
 * The mask is held across cpu_switch, which looks wrong and is not: each thread
 * saved DAIF on its own stack on the way in, and restores its own value when
 * something schedules it again. A thread running for the first time never
 * reaches that line — it starts in the trampoline, which unmasks interrupts
 * itself, and always has (D-037). */
void schedule(void) {
    if (!started || n_threads < 2) return;
    uint64_t f = irq_save();
    thread_t *prev = current;
    thread_t *next = next_runnable(prev);
    if (!next || next == prev) { irq_restore(f); return; }
    current = next;
    next->slices++;
    /* The low half belongs to whoever is about to run. Kernel-only threads
     * keep whatever was there; they never look at it. */
    if (next->space && next->space != prev->space) mmu_switch_user(next->space);
    cpu_switch(&prev->sp, next->sp);
    /* Execution resumes here when something schedules `prev` again — possibly
     * a long time later, and on a different thread's behalf. `f` is this
     * thread's own saved DAIF, read off this thread's own stack. */
    irq_restore(f);
}

void thread_yield(void) { schedule(); }

/* A process that has finished still holds its exit status and its stack — the
 * status because a parent may ask for it, the stack because we are standing on
 * it. Both are released by whoever collects it. Nobody frees themselves. */
void thread_exit(int status) {
    /* vm_free_space is outside the section on purpose: it returns hundreds of
     * pages, and holding interrupts off for that long is a timer tick missed
     * and a console character dropped. Nothing else can reach this address
     * space — the process is a zombie before the first page goes back. */
    uint64_t f = irq_save();
    current->state = THREAD_ZOMBIE;
    current->status = status;
    uint64_t space = current->space;
    current->space = 0;
    irq_restore(f);

    /* Letting go of the descriptors is what tells the other end of a pipe that
     * this process is finished — a reader waiting on a command that has exited
     * is released here, and nowhere else. */
    for (int i = 0; i < NFD; i++) {
        file_close(current->fd[i]);
        current->fd[i] = 0;
    }

    if (space) vm_free_space(space);

    /* Waking the parent is a search-then-write over slots the console
     * interrupt also writes. */
    f = irq_save();
    for (int i = 0; i < n_threads; i++)
        if (threads[i].id == current->parent && threads[i].state == THREAD_BLOCKED &&
            threads[i].wait_reason == WAIT_CHILD)
            threads[i].state = THREAD_RUNNABLE;
    irq_restore(f);

    for (;;) schedule();
}

/* Sleep until a character arrives. Unlike yielding in a loop, a thread parked
 * here is not scheduled at all — the CPU goes to whoever has work, and the
 * interrupt is what puts this one back in the queue.
 *
 * **Call this with interrupts already masked**, holding the mask from before
 * the check that found the buffer empty. Otherwise the wakeup can be lost: a
 * character arriving between "nothing to read" and "I am blocked" finds nobody
 * blocked, wakes nobody, and the thread then sleeps with the interrupt already
 * spent. A machine sitting at a prompt ignoring the key that was pressed looks
 * exactly like a dead console, and would be blamed on the driver.
 *
 * schedule() masks again on the way in and restores this thread's own value on
 * the way out, so the caller's mask is still held when this returns. */
void thread_block_on_console(void) {
    current->wait_reason = WAIT_CONSOLE;
    current->state = THREAD_BLOCKED;
    schedule();
}

/* Wait for a pipe to change. Both ends block here for opposite reasons — a
 * reader for bytes, a writer for room — so the wakeup is by object and wakes
 * everyone. Each re-tests its own condition in a loop, which is what makes a
 * shared wakeup safe: a thread woken for the wrong reason simply blocks again.
 *
 * Call with interrupts masked, holding the mask from before the condition was
 * tested. Same lost-wakeup shape as the console, and the same fix. */
void thread_block_on_pipe(const void *object) {
    current->wait_reason = WAIT_PIPE;
    current->wait_object = object;
    current->state = THREAD_BLOCKED;
    schedule();
}

void thread_wake_pipe(const void *object) {
    for (int i = 0; i < n_threads; i++)
        if (threads[i].state == THREAD_BLOCKED &&
            threads[i].wait_reason == WAIT_PIPE &&
            threads[i].wait_object == object)
            threads[i].state = THREAD_RUNNABLE;
}

/* Called from the interrupt, so it does the least it can: mark them runnable
 * and let the ordinary scheduler decide when. */
void thread_wake_console(void) {
    /* Already in interrupt context with interrupts masked, so this needs no
     * section of its own — but it is the reason every other one exists. */
    for (int i = 0; i < n_threads; i++)
        if (threads[i].state == THREAD_BLOCKED && threads[i].wait_reason == WAIT_CONSOLE)
            threads[i].state = THREAD_RUNNABLE;
}

/* Collect a finished child. Blocks if one is still running, returns -1 if
 * there was never a child to wait for — the distinction matters, because a
 * program that waits with no children should be told so rather than hang. */
int thread_wait(int *status_out) {
    for (;;) {
        int have_children = 0;
        /* The scan and the claim must be one step. Two parents looking at the
         * same zombie is not possible today — a process has one parent — but a
         * console interrupt landing mid-scan writes the states being read, and
         * the free_page calls below hand pages to an allocator this may be
         * interrupted inside of. */
        uint64_t f = irq_save();
        for (int i = 0; i < n_threads; i++) {
            if (threads[i].parent != current->id) continue;
            if (threads[i].state == THREAD_ZOMBIE) {
                int id = threads[i].id;
                if (status_out) *status_out = threads[i].status;
                if (threads[i].stack_low) {
                    for (unsigned p = 0; p < STACK_PAGES; p++)
                        free_page((void *)(uintptr_t)(threads[i].stack_low + p * PAGE_SIZE));
                    threads[i].stack_low = 0;
                }
                /* The slot goes back on the shelf, so clear what the next
                 * occupant must not inherit. `parent` especially: a stale one
                 * on a free slot is harmless, but a stale one that survived
                 * into a reused slot would make an unrelated process look like
                 * somebody's child. */
                threads[i].parent      = -1;
                threads[i].id          = -1;
                threads[i].space       = 0;
                threads[i].status      = 0;
                threads[i].wait_reason = 0;
                threads[i].name        = "free";
                threads[i].sp          = 0;
                threads[i].state       = THREAD_FREE;
                irq_restore(f);
                return id;
            }
            if (threads[i].state != THREAD_FREE) have_children = 1;
        }
        if (!have_children) { irq_restore(f); return -1; }
        /* Same shape as the console wait: marked blocked with the mask still
         * held, so a child exiting in the gap cannot wake a thread that has not
         * yet said it is asleep. */
        current->wait_reason = WAIT_CHILD;
        current->state = THREAD_BLOCKED;
        schedule();
        irq_restore(f);
    }
}

/* Anything not yet collected still counts: a zombie has finished running but
 * the process is not gone until someone has looked at why it stopped. */
int threads_alive(void) {
    int n = 0;
    for (int i = 0; i < n_threads; i++)
        if (threads[i].state == THREAD_RUNNABLE || threads[i].state == THREAD_BLOCKED) n++;
    return n;
}
