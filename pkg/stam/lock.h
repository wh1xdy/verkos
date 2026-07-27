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

#endif
