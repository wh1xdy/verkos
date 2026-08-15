/* stam — reading a disk.
 *
 * Everything the kernel ran at first came out of an initrd: an archive handed
 * to it at boot and unpacked in memory. That is enough to prove a process, a
 * pipe and a shell, and it is not a system — a system reads what it was not
 * given.
 *
 * The device is virtio-blk, over MMIO or over PCI; virtio.c knows which and
 * nothing here does. Not the easiest thing that would work — a `-kernel`-loaded
 * image could be handed a second ramdisk — but a driver that talks to a device
 * over a queue in shared memory is the shape every real driver has, and a fake
 * one would prove nothing about the interfaces underneath.
 *
 * A block request is three descriptors chained: a header saying read-or-write
 * and which sector, the buffer, and one byte for the answer. Three and not one,
 * because the device writes the middle and the last and reads the first, and a
 * descriptor carries one direction.
 */
#include "virtio.h"
#include "fdt.h"
#include "mmu.h"
#include "console.h"
#include "lock.h"
#include "sched.h"
#include "gic.h"
#include <stdint.h>

#define VIRTIO_BLK   2           /* device id */
#define F_VERSION_1  1u          /* bit 32 of the feature space */

struct blk_req {
    uint32_t type;               /* 0 = read, 1 = write */
    uint32_t reserved;
    uint64_t sector;
};

static virtio_dev_t dev;
static virtqueue_t  vq;
static uint64_t     capacity;    /* in 512-byte sectors */
static int          present;
static spinlock_t   blk_lock = SPINLOCK_INIT;

/* Whether an interrupt from this device has ever arrived. Until one has, every
 * request is polled — a driver that waits for something that never comes is a
 * machine that stops, and "the tree named an interrupt" is not the same claim
 * as "the interrupt arrives". It is earned by one completion and then trusted
 * (D-086). */
static volatile int irq_works;
static volatile int blk_done;
static const char   blk_token;   /* an address to wait on, nothing more */

/* Staging for the header, the answer and the data. The device reads and writes
 * memory directly, so all three have to be at addresses it can reach — which is
 * why they are here, in the kernel's own pages, rather than pointers the caller
 * supplied. A caller's buffer may be anywhere, including a user page that is
 * shared copy-on-write. */
static struct blk_req *hdr;
static uint8_t        *status;
static uint8_t        *data;     /* one sector */
static uint64_t        hdr_phys, status_phys, data_phys;

int virtio_blk_init(const fdt_t *f) {
    /* PCI first. It is the transport that exists on a machine booted by
     * firmware, and preferring the one that only a hypervisor's device tree
     * lists would mean the tested path and the real path are different paths. */
    if (virtio_find_pci(f, 0x01 /* mass storage */, &dev) != 0 &&
        virtio_find_mmio(f, VIRTIO_BLK, &dev) != 0) {
        /* Says which of the three it was: no transports in the tree at all,
         * or transports with nothing plugged into them, or devices that are
         * not block devices. "No disk" covers all three and distinguishes
         * none. */
        unsigned slots = 0, occupied = 0;
        virtio_mmio_counts(&slots, &occupied);
        kprintf("  disk      none: %u virtio-mmio slots, %u occupied, no block device\n",
                slots, occupied);
        return -1;
    }

    /* Nothing is negotiated except VIRTIO_F_VERSION_1, and only where it is
     * offered: every optional block feature here is one the driver would then
     * have to honour, and a driver that accepts a feature it ignores is worse
     * than one that refuses it. */
    if (virtio_begin(&dev, 0, F_VERSION_1) != 0) {
        kprintf("  disk      the device refused the features we offered\n");
        return -1;
    }

    capacity = (uint64_t)virtio_cfg_read32(&dev, 0)
             | ((uint64_t)virtio_cfg_read32(&dev, 4) << 32);

    uint8_t *stage = alloc_page();
    if (!stage || virtio_queue_init(&dev, &vq, 0) != 0) {
        kprintf("  disk      no memory for the queue, or the device's queue is "
                "smaller than the driver needs\n");
        return -1;
    }
    hdr         = (struct blk_req *)stage;
    status      = stage + 64;
    data        = stage + 128;
    hdr_phys    = kva_to_phys(hdr);
    status_phys = kva_to_phys(status);
    data_phys   = kva_to_phys(data);

    virtio_ready(&dev);
    present = 1;

    /* The interrupt, where the tree names one. Enabled here and *not* relied on
     * yet: blk_io polls until one has actually arrived. On the PCI transport
     * there is no number to enable — the routing is in the bridge's
     * interrupt-map, which vboot's synthetic tree does not carry — so that path
     * stays polled and says so rather than looking like it sleeps. */
    if (dev.irq >= 0) {
        gic_set_blk_irq(dev.irq);
        gic_enable_irq((unsigned)dev.irq);
    }

    kprintf("  disk      virtio-blk over %s, %llu sectors (%llu MiB), %s\n",
            dev.name, (unsigned long long)capacity,
            (unsigned long long)(capacity / 2048),
            dev.irq >= 0 ? "interrupt-driven" : "polled");
    return 0;
}

int virtio_blk_present(void) { return present; }
uint64_t virtio_blk_sectors(void) { return capacity; }

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

    if (write) { const uint8_t *s = buf; for (int i = 0; i < 512; i++) data[i] = s[i]; }
    hdr->type = write ? 1u : 0u;
    hdr->reserved = 0;
    hdr->sector = sector;
    blk_done = 0;
    *status = 0xff;                /* so a device that writes nothing is seen */

    vq.desc[0].addr  = hdr_phys;
    vq.desc[0].len   = sizeof *hdr;
    vq.desc[0].flags = VRING_NEXT;
    vq.desc[0].next  = 1;
    vq.desc[1].addr  = data_phys;
    vq.desc[1].len   = 512;
    /* WRITE here means "the device writes this", so it is set for a read and
        clear for a write — the opposite of what the name suggests, and the
        first thing to check when a transfer goes one way and not the other. */
    vq.desc[1].flags = write ? VRING_NEXT : (VRING_NEXT | VRING_WRITE);
    vq.desc[1].next  = 2;
    vq.desc[2].addr  = status_phys;
    vq.desc[2].len   = 1;
    vq.desc[2].flags = VRING_WRITE;
    vq.desc[2].next  = 0;

    virtio_submit(&vq, 0);

    if (irq_works) {
        uint64_t s = sched_lock_irq();
        if (!blk_done) thread_block_on_pipe_until(&blk_token, 2);
        sched_unlock_irq(s);
    }

    /* Bounded, because a device that never answers must produce an error and
     * not a machine that stops. */
    int done = 0;
    for (uint64_t spin = 0; spin < 200000000ull; spin++) {
        if (vq.used->idx != vq.last_used) { done = 1; break; }
        __asm__ volatile("" ::: "memory");
    }
    if (!done) {
        spin_unlock(&blk_lock);
        kprintf("  disk      the device did not answer a %s of sector %llu\n",
                write ? "write" : "read", (unsigned long long)sector);
        return -1;
    }
    while (virtio_take(&vq, 0) >= 0) { }
    virtio_ack(&dev);

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
    virtio_ack(&dev);
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
