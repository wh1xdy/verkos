/* stam — the interrupt controller.
 *
 * Everything here is found in the device tree: which controller this machine
 * has, where its two register banks live, and which interrupt number the timer
 * is wired to. That last one is the part people hardcode — 30 is right on
 * almost every ARM machine, which is exactly what makes it a bad thing to
 * assume, because the failure on a machine where it is wrong is a kernel that
 * boots and then never ticks, with nothing to indicate why.
 *
 * GICv2 only for now. v3 is detected and reported rather than half-supported:
 * a controller that is configured wrongly delivers no interrupts, which looks
 * identical to having no timer at all.
 */
#include "gic.h"
#include "console.h"
#include "fdt.h"
#include "sched.h"

/* Distributor */
#define GICD_CTLR         0x000
#define GICD_ISENABLER    0x100
#define GICD_IPRIORITYR   0x400
/* CPU interface */
#define GICC_CTLR         0x000
#define GICC_PMR          0x004
#define GICC_IAR          0x00c
#define GICC_EOIR         0x010

static volatile uint8_t *gicd, *gicc;
static int gic_present;

static void wr(volatile uint8_t *base, unsigned off, uint32_t v) {
    *(volatile uint32_t *)(base + off) = v;
}
static uint32_t rd(volatile uint8_t *base, unsigned off) {
    return *(volatile uint32_t *)(base + off);
}

/* A node's "reg" may hold several ranges; fdt_reg only returns the first. */
static int reg_index(const fdt_t *f, int node, unsigned idx, uint64_t *addr) {
    uint32_t len = 0;
    const uint8_t *p = fdt_getprop(f, node, "reg", &len);
    if (!p) return -1;
    uint32_t stride = (f->addr_cells + f->size_cells) * 4;
    if ((idx + 1) * stride > len) return -1;
    *addr = fdt_cells(p + idx * stride, f->addr_cells);
    return 0;
}

int gic_init(const fdt_t *f) {
    int node = fdt_find_compatible(f, "arm,gic-400");
    if (node == FDT_NONE) node = fdt_find_compatible(f, "arm,cortex-a15-gic");
    if (node == FDT_NONE) {
        if (fdt_find_compatible(f, "arm,gic-v3") != FDT_NONE) {
            kprintf("  gic       v3 found — not supported yet\n");
            return -1;
        }
        kprintf("  gic       no interrupt controller in the device tree\n");
        return -1;
    }

    uint64_t dist = 0, cpu = 0;
    if (reg_index(f, node, 0, &dist) || reg_index(f, node, 1, &cpu)) return -1;
    gicd = (volatile uint8_t *)(uintptr_t)dist;
    gicc = (volatile uint8_t *)(uintptr_t)cpu;

    wr(gicd, GICD_CTLR, 1);          /* distributor on                     */
    wr(gicc, GICC_PMR, 0xf0);        /* accept anything of decent priority */
    wr(gicc, GICC_CTLR, 1);          /* this CPU's interface on            */
    gic_present = 1;
    kprintf("  gic       v2 at 0x%llx / 0x%llx (from the device tree)\n",
            (unsigned long long)dist, (unsigned long long)cpu);
    return 0;
}

void gic_enable_irq(unsigned intid) {
    if (!gic_present) return;
    /* Priority first, then enable: enabling an interrupt whose priority is
     * still zero means it can be masked out by PMR and never arrive. */
    *(volatile uint8_t *)(gicd + GICD_IPRIORITYR + intid) = 0xa0;
    wr(gicd, GICD_ISENABLER + (intid / 32) * 4, 1u << (intid % 32));
}

/* Which interrupt the timer is on, according to the tree. The "interrupts"
 * property holds four triples — secure physical, non-secure physical,
 * virtual, hypervisor — of <type number flags>. We want the non-secure
 * physical one, and a type of 1 means PPI, whose numbers start at 16. */
int gic_timer_irq(const fdt_t *f) {
    int node = fdt_find_compatible(f, "arm,armv8-timer");
    if (node == FDT_NONE) node = fdt_find_compatible(f, "arm,armv7-timer");
    if (node == FDT_NONE) return -1;
    uint32_t len = 0;
    const uint8_t *p = fdt_getprop(f, node, "interrupts", &len);
    if (!p || len < 24) return -1;
    uint32_t type = (uint32_t)fdt_cells(p + 12, 1);   /* second triple */
    uint32_t num  = (uint32_t)fdt_cells(p + 16, 1);
    return (int)(type == 1 ? num + 16 : num + 32);
}

/* Called from the vector table. Acknowledge, act, tell the controller we are
 * done — in that order, or the interrupt arrives again immediately. */
extern void timer_tick(void);
static int timer_intid = -1;
void gic_set_timer_irq(int id) { timer_intid = id; }

void irq_dispatch(void) {
    if (!gic_present) return;
    uint32_t iar = rd(gicc, GICC_IAR);
    unsigned id = iar & 0x3ff;
    if (id == 1023) return;                  /* spurious */
    if ((int)id == timer_intid) timer_tick();
    /* Tell the controller we are done *before* switching threads. Otherwise
     * the interrupt stays active while another thread runs, and that thread
     * can never be interrupted in turn — the machine would appear to schedule
     * exactly once and then stop. */
    wr(gicc, GICC_EOIR, iar);
    if ((int)id == timer_intid) schedule();
}
