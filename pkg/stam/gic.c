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
#include "mmu.h"

/* Distributor */
#define GICD_CTLR         0x000
#define GICD_ISENABLER    0x100
#define GICD_IPRIORITYR   0x400
/* CPU interface */
#define GICC_CTLR         0x000
#define GICC_PMR          0x004
#define GICC_IAR          0x00c
#define GICC_EOIR         0x010

static volatile uint8_t *gicd, *gicc;   /* v2: distributor, CPU interface   */
static volatile uint8_t *gicr;          /* v3: this CPU's redistributor     */
static int gic_present;
static int gic_v3;

/* v3's CPU interface, reached with system registers rather than loads and
 * stores. Written as S3_ encodings because that spelling assembles on every
 * toolchain, named registers only on some. */
#define ICC_SRE_EL1      "S3_0_C12_C12_5"
#define ICC_PMR_EL1      "S3_0_C4_C6_0"
#define ICC_IGRPEN1_EL1  "S3_0_C12_C12_7"
#define ICC_IAR1_EL1     "S3_0_C12_C12_0"
#define ICC_EOIR1_EL1    "S3_0_C12_C12_1"
#define SYSREG_READ(r)   ({ uint64_t v_; __asm__ volatile("mrs %0, " r : "=r"(v_)); v_; })
#define SYSREG_WRITE(r,v) __asm__ volatile("msr " r ", %0\nisb\n" :: "r"((uint64_t)(v)))

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

/* v3 initialisation. Three parts must all happen, and leaving any one out
 * produces a machine that takes no interrupts and says nothing about why:
 * wake this core's redistributor, tell the CPU interface to use system
 * registers, and enable group 1 in both the distributor and the interface. */
static int gic_init_v3(const fdt_t *f, int node) {
    uint64_t dist = 0, redist = 0;
    if (reg_index(f, node, 0, &dist) || reg_index(f, node, 1, &redist)) return -1;
    gicd = (volatile uint8_t *)(uintptr_t)phys_to_kva(dist);
    gicr = (volatile uint8_t *)(uintptr_t)phys_to_kva(redist);

    /* Wake the redistributor: clear ProcessorSleep, then wait until it says
     * its children are awake. Skipping the wait is a race that usually works. */
    uint32_t waker = rd(gicr, 0x0014);
    wr(gicr, 0x0014, waker & ~(1u << 1));
    for (int spin = 0; spin < 1000000; spin++)
        if (!(rd(gicr, 0x0014) & (1u << 2))) break;

    /* ARE_NS and group 1, at the distributor. Without ARE the redistributor
     * layout is not the one we just used. */
    wr(gicd, GICD_CTLR, (1u << 4) | (1u << 1));

    /* The CPU interface only answers as a system register once SRE is set,
     * and on some implementations the bit is write-once — so read it back
     * rather than assume. */
    SYSREG_WRITE(ICC_SRE_EL1, SYSREG_READ(ICC_SRE_EL1) | 1);
    if (!(SYSREG_READ(ICC_SRE_EL1) & 1)) {
        kprintf("  gic       v3 refuses the system register interface\n");
        return -1;
    }
    SYSREG_WRITE(ICC_PMR_EL1, 0xf0);
    SYSREG_WRITE(ICC_IGRPEN1_EL1, 1);

    gic_present = 1; gic_v3 = 1;
    kprintf("  gic       v3 at 0x%llx, redistributor 0x%llx (from the device tree)\n",
            (unsigned long long)dist, (unsigned long long)redist);
    return 0;
}

int gic_init(const fdt_t *f) {
    int node = fdt_find_compatible(f, "arm,gic-v3");
    if (node != FDT_NONE) return gic_init_v3(f, node);

    node = fdt_find_compatible(f, "arm,gic-400");
    if (node == FDT_NONE) node = fdt_find_compatible(f, "arm,cortex-a15-gic");
    if (node == FDT_NONE) {
        kprintf("  gic       no interrupt controller in the device tree\n");
        return -1;
    }

    uint64_t dist = 0, cpu = 0;
    if (reg_index(f, node, 0, &dist) || reg_index(f, node, 1, &cpu)) return -1;
    /* The tree gives physical addresses; the kernel reaches devices through
     * its own window onto them, exactly as it does the UART. */
    gicd = (volatile uint8_t *)(uintptr_t)phys_to_kva(dist);
    gicc = (volatile uint8_t *)(uintptr_t)phys_to_kva(cpu);

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
    if (gic_v3 && intid < 32) {
        /* Per-CPU interrupts live in the redistributor's second 64 KiB frame,
         * not in the distributor. Configuring them in the distributor — where
         * v2 kept them — writes to registers that are reserved in v3, and the
         * timer then simply never fires. */
        volatile uint8_t *sgi = gicr + 0x10000;
        wr(sgi, 0x0080, rd(sgi, 0x0080) | (1u << intid));   /* group 1 */
        *(volatile uint8_t *)(sgi + 0x0400 + intid) = 0xa0;
        wr(sgi, 0x0100, 1u << intid);
        return;
    }
    if (gic_v3) wr(gicd, 0x0080 + (intid / 32) * 4,
                   rd(gicd, 0x0080 + (intid / 32) * 4) | (1u << (intid % 32)));
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
    uint32_t iar = gic_v3 ? (uint32_t)SYSREG_READ(ICC_IAR1_EL1) : rd(gicc, GICC_IAR);
    unsigned id = iar & 0xffffff;
    if (id >= 1020) return;                  /* spurious */
    if ((int)id == timer_intid) timer_tick();
    /* Tell the controller we are done *before* switching threads. Otherwise
     * the interrupt stays active while another thread runs, and that thread
     * can never be interrupted in turn — the machine would appear to schedule
     * exactly once and then stop. */
    if (gic_v3) SYSREG_WRITE(ICC_EOIR1_EL1, iar);
    else        wr(gicc, GICC_EOIR, iar);
    if ((int)id == timer_intid) schedule();
}
