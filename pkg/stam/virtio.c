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

static volatile uint8_t *base;       /* the device's registers */
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
        volatile uint8_t *b = (volatile uint8_t *)(uintptr_t)phys_to_kva(addr);
        if (*(volatile uint32_t *)(b + R_MAGIC) != 0x74726976) continue;
        uint32_t id = *(volatile uint32_t *)(b + R_DEVICE_ID);
        if (id) blk_seen++;                  /* a slot with something in it */
        if (id != VIRTIO_BLK) continue;
        return b;
    }
    return 0;
}

int virtio_blk_init(const fdt_t *f) {
    base = find_blk(f);
    if (!base) {
        /* Says which of the three it was: no transports in the tree at all, or
         * transports with nothing plugged into them, or devices that are not
         * block devices. "No disk" covers all three and distinguishes none. */
        kprintf("  disk      none: %u virtio-mmio slots, %u occupied, no block device\n",
                slots_seen, blk_seen);
        return -1;
    }

    uint32_t version = rd(R_VERSION);

    /* The handshake, in the order the specification requires: acknowledge,
     * claim to be a driver, agree the features, and only then say the device
     * may be used. Setting DRIVER_OK before the queue exists is a device that
     * may start writing into memory nothing has told it about. */
    wr(R_STATUS, 0);
    wr(R_STATUS, S_ACK);
    wr(R_STATUS, S_ACK | S_DRIVER);

    /* Nothing is negotiated except VIRTIO_F_VERSION_1, and only where it is
     * offered: every optional block feature here is one the driver would then
     * have to honour, and a driver that accepts a feature it ignores is worse
     * than one that refuses it. */
    wr(R_DEVICE_FEAT_SEL, 1);
    uint32_t hi = rd(R_DEVICE_FEAT);
    wr(R_DRIVER_FEAT_SEL, 1);
    wr(R_DRIVER_FEAT, hi & 1u);          /* bit 32 of the feature space */
    wr(R_DEVICE_FEAT_SEL, 0);
    wr(R_DRIVER_FEAT_SEL, 0);
    wr(R_DRIVER_FEAT, 0);
    if (version >= 2) {
        wr(R_STATUS, S_ACK | S_DRIVER | S_FEAT_OK);
        if (!(rd(R_STATUS) & S_FEAT_OK)) {
            kprintf("  disk      the device refused the features we offered\n");
            return -1;
        }
    }

    /* Capacity is the first field of the device configuration space, in
     * 512-byte sectors, and it is read as two halves because the register block
     * is 32 bits wide. */
    capacity = (uint64_t)*(volatile uint32_t *)(base + R_CONFIG)
             | ((uint64_t)*(volatile uint32_t *)(base + R_CONFIG + 4) << 32);

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

    wr(R_QUEUE_SEL, 0);
    if (rd(R_QUEUE_NUM_MAX) < QSIZE) {
        kprintf("  disk      the device's queue is smaller than the driver needs\n");
        return -1;
    }
    wr(R_QUEUE_NUM, QSIZE);
    if (version == 1) {
        wr(R_GUEST_PAGE_SZ, PAGE_SIZE);
        wr(R_QUEUE_ALIGN, QALIGN);
        wr(R_QUEUE_PFN, (uint32_t)(q_phys / PAGE_SIZE));
    } else {
        uint64_t avail = q_phys + O_AVAIL;
        uint64_t used  = q_phys + O_USED;
        wr(R_QUEUE_DESC_LO,  (uint32_t)q_phys);
        wr(R_QUEUE_DESC_HI,  (uint32_t)(q_phys >> 32));
        wr(R_QUEUE_AVAIL_LO, (uint32_t)avail);
        wr(R_QUEUE_AVAIL_HI, (uint32_t)(avail >> 32));
        wr(R_QUEUE_USED_LO,  (uint32_t)used);
        wr(R_QUEUE_USED_HI,  (uint32_t)(used >> 32));
        wr(R_QUEUE_READY, 1);
    }
    wr(R_STATUS, S_ACK | S_DRIVER | S_FEAT_OK | S_DRIVER_OK);

    present = 1;
    kprintf("  disk      virtio-blk v%u, %llu sectors (%llu MiB)\n",
            version, (unsigned long long)capacity,
            (unsigned long long)(capacity / 2048));
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
int virtio_blk_read(uint64_t sector, void *out) {
    if (!present) return -1;
    if (sector >= capacity) return -1;

    uint64_t fl = spin_lock_irq(&blk_lock);

    hdr->type = 0;                 /* read */
    hdr->reserved = 0;
    hdr->sector = sector;
    *status = 0xff;                /* so a device that writes nothing is seen */

    vq_desc[0].addr  = hdr_phys;
    vq_desc[0].len   = sizeof *hdr;
    vq_desc[0].flags = 1;          /* NEXT */
    vq_desc[0].next  = 1;
    vq_desc[1].addr  = data_phys;
    vq_desc[1].len   = 512;
    vq_desc[1].flags = 1 | 2;      /* NEXT | WRITE (the device writes it) */
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

    wr(R_QUEUE_NOTIFY, 0);

    /* Bounded, because a device that never answers must produce an error and
     * not a machine that stops. */
    int done = 0;
    for (uint64_t spin = 0; spin < 200000000ull; spin++) {
        if (vq_used->idx != last_used) { done = 1; break; }
        __asm__ volatile("" ::: "memory");
    }
    if (!done) {
        spin_unlock_irq(&blk_lock, fl);
        kprintf("  disk      the device did not answer a read of sector %llu\n",
                (unsigned long long)sector);
        return -1;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    last_used = vq_used->idx;
    wr(R_INT_ACK, rd(R_INT_STATUS));

    int ok = (*status == 0);
    if (ok) { uint8_t *o = out; for (int i = 0; i < 512; i++) o[i] = data[i]; }
    spin_unlock_irq(&blk_lock, fl);
    return ok ? 0 : -1;
}
