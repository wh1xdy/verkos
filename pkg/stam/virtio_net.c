/* stam — a network card.
 *
 * The same transport as the disk, two queues instead of one: queue 0 is
 * receive and queue 1 is transmit. The device fills the first with frames that
 * arrived and empties the second of frames to send, and the driver's whole job
 * is keeping buffers in the first and putting frames in the second.
 *
 * ---- the header nobody sees ----
 *
 * Every buffer on both queues carries a virtio_net_hdr in front of the
 * ethernet frame — offload flags, segmentation sizes, a checksum offset. None
 * of it is used here: nothing is negotiated that would make the device fill it
 * in, so it is written as zeros on transmit and skipped on receive.
 *
 * Its *size* is the trap. Ten bytes on a legacy device, twelve on a modern one,
 * because VIRTIO_F_VERSION_1 brings a num_buffers field that is present whether
 * or not buffer merging was negotiated. Get it wrong by two and every frame is
 * shifted by two: the destination MAC reads as the last two bytes of the header
 * followed by four of the real address, which matches nothing, and the card
 * looks like it is receiving nothing but rubbish. So it is computed from what
 * the two sides actually agreed on, not assumed.
 *
 * ---- polled ----
 *
 * The card's interrupt is not used. The disk earned its interrupt by first
 * working polled and then proving one arrived (D-086), and the same order is
 * right here: what is being shown is that the card sends and receives at all.
 * A driver that sleeps for something that never comes is a machine that stops.
 */
#include "virtio.h"
#include "net.h"
#include "mmu.h"
#include "console.h"
#include "lock.h"
#include <stdint.h>

#define VIRTIO_NET      1        /* device id */
#define F_VERSION_1     1u       /* bit 32 */
#define F_NET_MAC       (1u << 5)/* bit 5: the config space carries a MAC */

#define RX_BUFFERS      4        /* posted at all times; VIRTIO_QSIZE is 8 */
#define FRAME_MAX       1514     /* ethernet without the checksum */

static virtio_dev_t dev;
static virtqueue_t  rxq, txq;
static int          present;
static unsigned     hdr_size;
static uint8_t      mac[6];
static spinlock_t   net_lock = SPINLOCK_INIT;

/* One page per buffer. Two would fit, and one apiece means a driver bug that
 * runs off the end of a frame hits an unmapped page instead of the next
 * buffer — a fault that names the line, rather than a frame that is subtly
 * wrong somewhere else. */
static uint8_t *rx_buf[RX_BUFFERS];
static uint64_t rx_phys[RX_BUFFERS];
static uint8_t *tx_buf;
static uint64_t tx_phys;

static void rx_post(unsigned i) {
    rxq.desc[i].addr  = rx_phys[i];
    rxq.desc[i].len   = hdr_size + FRAME_MAX;
    rxq.desc[i].flags = VRING_WRITE;      /* the device writes it */
    rxq.desc[i].next  = 0;
    virtio_submit(&rxq, (uint16_t)i);
}

int virtio_net_init(const fdt_t *f) {
    if (virtio_find_pci(f, 0x02 /* network */, &dev) != 0 &&
        virtio_find_mmio(f, VIRTIO_NET, &dev) != 0)
        return -1;                        /* a machine with no card, silently */

    if (virtio_begin(&dev, F_NET_MAC, F_VERSION_1) != 0) {
        kprintf("  net       the card refused the features we offered\n");
        return -1;
    }
    hdr_size = (dev.feat_hi & F_VERSION_1) ? 12 : 10;

    /* The address, if the device said it has one. If it did not, the field
     * holds whatever the register happened to contain, and a card that invents
     * its own address is a card whose frames nothing answers. */
    if (!(dev.feat_lo & F_NET_MAC)) {
        kprintf("  net       the card offers no MAC address; not used\n");
        return -1;
    }
    for (int i = 0; i < 6; i++) mac[i] = virtio_cfg_read8(&dev, (unsigned)i);

    if (virtio_queue_init(&dev, &rxq, 0) != 0 ||
        virtio_queue_init(&dev, &txq, 1) != 0) {
        kprintf("  net       no memory for the queues, or they are too small\n");
        return -1;
    }
    for (unsigned i = 0; i < RX_BUFFERS; i++) {
        rx_buf[i] = alloc_page();
        if (!rx_buf[i]) { kprintf("  net       no memory for receive buffers\n"); return -1; }
        rx_phys[i] = kva_to_phys(rx_buf[i]);
    }
    tx_buf = alloc_page();
    if (!tx_buf) { kprintf("  net       no memory for the transmit buffer\n"); return -1; }
    tx_phys = kva_to_phys(tx_buf);

    virtio_ready(&dev);

    /* Buffers go on the receive queue only after DRIVER_OK: before it the
     * device may not touch them, and posting them earlier would be handing out
     * addresses to something not yet allowed to write to them. */
    for (unsigned i = 0; i < RX_BUFFERS; i++) rx_post(i);

    present = 1;
    kprintf("  net       virtio-net over %s, %02x:%02x:%02x:%02x:%02x:%02x, "
            "%u-byte header, polled\n",
            dev.name, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], hdr_size);
    return 0;
}

int virtio_net_present(void) { return present; }

void virtio_net_mac(uint8_t out[6]) {
    for (int i = 0; i < 6; i++) out[i] = mac[i];
}

int virtio_net_send(const void *frame, unsigned len) {
    if (!present || len > FRAME_MAX) return -1;
    spin_lock(&net_lock);

    for (unsigned i = 0; i < hdr_size; i++) tx_buf[i] = 0;
    const uint8_t *s = frame;
    for (unsigned i = 0; i < len; i++) tx_buf[hdr_size + i] = s[i];

    txq.desc[0].addr  = tx_phys;
    txq.desc[0].len   = hdr_size + len;
    txq.desc[0].flags = 0;                /* the device reads it */
    txq.desc[0].next  = 0;
    virtio_submit(&txq, 0);

    /* Bounded. A card that never takes the frame must produce an error and not
     * a machine that stops. */
    int done = 0;
    for (uint64_t spin = 0; spin < 50000000ull; spin++) {
        if (txq.used->idx != txq.last_used) { done = 1; break; }
        __asm__ volatile("" ::: "memory");
    }
    if (done) while (virtio_take(&txq, 0) >= 0) { }
    virtio_ack(&dev);
    spin_unlock(&net_lock);
    return done ? 0 : -1;
}

int virtio_net_recv(void *out, unsigned max) {
    if (!present) return -1;
    spin_lock(&net_lock);
    uint32_t len = 0;
    int head = virtio_take(&rxq, &len);
    if (head < 0) { spin_unlock(&net_lock); return -1; }
    /* A completion naming a descriptor we never posted. There is nothing to
     * hand back — re-posting it would put an index we do not own on the queue,
     * with an address out of an array we did not write. The compiler caught
     * exactly that written the other way round; the buffer is dropped and said
     * so, because a card returning indices out of range is worth one line. */
    if (head >= (int)RX_BUFFERS) {
        spin_unlock(&net_lock);
        kprintf("  net       the card completed descriptor %d, which was never posted\n", head);
        return -1;
    }
    /* len counts the header the device wrote as well as the frame. A device
     * that reported less than a header has told us something impossible, and
     * subtracting would wrap. */
    if (len < hdr_size) { rx_post((unsigned)head); spin_unlock(&net_lock); return -1; }
    unsigned n = len - hdr_size;
    if (n > max) n = max;
    const uint8_t *s = rx_buf[head] + hdr_size;
    uint8_t *d = out;
    for (unsigned i = 0; i < n; i++) d[i] = s[i];
    rx_post((unsigned)head);
    virtio_ack(&dev);
    spin_unlock(&net_lock);
    return (int)n;
}
