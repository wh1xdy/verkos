/* stam — the virtio transport, without a device attached to it.
 *
 * ---- what virtio actually is ----
 *
 * A register block for control, and a ring in ordinary memory for work. The
 * guest writes descriptors into a table, publishes their indices in the
 * available ring, kicks the device, and reads completions out of the used ring.
 *
 * A request is one descriptor or several chained: the device reads some of them
 * and writes others, and a descriptor carries one direction, so a block request
 * is three — a header saying read-or-write and which sector, the buffer, and
 * one byte for the answer.
 *
 * ---- the two versions ----
 *
 * MMIO version 1 (legacy) and version 2 differ in how the queue's address is
 * given: legacy publishes one page-frame number and derives the rest from a
 * guest page size it is told, while version 2 takes the three addresses
 * separately. Both are handled, because which one appears depends on the QEMU
 * that happens to be installed, and a kernel that supports the one on this
 * machine is a kernel that fails silently on the next.
 *
 * ---- why this file has no device in it ----
 *
 * It had one, until a network card wanted the same handshake. The block driver
 * is in virtio_blk.c and the card in virtio_net.c; neither contains a line
 * about how registers are reached.
 */
#include "virtio.h"
#include "fdt.h"
#include "mmu.h"
#include "console.h"
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
#define O_AVAIL    (O_DESC + 16 * VIRTIO_QSIZE)
#define O_USED     ((O_AVAIL + 6 + 2 * VIRTIO_QSIZE + QALIGN - 1) & ~(QALIGN - 1))

/* ---- MMIO --------------------------------------------------------------- */

static uint32_t rd(const virtio_dev_t *d, unsigned off) {
    return *(volatile uint32_t *)(d->base + off);
}
static void wr(const virtio_dev_t *d, unsigned off, uint32_t v) {
    *(volatile uint32_t *)(d->base + off) = v;
}

static unsigned mmio_slots, mmio_occupied;

void virtio_mmio_counts(unsigned *slots, unsigned *occupied) {
    if (slots) *slots = mmio_slots;
    if (occupied) *occupied = mmio_occupied;
}

/* Every virtio-mmio slot in the tree, in order, looking for one device type.
 * QEMU publishes thirty-two of them and fills them from the top, so most read
 * back a device id of zero and must be skipped rather than treated as a
 * failure. */
int virtio_find_mmio(const fdt_t *f, uint32_t device_id, virtio_dev_t *d) {
    int depth = 0;
    mmio_slots = mmio_occupied = 0;
    /* From the root, not from -1: fdt_next_node steps past the node it is
     * given, and -1 is not a node. It walked nothing and reported nothing,
     * which reads exactly like a machine with no disk. */
    for (int off = fdt_root(f); off != FDT_NONE; off = fdt_next_node(f, off, &depth)) {
        if (!fdt_is_compatible(f, off, "virtio,mmio")) continue;
        mmio_slots++;
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
        if (id) mmio_occupied++;             /* a slot with something in it */
        if (id != device_id) continue;
        d->use_pci      = 0;
        d->base         = b;
        d->irq          = this_irq;
        d->mmio_version = *(volatile uint32_t *)(b + R_VERSION);
        d->name         = d->mmio_version == 1 ? "mmio v1" : "mmio v2";
        return 0;
    }
    return -1;
}

/* ---- PCI ----------------------------------------------------------------
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

/* Place a capability's structure: assign the BAR it names and add the offset
 * inside it. */
static volatile uint8_t *cap_region(const virtio_dev_t *d, unsigned cap) {
    unsigned bar = pci_cfg_read8(&d->pci, cap + 4);
    uint32_t off = pci_cfg_read32(&d->pci, cap + 8);
    uint64_t at  = pci_bar_assign(&d->pci, bar);
    if (!at) return 0;
    /* Not phys_to_kva. Firmware puts a 64-bit prefetchable region wherever it
     * likes, and on this machine that is 0x8000004000 — past the end of the
     * kernel's straight window, where the arithmetic wraps to a block that
     * exists and holds something else (D-081). */
    uint64_t va = 0;
    if (mmu_map_device(at + off, &va) != 0) return 0;
    return (volatile uint8_t *)(uintptr_t)va;
}

int virtio_find_pci(const fdt_t *f, uint8_t pci_class, virtio_dev_t *d) {
    if (pci_init(f) != 0 || !pci_present()) return -1;
    /* 0x1042 is a modern virtio block device and 0x1001 the transitional one;
     * a card is 0x1041 or 0x1000. Both ranges are accepted, because a
     * transitional device carries the modern capability structures as well and
     * QEMU produces one by default — a driver that only took the modern id
     * would work on the command line that says disable-legacy=on and nowhere
     * else. What decides is not the id but whether the four capabilities are
     * there.
     *
     * Silent when there is none: this is the first of two transports to try,
     * and a machine that has the other is not a machine with a problem. */
    if (pci_find(0x1af4, 0x1000, 0x107f, pci_class, &d->pci) != 0) return -1;

    d->use_pci      = 1;
    d->name         = "pci";
    d->mmio_version = 0;
    d->irq          = -1;   /* the routing is in the bridge's interrupt-map,
                             * which vboot's synthetic tree does not carry */
    d->common = d->notify = d->isr = d->devcfg = 0;
    d->notify_mult = 0;

    unsigned cap = 0;
    while ((cap = pci_find_cap(&d->pci, VIRTIO_PCI_CAP, cap)) != 0) {
        switch (pci_cfg_read8(&d->pci, cap + 3)) {
        case CFG_COMMON: d->common = cap_region(d, cap); break;
        case CFG_NOTIFY: d->notify = cap_region(d, cap);
                         d->notify_mult = pci_cfg_read32(&d->pci, cap + 16); break;
        case CFG_ISR:    d->isr    = cap_region(d, cap); break;
        case CFG_DEVICE: d->devcfg = cap_region(d, cap); break;
        default: break;
        }
    }
    if (!d->common || !d->notify || !d->isr || !d->devcfg) {
        kprintf("  virtio    pci %04x:%04x has no modern capability structures "
                "(common %d notify %d isr %d config %d)\n",
                d->pci.vendor, d->pci.device, d->common != 0, d->notify != 0,
                d->isr != 0, d->devcfg != 0);
        return -1;
    }
    return 0;
}

static uint16_t c_rd16(const virtio_dev_t *d, unsigned o) {
    return *(volatile uint16_t *)(d->common + o);
}
static void c_wr16(const virtio_dev_t *d, unsigned o, uint16_t v) {
    *(volatile uint16_t *)(d->common + o) = v;
}
static uint32_t c_rd32(const virtio_dev_t *d, unsigned o) {
    return *(volatile uint32_t *)(d->common + o);
}
static void c_wr32(const virtio_dev_t *d, unsigned o, uint32_t v) {
    *(volatile uint32_t *)(d->common + o) = v;
}

/* ---- the dispatch -------------------------------------------------------
 *
 * A table of function pointers is the obvious way to write this, and it does
 * not work here. A static initialiser holding the address of a function is a
 * relocation, this kernel applies none, and the entries hold the addresses the
 * image had when it was linked — the same trap as D-037, D-039 and D-076, this
 * time wearing the shape of a design pattern rather than of one variable. It
 * presented as an instruction fetch at 0x40088e20 with the kernel running at
 * 0xffffff8040088e20.
 *
 * So the dispatch is a branch on a variable. Less elegant, and it is the same
 * two calls either way. Every one is the same shape on purpose: a reader
 * checking that the two transports are used consistently should not have to
 * read eight different arrangements of the same branch. */
static void status_set(virtio_dev_t *d, uint8_t s) {
    if (d->use_pci) *(volatile uint8_t *)(d->common + 0x14) = s;
    else            wr(d, R_STATUS, s);
}
static uint8_t status_get(const virtio_dev_t *d) {
    if (d->use_pci) return *(volatile uint8_t *)(d->common + 0x14);
    return (uint8_t)rd(d, R_STATUS);
}

/* `sel` is 0 for feature bits 0..31 and 1 for 32..63. Both halves matter: the
 * transport's own bits live in the high word and every device's own bits — the
 * one saying its configuration space carries a MAC address, for instance — live
 * in the low one. Only the high word was ever read here, and the low one was
 * written as zero, which is correct for a block device and would have quietly
 * cost the card its address. */
static uint32_t features_get(virtio_dev_t *d, unsigned sel) {
    if (d->use_pci) { c_wr32(d, 0x00, sel); return c_rd32(d, 0x04); }
    wr(d, R_DEVICE_FEAT_SEL, sel);
    return rd(d, R_DEVICE_FEAT);
}
static void features_set(virtio_dev_t *d, uint32_t lo, uint32_t hi) {
    if (d->use_pci) {
        c_wr32(d, 0x08, 1); c_wr32(d, 0x0c, hi);
        c_wr32(d, 0x08, 0); c_wr32(d, 0x0c, lo);
    } else {
        wr(d, R_DRIVER_FEAT_SEL, 1); wr(d, R_DRIVER_FEAT, hi);
        wr(d, R_DRIVER_FEAT_SEL, 0); wr(d, R_DRIVER_FEAT, lo);
        wr(d, R_DEVICE_FEAT_SEL, 0);
    }
}

uint32_t virtio_cfg_read32(const virtio_dev_t *d, unsigned off) {
    if (d->use_pci) return *(volatile uint32_t *)(d->devcfg + off);
    return *(volatile uint32_t *)(d->base + R_CONFIG + off);
}
uint8_t virtio_cfg_read8(const virtio_dev_t *d, unsigned off) {
    if (d->use_pci) return *(volatile uint8_t *)(d->devcfg + off);
    return *(volatile uint8_t *)(d->base + R_CONFIG + off);
}

void virtio_ack(virtio_dev_t *d) {
    if (d->use_pci) (void)*(volatile uint8_t *)d->isr;   /* reading clears it */
    else            wr(d, R_INT_ACK, rd(d, R_INT_STATUS));
}

/* The handshake, in the order the specification requires: acknowledge, claim
 * to be a driver, agree the features, and only then — in virtio_ready, after
 * the queues exist — say the device may be used. Setting DRIVER_OK before the
 * queue exists is a device that may start writing into memory nothing has told
 * it about. */
int virtio_begin(virtio_dev_t *d, uint32_t want_lo, uint32_t want_hi) {
    status_set(d, 0);
    status_set(d, S_ACK);
    status_set(d, S_ACK | S_DRIVER);
    d->feat_lo = features_get(d, 0) & want_lo;
    d->feat_hi = features_get(d, 1) & want_hi;
    features_set(d, d->feat_lo, d->feat_hi);
    if (d->use_pci || d->mmio_version >= 2) {
        status_set(d, S_ACK | S_DRIVER | S_FEAT_OK);
        if (!(status_get(d) & S_FEAT_OK)) return -1;
    }
    return 0;
}

void virtio_ready(virtio_dev_t *d) {
    status_set(d, S_ACK | S_DRIVER | S_FEAT_OK | S_DRIVER_OK);
}

int virtio_queue_init(virtio_dev_t *d, virtqueue_t *q, unsigned index) {
    uint8_t *page = alloc_page();
    if (!page) return -1;
    for (unsigned i = 0; i < PAGE_SIZE; i++) page[i] = 0;

    q->dev       = d;
    q->index     = index;
    q->page      = page;
    q->desc      = (struct vring_desc  *)(page + O_DESC);
    q->avail     = (struct vring_avail *)(page + O_AVAIL);
    q->used      = (struct vring_used  *)(page + O_USED);
    q->phys      = kva_to_phys(page);
    q->last_used = 0;
    q->notify_addr = 0;

    uint64_t desc = q->phys, avail = q->phys + O_AVAIL, used = q->phys + O_USED;

    if (d->use_pci) {
        c_wr16(d, 0x16, (uint16_t)index);                  /* queue_select */
        if (c_rd16(d, 0x18) < VIRTIO_QSIZE) return -1;     /* queue_size   */
        c_wr16(d, 0x18, VIRTIO_QSIZE);
        *(volatile uint64_t *)(d->common + 0x20) = desc;
        *(volatile uint64_t *)(d->common + 0x28) = avail;
        *(volatile uint64_t *)(d->common + 0x30) = used;
        /* Where to kick this particular queue. The multiplier is the device's,
         * and the offset is per queue — which is why it is stored here and not
         * added into d->notify. Adding it in place is right for one queue and
         * wrong for every queue after it, silently, because a write to a valid
         * page that means nothing does nothing. */
        uint16_t off = c_rd16(d, 0x1e);
        q->notify_addr = d->notify + (uint64_t)off * d->notify_mult;
        c_wr16(d, 0x1c, 1);                                /* queue_enable */
    } else {
        wr(d, R_QUEUE_SEL, index);
        if (rd(d, R_QUEUE_NUM_MAX) < VIRTIO_QSIZE) return -1;
        wr(d, R_QUEUE_NUM, VIRTIO_QSIZE);
        if (d->mmio_version == 1) {
            wr(d, R_GUEST_PAGE_SZ, PAGE_SIZE);
            wr(d, R_QUEUE_ALIGN, QALIGN);
            wr(d, R_QUEUE_PFN, (uint32_t)(desc / PAGE_SIZE));
        } else {
            wr(d, R_QUEUE_DESC_LO,  (uint32_t)desc);
            wr(d, R_QUEUE_DESC_HI,  (uint32_t)(desc >> 32));
            wr(d, R_QUEUE_AVAIL_LO, (uint32_t)avail);
            wr(d, R_QUEUE_AVAIL_HI, (uint32_t)(avail >> 32));
            wr(d, R_QUEUE_USED_LO,  (uint32_t)used);
            wr(d, R_QUEUE_USED_HI,  (uint32_t)(used >> 32));
            wr(d, R_QUEUE_READY, 1);
        }
    }
    return 0;
}

void virtio_submit(virtqueue_t *q, uint16_t head) {
    q->avail->ring[q->avail->idx % VIRTIO_QSIZE] = head;
    /* The descriptors must be in memory before the index that publishes them,
     * or the device follows an index into a table it has not seen written. A
     * store barrier and not a full one: the ordering that matters is between
     * two writes. */
    __asm__ volatile("dsb st" ::: "memory");
    q->avail->idx++;
    __asm__ volatile("dsb st" ::: "memory");

    virtio_dev_t *d = q->dev;
    if (d->use_pci) *(volatile uint16_t *)q->notify_addr = (uint16_t)q->index;
    else            wr(d, R_QUEUE_NOTIFY, q->index);
}

int virtio_take(virtqueue_t *q, uint32_t *len) {
    if (q->used->idx == q->last_used) return -1;
    __asm__ volatile("dsb sy" ::: "memory");
    struct vring_used_elem *e = &q->used->ring[q->last_used % VIRTIO_QSIZE];
    int head = (int)e->id;
    if (len) *len = e->len;
    q->last_used++;
    return head;
}
