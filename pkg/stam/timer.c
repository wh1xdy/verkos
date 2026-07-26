/* stam — the tick.
 *
 * The ARM generic timer is a counter every core has, running at a frequency
 * the firmware reports in CNTFRQ_EL0. We arm it to fire after a slice, and on
 * every interrupt arm it again. That repeating interrupt is what will later
 * let the kernel take a CPU back from a program that has no intention of
 * giving it up — which is the difference between running programs and running
 * an operating system.
 */
#include "timer.h"
#include "gic.h"
#include "console.h"

static uint64_t interval;      /* counter units between ticks */
static volatile uint64_t ticks;
static unsigned hz;

static uint64_t cntfrq(void) {
    uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v;
}
static void arm(uint64_t n) {
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(n));
}

void timer_tick(void) { ticks++; arm(interval); }
uint64_t timer_ticks(void) { return ticks; }
unsigned timer_hz(void) { return hz; }

int timer_init(const fdt_t *f, unsigned want_hz) {
    int irq = gic_timer_irq(f);
    if (irq < 0) { kprintf("  timer     no timer in the device tree\n"); return -1; }

    uint64_t freq = cntfrq();
    if (!freq) { kprintf("  timer     CNTFRQ_EL0 is zero — firmware did not set it\n"); return -1; }
    hz = want_hz;
    interval = freq / want_hz;

    gic_set_timer_irq(irq);
    gic_enable_irq((unsigned)irq);
    arm(interval);
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"(1ul));   /* enable, unmasked */

    kprintf("  timer     %llu Hz counter, ticking at %llu Hz on irq %llu\n",
            (unsigned long long)freq, (unsigned long long)want_hz, (unsigned long long)irq);
    return 0;
}

void irq_enable(void)  { __asm__ volatile("msr daifclr, #2" ::: "memory"); }
void irq_disable(void) { __asm__ volatile("msr daifset, #2" ::: "memory"); }
