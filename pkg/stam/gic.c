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
#include "smp.h"
#include "virtio.h"
#include "net.h"
#include "console.h"
#include "sched.h"
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
static int timer_intid = -1, console_intid = -1, blk_intid = -1, net_intid = -1;
static volatile uint8_t *gicr;          /* v3: the boot CPU's redistributor */
static uint64_t gicr_base;              /* v3: the first frame, for the walk */
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
    gicr_base = phys_to_kva(redist);

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

/* This core's redistributor frame, in a v3.
 *
 * The frames are laid out one after another, 128 KiB each — RD_base and
 * SGI_base, 64 KiB apiece — and which belongs to which processor is in
 * GICR_TYPER's top 32 bits, as an affinity value. Not by index: the order is
 * the firmware's to choose, and a kernel that assumed frame N belonged to CPU N
 * would work on QEMU and be wrong on a machine that numbered them differently.
 * TYPER bit 4 marks the last frame, which is where the walk stops. */
static volatile uint8_t *gicr_for_me(void) {
    uint64_t me = cpu_mpidr();
    /* MPIDR to the affinity form TYPER uses: Aff3 moves down beside Aff2..0. */
    uint32_t want = (uint32_t)((me & 0xffffffull) | ((me >> 32) & 0xffull) << 24);
    for (volatile uint8_t *f = (volatile uint8_t *)(uintptr_t)gicr_base; ; f += 0x20000) {
        uint64_t typer = *(volatile uint64_t *)(uintptr_t)(f + 0x0008);
        if ((uint32_t)(typer >> 32) == want) return f;
        if (typer & (1ull << 4)) return 0;          /* that was the last one */
    }
}

/* The half of the controller that belongs to one processor rather than to the
 * machine. gic_init does the distributor, which is shared and set up once; this
 * is what every core has to do for itself, and a core that skips it takes no
 * interrupts and says nothing about why. */
void gic_cpu_init(void) {
    if (!gic_present) return;
    if (gic_v3) {
        volatile uint8_t *r = gicr_for_me();
        if (!r) { kprintf("  gic       no redistributor for this cpu\n"); return; }
        uint32_t waker = rd(r, 0x0014);
        wr(r, 0x0014, waker & ~(1u << 1));
        for (int spin = 0; spin < 1000000; spin++)
            if (!(rd(r, 0x0014) & (1u << 2))) break;
        SYSREG_WRITE(ICC_SRE_EL1, SYSREG_READ(ICC_SRE_EL1) | 1);
        SYSREG_WRITE(ICC_PMR_EL1, 0xf0);
        SYSREG_WRITE(ICC_IGRPEN1_EL1, 1);
        /* The timer's PPI, in this core's own SGI frame. Interrupts below 32
         * are per-processor and enabling one on the boot core enables nothing
         * anywhere else. */
        volatile uint8_t *sgi = r + 0x10000;
        unsigned t = timer_intid >= 0 ? (unsigned)timer_intid : 30;
        if (t < 32) {
            wr(sgi, 0x0080, rd(sgi, 0x0080) | (1u << t));      /* group 1 */
            *(volatile uint8_t *)(sgi + 0x0400 + t) = 0xa0;
            wr(sgi, 0x0100, 1u << t);
        }
        return;
    }
    wr(gicc, GICC_PMR, 0xf0);
    wr(gicc, GICC_CTLR, 1);
    /* In a v2 the first 32 interrupt numbers are banked: this write reaches
     * this core's copy of the enable register, not the one the primary set. */
    unsigned t = timer_intid ? (unsigned)timer_intid : 30;
    if (t < 32) {
        *(volatile uint8_t *)(gicd + GICD_IPRIORITYR + t) = 0xa0;
        wr(gicd, GICD_ISENABLER + (t / 32) * 4, 1u << (t % 32));
    }
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
extern void console_interrupt(void);
void gic_set_timer_irq(int id)   { timer_intid = id; }
void gic_set_console_irq(int id) { console_intid = id; }
void gic_set_blk_irq(int id)     { blk_intid = id; }
void gic_set_net_irq(int id)     { net_intid = id; }

/* The frame is passed in so the handler can tell whether the interrupt landed
 * on a program or on the kernel. It matters for signals: a process that never
 * calls into the kernel — a loop that counts, which is exactly what a person
 * wants to interrupt — has no other moment at which it can be told to die.
 * Delivering only on the way out of a system call means Ctrl-C works on
 * everything except the programs it is for. */
void irq_dispatch(void *frame) {
    if (!gic_present) return;
    uint32_t iar = gic_v3 ? (uint32_t)SYSREG_READ(ICC_IAR1_EL1) : rd(gicc, GICC_IAR);
    unsigned id = iar & 0xffffff;
    if (id >= 1020) return;                  /* spurious */
    if ((int)id == timer_intid) timer_tick();
    else if ((int)id == console_intid) {
        console_interrupt();
        /* The lock, because a reader on another processor may be between
         * "nothing to read" and "I am blocked" right now, and that is the whole
         * window this is about. */
        uint64_t f = sched_lock_irq();
        thread_wake_console();
        sched_unlock_irq(f);
    }
    else if ((int)id == blk_intid) virtio_blk_interrupt();
    else if ((int)id == net_intid) virtio_net_interrupt();
    /* Tell the controller we are done *before* switching threads. Otherwise
     * the interrupt stays active while another thread runs, and that thread
     * can never be interrupted in turn — the machine would appear to schedule
     * exactly once and then stop. */
    if (gic_v3) SYSREG_WRITE(ICC_EOIR1_EL1, iar);
    else        wr(gicc, GICC_EOIR, iar);
    if ((int)id == timer_intid) schedule();

    /* And on the way back out: if this interrupt landed on a program and a
     * signal is waiting for it, this is where it dies. Only when the
     * interrupted context was EL0 — a signal raised while the kernel is in the
     * middle of something has to wait until the kernel is finished with it,
     * and the syscall path already covers that case.
     *
     * SPSR's low four bits are the exception level and stack pointer that were
     * in use: zero means EL0t, which is a program. */
    {
        const uint64_t *f = frame;
        uint64_t spsr = f[32];              /* see the frame layout in vectors.S */
        if ((spsr & 0xf) == 0) {
            int sig = thread_signalled();
            /* A handler is the reason this point matters at all: a program in
             * a loop that makes no system call is exactly the one that must be
             * interruptible, and this is the only place the kernel gets to look
             * at it. */
            if (sig && !sig_deliver(sig, frame)) thread_exit(128 + sig);
        }
    }
}
