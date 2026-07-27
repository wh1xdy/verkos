/* stam — keeping an interrupt out of a half-finished update.
 *
 * D-050 enabled interrupts for the duration of a system call, because a call
 * that waits for a device cannot be woken by that device otherwise, and wrote
 * down at the same line that this kernel has no locking and that it would be
 * the next thing to be wrong. It was.
 *
 * Two things share state with an interrupt handler today. The page allocator's
 * free list is popped in three statements — read the head, follow it, store the
 * next — and a timer tick between the first and the third reschedules, so a
 * second thread can take the *same page* and two address spaces end up owning
 * one piece of memory. And `thread_wake_console` runs from the interrupt and
 * writes thread states that `thread_wait` and `thread_exit` are reading.
 *
 * On one CPU the fix is not a lock, it is masking the interrupt: there is no
 * other processor to race with, only the handler that can interrupt us. So
 * that is what this is, and it is deliberately not dressed up as a spinlock.
 * A spinlock word here would be uncontended forever and would say, falsely,
 * that the SMP question had been answered.
 *
 * **It has not been.** The moment there is a second CPU this is not enough:
 * masking interrupts on one core says nothing about what the other is doing to
 * the same list. Every critical section marked here is a place that will need a
 * real lock, and marking them is most of that work done in advance.
 *
 * ---- 2026-07-28: there is a second CPU ----
 *
 * So the lock below is real, and the paragraph above is left as written because
 * it says what the sections were protecting and why. irq_save on its own is
 * still correct for state only this core and its own handler touch; anything
 * another core can reach needs the lock *and* the mask, in that order — a
 * handler that takes a lock this core already holds is a core waiting for
 * itself, which is not a race but a stop.
 */
#ifndef STAM_LOCK_H
#define STAM_LOCK_H
#include <stdint.h>

/* DAIF, then mask IRQ and FIQ. Returning the old value rather than
 * unconditionally unmasking on the way out is what makes these nest: an inner
 * section restores the state the outer one was in, not "enabled". */
static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("mrs %0, daif\n"
                     "msr daifset, #3\n"
                     : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");
}

/* One word, taken with a store-exclusive.
 *
 * ARMv8.0 — which cortex-a72 is — has no LSE atomics, so this is the
 * load-exclusive/store-exclusive loop that predates them, and it works on
 * everything. The wfe in the waiting path is not an optimisation for us: it
 * stops the spinning core from hammering the interconnect that the core
 * holding the lock needs in order to finish and release it.
 *
 * There is deliberately no owner field and no recursion count. A lock that can
 * be taken twice is a lock that stops reporting the mistake it exists to
 * report. */
typedef struct { volatile unsigned locked; } spinlock_t;

#define SPINLOCK_INIT  { 0 }

static inline void spin_lock(spinlock_t *l) {
    unsigned tmp, one = 1;
    __asm__ volatile(
        "   sevl\n"
        "1: wfe\n"
        "2: ldaxr   %w0, [%1]\n"
        "   cbnz    %w0, 1b\n"
        "   stxr    %w0, %w2, [%1]\n"
        "   cbnz    %w0, 2b\n"
        : "=&r"(tmp)
        : "r"(&l->locked), "r"(one)
        : "memory");
}

static inline void spin_unlock(spinlock_t *l) {
    /* stlr, not a plain store: everything written inside the section has to be
     * visible to the next core to take the lock before it sees the lock free.
     * Without the release the other core acquires and reads stale memory, which
     * looks like corruption at a line that touches nothing. */
    __asm__ volatile("stlr wzr, [%0]" :: "r"(&l->locked) : "memory");
}

/* The pair almost every caller wants: interrupts off first, then the lock.
 * Both, and in that order. Taking the lock with interrupts still on invites a
 * handler on this same core to ask for it, and that core then waits for
 * itself. */
static inline uint64_t spin_lock_irq(spinlock_t *l) {
    uint64_t f = irq_save();
    spin_lock(l);
    return f;
}

static inline void spin_unlock_irq(spinlock_t *l, uint64_t flags) {
    spin_unlock(l);
    irq_restore(flags);
}

#endif
