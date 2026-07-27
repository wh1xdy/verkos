/* stam — the other processors.
 *
 * Everything until now has been true of one CPU, and the parts that will stop
 * being true are already marked: D-057 masks interrupts where a second core
 * would need a lock, and says in as many words that masking is sufficient on
 * one processor and not on two. This is the beginning of the two.
 *
 * A secondary starts the way the machine started: MMU off, caches off, at a
 * physical address, with nothing set up. Everything the primary configured has
 * to be configured again — which is why the values are published rather than
 * recomputed. Recomputing them means two code paths that must agree, and the
 * whole of D-042 is what happens when two paths that must agree do not.
 */
#include "smp.h"
#include "console.h"
#include "mmu.h"
#include "lock.h"
#include "gic.h"
#include "timer.h"
#include "sched.h"
extern void exceptions_install(void);   /* vectors.S */

uint64_t cpu_mpidr(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(v));
    return v & 0x00ffffffu;          /* Aff2:Aff1:Aff0, without the flags */
}

/* The affinity values the tree gave us, in the order the tree gave them, so
 * that "cpu 1" here means the same thing as cpu@1 there. */
static uint64_t cpu_aff[8];
static unsigned n_cpus;
static volatile unsigned online = 1;

unsigned cpu_index(void) {
    uint64_t me = cpu_mpidr();
    for (unsigned i = 0; i < n_cpus; i++) if (cpu_aff[i] == me) return i;
    return 0;
}

unsigned smp_cpus_online(void) { return online; }

static cpu_t cpus[MAX_CPUS];
cpu_t *cpu_slot(unsigned i) { return i < MAX_CPUS ? &cpus[i] : &cpus[0]; }

/* Claim a slot and point TPIDR_EL1 at it. Called once per processor, by that
 * processor, before anything reads `current`. The primary does it too: one
 * path, so the boot core cannot quietly be the special case that works. */
void cpu_claim_self(unsigned idx) {
    cpu_t *c = cpu_slot(idx);
    c->index  = idx;
    c->online = 1;
    cpu_set_self(c);
}

/* Every cpu@N under /cpus, in tree order, by the affinity in its "reg". The
 * order is what makes "cpu 1" mean the same thing here and there. */
static unsigned cpu_affinities(const fdt_t *f, uint64_t *out, unsigned max) {
    int cpus = fdt_find_path(f, "/cpus");
    if (cpus == FDT_NONE) return 0;
    unsigned n = 0;
    int depth = 0, off = cpus;
    int base_depth = -1;
    while ((off = fdt_next_node(f, off, &depth)) != FDT_NONE && n < max) {
        if (base_depth < 0) base_depth = depth;
        if (depth < base_depth) break;              /* left /cpus entirely */
        if (depth != base_depth) continue;          /* not a direct child  */
        uint32_t len = 0;
        const void *reg = fdt_getprop(f, off, "reg", &len);
        if (!reg || !len) continue;
        out[n++] = fdt_cells(reg, len / 4 > 2 ? 2 : len / 4);
    }
    return n;
}

/* What a secondary needs to become the same kind of CPU the primary already
 * is. Physical addresses throughout: it reads this with the MMU off. */
typedef struct {
    uint64_t sp;          /* stack top, physical                          */
    uint64_t mair, tcr, ttbr0, ttbr1, sctlr;
    uint64_t entry_high;  /* where to go once the MMU is on               */
} secondary_cfg_t;

static secondary_cfg_t cfg;

extern void secondary_entry(void);      /* smpentry.S, runs with the MMU off */
void secondary_main(void);              /* below, runs in the high half     */
/* Reached through assembly, never as &symbol — see smpentry.S. */
extern uint64_t secondary_entry_addr(void);
extern uint64_t secondary_main_addr(void);

/* PSCI, through whichever conduit the firmware said. Getting this wrong is a
 * call that returns an error nobody reads, or worse a trap to an exception
 * level that is not there. */
static int use_hvc = 1;

static int64_t psci_call(uint32_t fn, uint64_t a, uint64_t b, uint64_t c) {
    register uint64_t x0 __asm__("x0") = fn;
    register uint64_t x1 __asm__("x1") = a;
    register uint64_t x2 __asm__("x2") = b;
    register uint64_t x3 __asm__("x3") = c;
    if (use_hvc)
        __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    else
        __asm__ volatile("smc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    return (int64_t)x0;
}

#define PSCI_CPU_ON_64  0xC4000003u

void secondary_main(void) {
    /* Running high now; the low half has done its job. Drop it here rather than
     * leaving it, so a stale physical pointer faults on this core too. */
    mmu_set_user_table(mmu_user_table());

    /* And its own vectors, again, at the address they have now.
     *
     * smpentry.S installs them before turning the MMU on, which it must — an
     * exception between here and there has nowhere to go. But it computes the
     * address PC-relative while running in the low half, so VBAR_EL1 ends up
     * holding the low alias, and the line above has just stopped that alias
     * existing. The core then runs correctly until its first interrupt and
     * vanishes into an address that is not mapped, with no fault reported
     * because reporting one needs the table it cannot reach. QEMU's monitor
     * showed it as a PC of 0x40081200 while the boot core carried on. */
    exceptions_install();
    unsigned me = cpu_index();
    cpu_claim_self(me);

    /* Its own vectors are already installed (smpentry.S), but the interrupt
     * controller and the timer are per-processor and were configured on the
     * primary only: the distributor is shared, the CPU interface is not, and
     * the timer is a register in this core. Without both this processor takes
     * no interrupts at all, which looks exactly like a scheduler that will not
     * give it any work. */
    gic_cpu_init();
    timer_cpu_start();

    /* The first thing a new processor does is say so, because the alternative
     * — a CPU that came up and is quietly doing nothing — is indistinguishable
     * from one that never started. */
    kprintf("  cpu %u      online, mpidr 0x%llx\n", me, (unsigned long long)cpu_mpidr());
    uint64_t f = irq_save();
    online++;
    irq_restore(f);

    sched_cpu_init("idle");     /* adopt this stack as this core's idler */
    sched_idle_loop();          /* take work, or wait for some           */
}

int smp_start_others(const fdt_t *f) {
    /* The conduit, from /psci. No node means no way to start anything, which
     * is a fact about the machine rather than a failure. */
    int psci = fdt_find_path(f, "/psci");
    if (psci == FDT_NONE) { kprintf("  cpus      no psci node; the other processors stay off\n"); return 0; }
    uint32_t mlen = 0;
    const char *m = fdt_getprop(f, psci, "method", &mlen);
    use_hvc = !(m && mlen && m[0] == 's');     /* "smc" or "hvc" */

    n_cpus = cpu_affinities(f, cpu_aff, 8);
    if (n_cpus < 2) { kprintf("  cpus      one processor\n"); return 0; }

    /* Everything the secondary cannot work out for itself. */
    __asm__ volatile("mrs %0, mair_el1"  : "=r"(cfg.mair));
    __asm__ volatile("mrs %0, tcr_el1"   : "=r"(cfg.tcr));
    /* Not the primary's ttbr0 — that is the empty user table, and the secondary
     * is still executing in the low half when it turns the MMU on. */
    cfg.ttbr0 = mmu_identity_table();
    __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(cfg.ttbr1));
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(cfg.sctlr));
    cfg.entry_high = secondary_main_addr();

    int started = 0;
    for (unsigned i = 1; i < n_cpus; i++) {
        unsigned char *stack = alloc_pages(2);
        if (!stack) { kprintf("  cpus      no stack for cpu %u\n", i); break; }
        cfg.sp = kva_to_phys(stack) + 2 * PAGE_SIZE;
        /* The configuration has to be in memory, not in this CPU's cache: the
         * one that reads it has its own caches off and walks straight to RAM.
         * This is D-034's lesson in a different place. */
        mmu_clean(&cfg, sizeof cfg);

        int64_t rc = psci_call(PSCI_CPU_ON_64, cpu_aff[i],
                               kva_to_phys((void *)(uintptr_t)secondary_entry_addr()),
                               kva_to_phys(&cfg));
        if (rc != 0) {
            kprintf("  cpus      psci refused cpu %u (%lld)\n", i, (long long)rc);
            continue;
        }
        /* Wait for it to say so itself rather than assuming the call worked.
         * PSCI returning success means the request was accepted, not that a
         * processor is running.
         *
         * Against a snapshot, not against started: if one processor fails to
         * arrive and a later one does, counting from started would credit the
         * arrival to whichever CPU the loop had reached — a report that does
         * not depend on what it reports.
         *
         * The wait is also what makes one shared cfg safe. The next iteration
         * overwrites cfg.sp, and this core has already read it and set SP by
         * the time it increments online. */
        unsigned before = online;
        for (volatile unsigned long spin = 0; spin < 200000000UL; spin++)
            if (online > before) break;
        if (online > before) started++;
        else kprintf("  cpus      cpu %u accepted the start and never arrived\n", i);
    }
    return started;
}
