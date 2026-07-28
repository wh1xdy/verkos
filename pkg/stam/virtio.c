/* stam — reading a disk.
 *
 * Everything the kernel has run so far came out of an initrd: an archive handed
 * to it at boot and unpacked in memory. That is enough to prove a process, a
 * pipe and a shell, and it is not a system — a system reads what it was not
 * given.
 *
 * The device is virtio-blk over MMIO, which is what QEMU's `virt` machine
 * offers and what nearly every hypervisor speaks. Not because it is the easiest
 * thing that would work — a `-kernel`-loaded image could be handed a second
 * ramdisk — but because a driver that talks to a device over a queue in shared
 * memory is the shape every real driver has, and a fake one would prove nothing
 * about the interfaces underneath.
 *
 * ---- what virtio actually is ----
 *
 * A register block for control, and a ring in ordinary memory for work. The
 * guest writes descriptors into a table, publishes their indices in the
 * available ring, kicks the device, and reads completions out of the used ring.
 * A block request is three descriptors chained: a header saying read-or-write
 * and which sector, the buffer, and one byte for the answer.
 *
 * Three-descriptor and not one, because the device writes the middle and the
 * last and reads the first, and a descriptor carries one direction.
 *
 * ---- the two versions ----
 *
 * Version 1 (legacy) and version 2 differ in how the queue's address is given:
 * legacy publishes one page-frame number and derives the rest from a guest page
 * size it is told, while version 2 takes the three addresses separately. Both
 * are handled, because which one appears depends on the QEMU that happens to be
 * installed, and a kernel that supports the one on this machine is a kernel
 * that fails silently on the next.
 */
#include "virtio.h"
#include "fdt.h"
#include "mmu.h"
#include "console.h"
#include "lock.h"
#include "sched.h"
#include "pci.h"
#include "gic.h"
#include <stdint.h>

/* The MMIO register block. */
#define R_MAGIC          0x000    /* "virt" */
#define R_VERSION        0x004
#define R_DEVICE_ID      0x008
#define R_DEVICE_FEAT    0x010
#define R_DEVICE_FEAT_SEL 0x014
#define R_DRIVER_FEAT    0x020
#define R_DRIVER_FEAT_SEL 0x024
#define R_GUEST_PAGE_SZ  0x028    /* legacy only */
#define R_QUEUE_SEL      0x030
#define R_QUEUE_NUM_MAX  0x034
#define R_QUEUE_NUM      0x038
#define R_QUEUE_ALIGN    0x03c    /* legacy only */
#define R_QUEUE_PFN      0x040    /* legacy only */
#define R_QUEUE_READY    0x044    /* version 2   */
#define R_QUEUE_NOTIFY   0x050
#define R_INT_STATUS     0x060
#define R_INT_ACK        0x064
#define R_STATUS         0x070
#define R_QUEUE_DESC_LO  0x080    /* version 2   */
#define R_QUEUE_DESC_HI  0x084
#define R_QUEUE_AVAIL_LO 0x090
#define R_QUEUE_AVAIL_HI 0x094
#define R_QUEUE_USED_LO  0x0a0
#define R_QUEUE_USED_HI  0x0a4
#define R_CONFIG         0x100

#define S_ACK        1
#define S_DRIVER     2
#define S_DRIVER_OK  4
#define S_FEAT_OK    8

#define VIRTIO_BLK   2           /* device id */
#define F_VERSION_1  (1ull << 32)

#define QSIZE 8                  /* descriptors; one request uses three */

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;              /* 1 = chained, 2 = device writes it */
    uint16_t next;
};
struct vring_avail {
    uint16_t flags, idx;
    uint16_t ring[QSIZE];
    uint16_t used_event;
};
struct vring_used_elem { uint32_t id, len; };
struct vring_used {
    uint16_t flags, idx;
    struct vring_used_elem ring[QSIZE];
    uint16_t avail_event;
};

struct blk_req {
    uint32_t type;               /* 0 = read, 1 = write */
    uint32_t reserved;
    uint64_t sector;
};

/* The ring is three structures at fixed offsets inside one page: the descriptor
 * table, then the available ring, then the used ring at the next multiple of
 * QALIGN.
 *
 * The alignment is not decoration. A legacy device is given one page-frame
 * number and derives all three addresses from it and from the alignment the
 * driver writes to QueueAlign — so putting the used ring anywhere else means
 * the device polls one address while the driver reads another. The first
 * version of this put it at the end of the page, which is where it would go if
 * the alignment were the page size, and every read timed out with the device
 * having done nothing wrong.
 *
 * 64 and not 4096 because the whole ring then fits in one page, and QueueAlign
 * is the driver's to choose. */
#define QALIGN     64
#define O_DESC     0
#define O_AVAIL    (O_DESC + 16 * QSIZE)
#define O_USED     ((O_AVAIL + 6 + 2 * QSIZE + QALIGN - 1) & ~(QALIGN - 1))

/* The two ways the same device appears.
 *
 * MMIO is what a hypervisor's device tree lists; PCI is what firmware
 * describes, and what a machine with real slots in it has. The block logic
 * below does not know which it is talking to.
 *
 * A table of function pointers is the obvious way to write that, and it does
 * not work here. A static initialiser holding the address of a function is a
 * relocation, this kernel applies none, and the entries hold the addresses the
 * image had when it was linked — the same trap as D-037, D-039 and D-076, this
 * time wearing the shape of a design pattern rather than of one variable. It
 * presented as an instruction fetch at 0x40088e20 with the kernel running at
 * 0xffffff8040088e20.
 *
 * So the dispatch is a branch on a variable. Less elegant, and it is the same
 * two calls either way.
 */
static int   use_pci;
static const char *vt_name;

static volatile uint8_t *base;       /* the MMIO device's registers */
static uint8_t          *ring;       /* its one queue, in the high half  */
static struct vring_desc  *vq_desc;
static struct vring_avail *vq_avail;
static struct vring_used  *vq_used;
static uint64_t          q_phys;     /* and where the device sees it     */
static uint16_t          last_used;
static uint64_t          capacity;   /* in 512-byte sectors */
static int               present;
static spinlock_t        blk_lock = SPINLOCK_INIT;

/* Staging for the header, the answer and the data. The device reads and writes
 * memory directly, so all three have to be at addresses it can reach — which is
 * why they are here, in the kernel's own pages, rather than pointers the caller
 * supplied. A caller's buffer may be anywhere, including a user page that is
 * shared copy-on-write. */
static struct blk_req *hdr;
static uint8_t        *status;
static uint8_t        *data;         /* one sector */
static uint64_t        hdr_phys, status_phys, data_phys;

static uint32_t rd(unsigned off)            { return *(volatile uint32_t *)(base + off); }
static void     wr(unsigned off, uint32_t v){ *(volatile uint32_t *)(base + off) = v; }

/* Every virtio-mmio slot in the tree, in order, looking for a block device.
 * QEMU publishes thirty-two of them and fills them from the top, so most read
 * back a device id of zero and must be skipped rather than treated as a
 * failure. */
static unsigned slots_seen, blk_seen;
static int      blk_irq = -1;      /* the transport's interrupt, if the tree says */
/* Whether an interrupt from this device has ever arrived. Until one has, every
 * request is polled — a driver that waits for something that never comes is a
 * machine that stops, and "the tree named an interrupt" is not the same claim
 * as "the interrupt arrives". It is earned by one completion and then trusted
 * (D-086). */
static volatile int irq_works;
static volatile int blk_done;
static const char   blk_token;     /* an address to wait on, nothing more */

static volatile uint8_t *find_blk(const fdt_t *f) {
    int depth = 0;
    slots_seen = blk_seen = 0;
    /* From the root, not from -1: fdt_next_node steps past the node it is
     * given, and -1 is not a node. It walked nothing and reported nothing,
     * which reads exactly like a machine with no disk. */
    for (int off = fdt_root(f); off != FDT_NONE; off = fdt_next_node(f, off, &depth)) {
        if (!fdt_is_compatible(f, off, "virtio,mmio")) continue;
        slots_seen++;
        uint32_t len = 0;
        const void *reg = fdt_getprop(f, off, "reg", &len);
        if (!reg || len < 8) continue;
        uint64_t addr = fdt_cells(reg, f->addr_cells);
        /* <type number flags>: type 0 is an SPI, whose numbers start at 32. */
        uint32_t ilen = 0;
        const uint8_t *ints = fdt_getprop(f, off, "interrupts", &ilen);
        int this_irq = -1;
        if (ints && ilen >= 12 && fdt_cells(ints, 1) == 0)
            this_irq = (int)fdt_cells(ints + 4, 1) + 32;
        volatile uint8_t *b = (volatile uint8_t *)(uintptr_t)phys_to_kva(addr);
        if (*(volatile uint32_t *)(b + R_MAGIC) != 0x74726976) continue;
        uint32_t id = *(volatile uint32_t *)(b + R_DEVICE_ID);
        if (id) blk_seen++;                  /* a slot with something in it */
        if (id != VIRTIO_BLK) continue;
        blk_irq = this_irq;
        return b;
    }
    return 0;
}

static uint32_t mmio_version;

static void     mmio_status_set(uint8_t s) { wr(R_STATUS, s); }
static uint8_t  mmio_status_get(void)      { return (uint8_t)rd(R_STATUS); }
static uint32_t mmio_features_hi(void)     { wr(R_DEVICE_FEAT_SEL, 1); return rd(R_DEVICE_FEAT); }
static void     mmio_features_set(uint32_t hi) {
    wr(R_DRIVER_FEAT_SEL, 1); wr(R_DRIVER_FEAT, hi);
    wr(R_DRIVER_FEAT_SEL, 0); wr(R_DRIVER_FEAT, 0);
    wr(R_DEVICE_FEAT_SEL, 0);
}
static uint64_t mmio_capacity(void) {
    return (uint64_t)*(volatile uint32_t *)(base + R_CONFIG)
         | ((uint64_t)*(volatile uint32_t *)(base + R_CONFIG + 4) << 32);
}
static void mmio_notify(void)  { wr(R_QUEUE_NOTIFY, 0); }
static void mmio_irq_ack(void) { wr(R_INT_ACK, rd(R_INT_STATUS)); }
static int  mmio_queue_setup(uint64_t desc, uint64_t avail, uint64_t used) {
    wr(R_QUEUE_SEL, 0);
    if (rd(R_QUEUE_NUM_MAX) < QSIZE) return -1;
    wr(R_QUEUE_NUM, QSIZE);
    if (mmio_version == 1) {
        wr(R_GUEST_PAGE_SZ, PAGE_SIZE);
        wr(R_QUEUE_ALIGN, QALIGN);
        wr(R_QUEUE_PFN, (uint32_t)(desc / PAGE_SIZE));
    } else {
        wr(R_QUEUE_DESC_LO,  (uint32_t)desc);
        wr(R_QUEUE_DESC_HI,  (uint32_t)(desc >> 32));
        wr(R_QUEUE_AVAIL_LO, (uint32_t)avail);
        wr(R_QUEUE_AVAIL_HI, (uint32_t)(avail >> 32));
        wr(R_QUEUE_USED_LO,  (uint32_t)used);
        wr(R_QUEUE_USED_HI,  (uint32_t)(used >> 32));
        wr(R_QUEUE_READY, 1);
    }
    return 0;
}


/* ---- virtio over PCI ---------------------------------------------------
 *
 * The same device, described by capabilities in configuration space rather
 * than by a fixed register block. Each capability says which base address
 * register the structure is in and where inside it — so nothing here is at a
 * known address, and everything is where the device said. */
#define VIRTIO_PCI_CAP        0x09
#define CFG_COMMON  1
#define CFG_NOTIFY  2
#define CFG_ISR     3
#define CFG_DEVICE  4

static pci_dev_t         vpci;
static volatile uint8_t *pci_common, *pci_notify, *pci_isr, *pci_devcfg;
static uint32_t          notify_mult;

static uint16_t c_rd16(unsigned o) { return *(volatile uint16_t *)(pci_common + o); }
static void     c_wr16(unsigned o, uint16_t v) { *(volatile uint16_t *)(pci_common + o) = v; }
static uint32_t c_rd32(unsigned o) { return *(volatile uint32_t *)(pci_common + o); }
static void     c_wr32(unsigned o, uint32_t v) { *(volatile uint32_t *)(pci_common + o) = v; }

static void     pcit_status_set(uint8_t s) { *(volatile uint8_t *)(pci_common + 0x14) = s; }
static uint8_t  pcit_status_get(void)      { return *(volatile uint8_t *)(pci_common + 0x14); }
static uint32_t pcit_features_hi(void)     { c_wr32(0x00, 1); return c_rd32(0x04); }
static void     pcit_features_set(uint32_t hi) {
    c_wr32(0x08, 1); c_wr32(0x0c, hi);
    c_wr32(0x08, 0); c_wr32(0x0c, 0);
}
static uint64_t pcit_capacity(void) {
    return (uint64_t)*(volatile uint32_t *)(pci_devcfg)
         | ((uint64_t)*(volatile uint32_t *)(pci_devcfg + 4) << 32);
}
static void pcit_notify(void)  { *(volatile uint16_t *)pci_notify = 0; }
static void pcit_irq_ack(void) { (void)*(volatile uint8_t *)pci_isr; }   /* reading clears it */
static int  pcit_queue_setup(uint64_t desc, uint64_t avail, uint64_t used) {
    c_wr16(0x16, 0);                                   /* queue_select    */
    if (c_rd16(0x18) < QSIZE) return -1;               /* queue_size      */
    c_wr16(0x18, QSIZE);
    *(volatile uint64_t *)(pci_common + 0x20) = desc;
    *(volatile uint64_t *)(pci_common + 0x28) = avail;
    *(volatile uint64_t *)(pci_common + 0x30) = used;
    /* Where to kick this particular queue. The multiplier is the device's, and
     * a driver that assumes zero writes to the wrong address on any device that
     * uses a non-zero one — silently, because a write to a valid page that
     * means nothing does nothing. */
    uint16_t off = c_rd16(0x1e);
    pci_notify += (uint64_t)off * notify_mult;
    c_wr16(0x1c, 1);                                   /* queue_enable    */
    return 0;
}
/* The dispatch. Every one is the same shape on purpose: a reader checking that
 * the two transports are used consistently should not have to read eight
 * different arrangements of the same branch. */
static void     vt_status_set(uint8_t s)  { if (use_pci) pcit_status_set(s); else mmio_status_set(s); }
static uint8_t  vt_status_get(void)       { return use_pci ? pcit_status_get() : mmio_status_get(); }
static uint32_t vt_features_hi(void)      { return use_pci ? pcit_features_hi() : mmio_features_hi(); }
static void     vt_features_set(uint32_t h) { if (use_pci) pcit_features_set(h); else mmio_features_set(h); }
static uint64_t vt_capacity(void)         { return use_pci ? pcit_capacity() : mmio_capacity(); }
static void     vt_notify(void)           { if (use_pci) pcit_notify(); else mmio_notify(); }
static void     vt_irq_ack(void)          { if (use_pci) pcit_irq_ack(); else mmio_irq_ack(); }
static int      vt_queue_setup(uint64_t d, uint64_t a, uint64_t u) {
    return use_pci ? pcit_queue_setup(d, a, u) : mmio_queue_setup(d, a, u);
}

/* Place a capability's structure: assign the BAR it names and add the offset
 * inside it. */
static volatile uint8_t *cap_region(unsigned cap) {
    unsigned bar = pci_cfg_read8(&vpci, cap + 4);
    uint32_t off = pci_cfg_read32(&vpci, cap + 8);
    uint64_t at  = pci_bar_assign(&vpci, bar);
    if (!at) return 0;
    /* Not phys_to_kva. Firmware puts a 64-bit prefetchable region wherever it
     * likes, and on this machine that is 0x8000004000 — past the end of the
     * kernel's straight window, where the arithmetic wraps to a block that
     * exists and holds something else (D-081). */
    uint64_t va = 0;
    if (mmu_map_device(at + off, &va) != 0) return 0;
    return (volatile uint8_t *)(uintptr_t)va;
}

static int find_pci_blk(void) {
    if (!pci_present()) return -1;
    /* 0x1042 is a modern virtio block device and 0x1001 the transitional one.
     * Both are accepted, because a transitional device carries the modern
     * capability structures as well and QEMU produces one by default — a driver
     * that only took 0x1042 would work on the command line that says
     * disable-legacy=on and nowhere else.
     *
     * What decides is not the id but whether the four capabilities are there.
     * A device that has them is driven through them; one that does not is left
     * alone rather than half driven, and says so. */
    /* Silent when there is none: this is the first of two transports to try,
     * and a machine that has the other is not a machine with a problem. The
     * line that reports the outcome is the one at the end of virtio_blk_init,
     * which names whichever was used. */
    if (pci_find(0x1af4, 0x1000, 0x107f, 0x01 /* mass storage */, &vpci) != 0) return -1;

    unsigned cap = 0;
    while ((cap = pci_find_cap(&vpci, VIRTIO_PCI_CAP, cap)) != 0) {
        switch (pci_cfg_read8(&vpci, cap + 3)) {
        case CFG_COMMON: pci_common = cap_region(cap); break;
        case CFG_NOTIFY: pci_notify = cap_region(cap);
                         notify_mult = pci_cfg_read32(&vpci, cap + 16); break;
        case CFG_ISR:    pci_isr    = cap_region(cap); break;
        case CFG_DEVICE: pci_devcfg = cap_region(cap); break;
        default: break;
        }
    }
    if (!pci_common || !pci_notify || !pci_isr || !pci_devcfg) {
        kprintf("  disk      pci %04x:%04x has no modern capability structures "
                "(common %d notify %d isr %d config %d)\n",
                vpci.vendor, vpci.device, pci_common != 0, pci_notify != 0,
                pci_isr != 0, pci_devcfg != 0);
        return -1;
    }
    return 0;
}

int virtio_blk_init(const fdt_t *f) {
    /* PCI first. It is the transport that exists on a machine booted by
     * firmware, and preferring the one that only a hypervisor's device tree
     * lists would mean the tested path and the real path are different paths. */
    if (pci_init(f) == 0 && find_pci_blk() == 0) {
        use_pci = 1; vt_name = "pci";
    } else {
        base = find_blk(f);
        if (!base) {
            /* Says which of the three it was: no transports in the tree at all,
             * or transports with nothing plugged into them, or devices that are
             * not block devices. "No disk" covers all three and distinguishes
             * none. */
            kprintf("  disk      none: %u virtio-mmio slots, %u occupied, no block device\n",
                    slots_seen, blk_seen);
            return -1;
        }
        mmio_version = rd(R_VERSION);
        use_pci = 0; vt_name = mmio_version == 1 ? "mmio v1" : "mmio v2";
    }

    /* The handshake, in the order the specification requires: acknowledge,
     * claim to be a driver, agree the features, and only then say the device
     * may be used. Setting DRIVER_OK before the queue exists is a device that
     * may start writing into memory nothing has told it about. */
    vt_status_set(0);
    vt_status_set(S_ACK);
    vt_status_set(S_ACK | S_DRIVER);

    /* Nothing is negotiated except VIRTIO_F_VERSION_1, and only where it is
     * offered: every optional block feature here is one the driver would then
     * have to honour, and a driver that accepts a feature it ignores is worse
     * than one that refuses it. */
    uint32_t hi = vt_features_hi();
    vt_features_set(hi & 1u);              /* bit 32 of the feature space */
    if (use_pci || mmio_version >= 2) {
        vt_status_set(S_ACK | S_DRIVER | S_FEAT_OK);
        if (!(vt_status_get() & S_FEAT_OK)) {
            kprintf("  disk      the device refused the features we offered\n");
            return -1;
        }
    }

    capacity = vt_capacity();

    /* One page for the ring, one for the three staging buffers. */
    ring = alloc_page();
    uint8_t *stage = alloc_page();
    if (!ring || !stage) { kprintf("  disk      no memory for the queue\n"); return -1; }
    for (unsigned i = 0; i < PAGE_SIZE; i++) ring[i] = 0;
    vq_desc  = (struct vring_desc  *)(ring + O_DESC);
    vq_avail = (struct vring_avail *)(ring + O_AVAIL);
    vq_used  = (struct vring_used  *)(ring + O_USED);
    q_phys      = kva_to_phys(ring);
    hdr         = (struct blk_req *)stage;
    status      = stage + 64;
    data        = stage + 128;
    hdr_phys    = kva_to_phys(hdr);
    status_phys = kva_to_phys(status);
    data_phys   = kva_to_phys(data);

    if (vt_queue_setup(q_phys, q_phys + O_AVAIL, q_phys + O_USED) != 0) {
        kprintf("  disk      the device's queue is smaller than the driver needs\n");
        return -1;
    }
    vt_status_set(S_ACK | S_DRIVER | S_FEAT_OK | S_DRIVER_OK);

    present = 1;

    /* The interrupt, where the tree names one. Enabled here and *not* relied on
     * yet: blk_io polls until one has actually arrived. On the PCI transport
     * there is no number to enable — the routing is in the bridge's
     * interrupt-map, which vboot's synthetic tree does not carry — so that path
     * stays polled and says so rather than looking like it sleeps. */
    if (!use_pci && blk_irq >= 0) {
        gic_set_blk_irq(blk_irq);
        gic_enable_irq((unsigned)blk_irq);
    }

    kprintf("  disk      virtio-blk over %s, %llu sectors (%llu MiB), %s\n",
            vt_name, (unsigned long long)capacity,
            (unsigned long long)(capacity / 2048),
            (!use_pci && blk_irq >= 0) ? "interrupt-driven" : "polled");
    return 0;
}

int virtio_blk_present(void) { return present; }
uint64_t virtio_blk_sectors(void) { return capacity; }

/* One sector, synchronously.
 *
 * No interrupt: the completion is polled. That is a deliberate choice and not a
 * shortcut — an interrupt-driven version needs the requesting thread to block
 * and be woken, and the thing being proved here is that the device answers at
 * all. Polling makes the failure "it never answered" instead of "it answered
 * and the wakeup was lost", and those are different bugs.
 */
/* One sector, either direction. The only differences are which way the middle
 * descriptor points and what the header says, which is why this is one function
 * — two copies of the ring dance would be two places for one bug to be fixed in
 * one of. */
static int blk_io(uint64_t sector, void *buf, int write) {
    if (!present) return -1;
    if (sector >= capacity) return -1;

    /* The lock, but not the mask: the interrupt that ends this request has to be
     * able to arrive, and on the polled path the timer still has to tick. */
    spin_lock(&blk_lock);
    uint64_t fl = 0; (void)fl;

    if (write) { const uint8_t *s = buf; for (int i = 0; i < 512; i++) data[i] = s[i]; }
    hdr->type = write ? 1u : 0u;
    hdr->reserved = 0;
    hdr->sector = sector;
    blk_done = 0;
    *status = 0xff;                /* so a device that writes nothing is seen */

    vq_desc[0].addr  = hdr_phys;
    vq_desc[0].len   = sizeof *hdr;
    vq_desc[0].flags = 1;          /* NEXT */
    vq_desc[0].next  = 1;
    vq_desc[1].addr  = data_phys;
    vq_desc[1].len   = 512;
    /* WRITE here means "the device writes this", so it is set for a read and
        clear for a write — the opposite of what the name suggests, and the
        first thing to check when a transfer goes one way and not the other. */
    vq_desc[1].flags = write ? 1u : (1u | 2u);
    vq_desc[1].next  = 2;
    vq_desc[2].addr  = status_phys;
    vq_desc[2].len   = 1;
    vq_desc[2].flags = 2;          /* WRITE */
    vq_desc[2].next  = 0;

    vq_avail->ring[vq_avail->idx % QSIZE] = 0;
    /* The descriptors must be in memory before the index that publishes them,
     * or the device follows an index into a table it has not seen written. A
     * store barrier and not a full one: the ordering that matters is between
     * two writes. */
    __asm__ volatile("dsb st" ::: "memory");
    vq_avail->idx++;
    __asm__ volatile("dsb st" ::: "memory");

    vt_notify();

    /* Not slept on; D-086 through D-093. The wait below runs with interrupts
     * enabled, which is what all of it left behind. */

    /* Bounded, because a device that never answers must produce an error and
     * not a machine that stops. */
    int done = 0;
    for (uint64_t spin = 0; spin < 200000000ull; spin++) {
        if (vq_used->idx != last_used) { done = 1; break; }
        __asm__ volatile("" ::: "memory");
    }
    if (!done) {
        spin_unlock(&blk_lock);
        kprintf("  disk      the device did not answer a %s of sector %llu\n",
                write ? "write" : "read", (unsigned long long)sector);
        return -1;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    last_used = vq_used->idx;
    vt_irq_ack();

    int ok = (*status == 0);
    if (ok && !write) { uint8_t *o = buf; for (int i = 0; i < 512; i++) o[i] = data[i]; }
    spin_unlock(&blk_lock);
    return ok ? 0 : -1;
}

/* Acknowledge and wake. Nothing else: the thread that made the request reads
 * the ring and the status byte itself, and doing that here would be a handler
 * touching state whose owner is asleep rather than gone. */
void virtio_blk_interrupt(void) {
    if (!present) return;
    vt_irq_ack();
    /* Said once, and only from here. A line printed where the driver is set up
     * would say the same thing whether or not the device ever raised anything,
     * which is the report this project keeps finding. */
    if (!irq_works) {
        irq_works = 1;
        kprintf("  disk      the device's interrupt arrived; requests sleep now\n");
    }
    uint64_t s = sched_lock_irq();
    blk_done = 1;
    thread_wake_pipe(&blk_token);
    sched_unlock_irq(s);
}

int virtio_blk_read(uint64_t sector, void *out)        { return blk_io(sector, out, 0); }
int virtio_blk_write(uint64_t sector, const void *in)  { return blk_io(sector, (void *)in, 1); }
