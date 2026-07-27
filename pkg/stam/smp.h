#ifndef STAM_SMP_H
#define STAM_SMP_H
#include <stdint.h>
#include "fdt.h"

/* Which processor this is, as an index rather than as an MPIDR. The affinity
 * register is a hierarchy of fields with gaps in it, and every place that
 * wants to say "this CPU" wants a small number instead. */
unsigned cpu_index(void);
uint64_t cpu_mpidr(void);

#define MAX_CPUS 8

/* What is per-processor rather than per-machine.
 *
 * `current` was one variable for the whole kernel, which is the assumption that
 * kept the secondaries parked (D-076). It lives here now, and TPIDR_EL1 holds
 * the pointer — a register the hardware banks per core, so this_cpu() is one
 * instruction and does not have to search MPIDR against a table on a path that
 * runs on every scheduling decision. */
struct thread;
typedef struct cpu {
    struct thread *cur;      /* what this processor is running          */
    struct thread *idle;     /* and what it runs when there is nothing   */
    unsigned       index;
    unsigned       online;
    uint64_t       ticks;    /* its own, so an idle core is visibly idle */
} cpu_t;

static inline cpu_t *this_cpu(void) {
    uint64_t p;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(p));
    return (cpu_t *)(uintptr_t)p;
}
static inline void cpu_set_self(cpu_t *c) {
    __asm__ volatile("msr tpidr_el1, %0" :: "r"((uint64_t)(uintptr_t)c));
}
cpu_t *cpu_slot(unsigned i);
void   cpu_claim_self(unsigned idx);   /* every processor, on itself */

/* Start the other processors, using whatever the device tree says the
 * firmware's conduit is. Returns how many answered. */
int smp_start_others(const fdt_t *f);
unsigned smp_cpus_online(void);
#endif
