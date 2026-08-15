#ifndef STAM_VIRTIO_H
#define STAM_VIRTIO_H
#include <stdint.h>
#include "fdt.h"
#include "pci.h"

/* ---- the transport ------------------------------------------------------
 *
 * virtio is two things: a way to reach a device's registers, and a ring in
 * ordinary memory that carries the work. The first comes in two shapes — a
 * fixed MMIO register block a hypervisor's device tree lists, and a set of
 * capabilities in PCI configuration space — and the second is identical in
 * both.
 *
 * Everything above this header talks to the ring. Nothing above it knows which
 * shape the registers had, which is what lets one block driver serve a machine
 * booted by QEMU's device tree and a machine booted by firmware.
 *
 * This was file-static state serving exactly one device until 2026-08-16, when
 * a second device (a network card) needed the same handshake and two queues
 * instead of one. Two things were wrong with the old shape and only the first
 * was visible: everything was a global, so a second device would have
 * overwritten the first; and queue setup added the notify offset *into* the
 * notify pointer, which is correct for one queue and wrong for the second,
 * third and fourth. That second one had never fired because nothing had ever
 * asked for a second queue.
 */

#define VIRTIO_QSIZE 8           /* descriptors per queue */

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
struct vring_avail {
    uint16_t flags, idx;
    uint16_t ring[VIRTIO_QSIZE];
    uint16_t used_event;
};
struct vring_used_elem { uint32_t id, len; };
struct vring_used {
    uint16_t flags, idx;
    struct vring_used_elem ring[VIRTIO_QSIZE];
    uint16_t avail_event;
};

#define VRING_NEXT   1u          /* this descriptor is chained to another   */
#define VRING_WRITE  2u          /* the *device* writes this one            */

typedef struct {
    int         use_pci;
    const char *name;            /* "mmio v1", "mmio v2" or "pci"           */
    int         irq;             /* the tree's number for it, or -1         */

    /* MMIO */
    volatile uint8_t *base;
    uint32_t          mmio_version;

    /* PCI */
    pci_dev_t         pci;
    volatile uint8_t *common, *notify, *isr, *devcfg;
    uint32_t          notify_mult;

    /* What the two sides agreed on, filled in by virtio_begin. A driver reads
     * these rather than assuming: a config field whose feature bit was not
     * negotiated holds nothing in particular, and reading it anyway is how a
     * driver invents a MAC address out of whatever the register happened to
     * contain. */
    uint32_t          feat_lo, feat_hi;
} virtio_dev_t;

typedef struct {
    virtio_dev_t      *dev;
    unsigned           index;
    uint8_t           *page;     /* the ring, one page, in the high half    */
    struct vring_desc  *desc;
    struct vring_avail *avail;
    struct vring_used  *used;
    uint64_t           phys;     /* and where the device sees it            */
    uint16_t           last_used;
    volatile uint8_t  *notify_addr;   /* PCI: this queue's own doorbell     */
} virtqueue_t;

/* Find a device of this type. Both return 0 on success and fill `d`.
 *
 * `pci_class` is the base class byte the card reports — 0x01 for storage,
 * 0x02 for network. A transitional virtio card's device id names the legacy
 * device it stands in for, so the class is what identifies it. */
int virtio_find_pci(const fdt_t *f, uint8_t pci_class, virtio_dev_t *d);
int virtio_find_mmio(const fdt_t *f, uint32_t device_id, virtio_dev_t *d);
/* How the last virtio_find_mmio went, for a driver that wants to say why it
 * found nothing: how many transports the tree listed and how many were
 * occupied by anything at all. */
void virtio_mmio_counts(unsigned *slots, unsigned *occupied);

/* The handshake, in the order the specification requires. The two masks are
 * every feature this driver is willing to honour: bits 0..31 in `want_lo`,
 * bits 32..63 in `want_hi`, where bit 0 of the high word is VIRTIO_F_VERSION_1.
 * What the device offers is intersected with them, so a driver names what it
 * implements and never accidentally accepts what it does not — accepting a
 * feature and ignoring it is worse than refusing it, because the device then
 * changes its behaviour and the driver does not.
 *
 * The offered bits are read here, between DRIVER and FEATURES_OK, because that
 * is where the specification puts the read and because a device is entitled to
 * report nothing before it has been acknowledged.
 *
 * Returns 0, or -1 if the device refused what was offered. */
int  virtio_begin(virtio_dev_t *d, uint32_t want_lo, uint32_t want_hi);
/* Set up queue `index`. Allocates its page. Returns 0. */
int  virtio_queue_init(virtio_dev_t *d, virtqueue_t *q, unsigned index);
/* DRIVER_OK: the device may now be used. Nothing before this may kick it. */
void virtio_ready(virtio_dev_t *d);
/* Publish descriptor `head` on this queue and tell the device. */
void virtio_submit(virtqueue_t *q, uint16_t head);
/* The next completion, or -1 if there is none. `len` may be null. */
int  virtio_take(virtqueue_t *q, uint32_t *len);
/* Acknowledge the device's interrupt. Says nothing about which queue. */
void virtio_ack(virtio_dev_t *d);
/* The device-specific configuration space. */
uint32_t virtio_cfg_read32(const virtio_dev_t *d, unsigned off);
uint8_t  virtio_cfg_read8(const virtio_dev_t *d, unsigned off);

/* ---- the block device ---------------------------------------------------
 *
 * Returns 0 when a virtio-blk device was found and set up; -1 otherwise, which
 * is a fact about the machine and not a failure — stam still boots from an
 * initrd. */
int      virtio_blk_init(const fdt_t *f);
int      virtio_blk_present(void);
uint64_t virtio_blk_sectors(void);
/* From the interrupt handler: acknowledge the device and wake whoever is
 * waiting for the request to finish. */
void     virtio_blk_interrupt(void);
int      virtio_blk_read(uint64_t sector, void *out);
int      virtio_blk_write(uint64_t sector, const void *in);
#endif
