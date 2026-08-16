/* stam — ethernet, ARP, IPv4 and an echo.
 *
 * The smallest thing that proves a network card works in both directions and
 * is not a demonstration of itself. A driver can always claim it sent a frame:
 * writing to a queue succeeds whether or not anything is listening. An echo
 * reply cannot be produced by this kernel — it is built by whatever is at the
 * other end, addressed back to a MAC address it learned from us, carrying the
 * payload we chose, with a checksum computed over it. If those bytes come
 * back, the frame left the machine and something outside answered.
 *
 * ---- alignment ----
 *
 * Every multi-byte field here is read and written a byte at a time. Not
 * pedantry: stam is built with -mstrict-align because it runs with the MMU off
 * until it has read the device tree, and a packet is a stream of bytes at
 * offsets nobody chose for the compiler's benefit. An IPv4 source address sits
 * fourteen bytes into the frame plus twelve into the header — offset 26, which
 * is even and not a multiple of four. A `uint32_t *` there is an alignment
 * fault on some paths and a silently wrong value nowhere; the byte accessors
 * cost nothing and cannot be wrong.
 *
 * ---- what is not here ----
 *
 * No routing: everything not on the local network would go to a gateway, and
 * there is no table to look it up in. No fragmentation, no options, no TCP, no
 * sockets. A program cannot reach any of this. It is the kernel proving it has
 * a card, in the way the disk was first proved by reading one sector.
 */
#include "net.h"
#include "virtio.h"
#include "console.h"
#include "timer.h"
#include <stdint.h>

#define ET_ARP   0x0806
#define ET_IPV4  0x0800
#define IP_ICMP  1

/* QEMU's user-mode network, which is what this is tested against: the guest is
 * 10.0.2.15, the gateway and the host's stand-in is 10.0.2.2. Hard-coded
 * because there is no DHCP client yet, and said out loud in the boot line so
 * that a reader is not left thinking the kernel discovered it. */
static uint32_t my_ip  = IPV4(10,0,2,15);
static uint32_t gw_ip  = IPV4(10,0,2,2);

static uint8_t  my_mac[6];
static int      up;

/* Byte order on the wire is big-endian, and every field below is written by
 * these two so that no line has to think about it. */
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* The internet checksum: the one's complement of the one's complement sum of
 * every 16-bit word. An odd trailing byte is padded with zero on the right,
 * which is not the same as adding it as-is and is where a hand-written one
 * usually goes wrong. */
static uint16_t checksum(const uint8_t *p, unsigned n) {
    uint32_t sum = 0;
    unsigned i = 0;
    for (; i + 1 < n; i += 2) sum += get16(p + i);
    if (i < n) sum += (uint32_t)p[i] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Microseconds off the architected counter, not timer ticks. A round trip to a
 * hypervisor's built-in gateway is far shorter than one tick, so measuring in
 * ticks printed "0 ticks" — a number that is the same whether the reply took no
 * time or the clock is not running, which is the shape this project keeps
 * finding. The counter runs at a frequency the hardware publishes. */
static uint64_t micros(void) {
    uint64_t c, hz;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(c));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(hz));
    if (!hz) return 0;
    return c / (hz / 1000000ull ? hz / 1000000ull : 1);
}

static void eth_header(uint8_t *f, const uint8_t dst[6], uint16_t type) {
    for (int i = 0; i < 6; i++) f[i] = dst[i];
    for (int i = 0; i < 6; i++) f[6 + i] = my_mac[i];
    put16(f + 12, type);
}

int net_init(const fdt_t *f) {
    if (virtio_net_init(f) != 0) return -1;
    virtio_net_mac(my_mac);
    up = 1;
    kprintf("  net       10.0.2.15, gateway 10.0.2.2 — not discovered, assumed: "
            "there is no DHCP client\n");
    return 0;
}

int net_present(void) { return up; }

/* ---- ARP ---------------------------------------------------------------- */

int net_arp_resolve(uint32_t ip, uint8_t out[6]) {
    if (!up) return -1;
    static const uint8_t bcast[6] = { 0xff,0xff,0xff,0xff,0xff,0xff };

    uint8_t f[60];
    for (unsigned i = 0; i < sizeof f; i++) f[i] = 0;
    eth_header(f, bcast, ET_ARP);
    put16(f + 14, 1);            /* ethernet   */
    put16(f + 16, ET_IPV4);      /* IPv4       */
    f[18] = 6; f[19] = 4;
    put16(f + 20, 1);            /* request    */
    for (int i = 0; i < 6; i++) f[22 + i] = my_mac[i];
    put32(f + 28, my_ip);
    /* target hardware address left zero: it is the question */
    put32(f + 38, ip);
    /* Padded to sixty bytes. An ARP request is forty-two, and the minimum
     * ethernet frame is sixty before the checksum — a card is entitled to
     * refuse a shorter one, and a hypervisor's virtual card is entitled to
     * pass it, which is exactly the difference that shows up only on real
     * hardware. */
    if (virtio_net_send(f, sizeof f) != 0) return -1;

    uint64_t deadline = timer_ticks() + timer_hz();   /* one second */
    uint8_t  r[1514];
    while (timer_ticks() < deadline) {
        int n = virtio_net_recv(r, sizeof r);
        if (n < 42) continue;
        if (get16(r + 12) != ET_ARP) continue;
        if (get16(r + 20) != 2) continue;             /* a reply     */
        if (get32(r + 28) != ip) continue;            /* about this address */
        for (int i = 0; i < 6; i++) out[i] = r[22 + i];
        return 0;
    }
    return -1;
}

/* ---- ICMP echo ---------------------------------------------------------- */

/* Chosen so a reply carrying anything else is visibly not ours. */
#define PING_ID   0x5354            /* "ST" */
#define PAYLOAD   32

int net_ping(uint32_t ip, uint16_t seq, int *check) {
    if (check) *check = 0;
    if (!up) return -1;

    uint8_t gw[6];
    if (net_arp_resolve(ip, gw) != 0) return -1;

    uint8_t f[14 + 20 + 8 + PAYLOAD];
    for (unsigned i = 0; i < sizeof f; i++) f[i] = 0;
    eth_header(f, gw, ET_IPV4);

    uint8_t *ipv4 = f + 14;
    ipv4[0] = 0x45;                        /* version 4, 5 words of header */
    ipv4[1] = 0;
    put16(ipv4 + 2, 20 + 8 + PAYLOAD);
    put16(ipv4 + 4, seq);                  /* identification */
    put16(ipv4 + 6, 0);
    ipv4[8]  = 64;                         /* ttl   */
    ipv4[9]  = IP_ICMP;
    put16(ipv4 + 10, 0);                   /* checksum, over the header only */
    put32(ipv4 + 12, my_ip);
    put32(ipv4 + 16, ip);
    put16(ipv4 + 10, checksum(ipv4, 20));

    uint8_t *icmp = ipv4 + 20;
    icmp[0] = 8;                           /* echo request */
    icmp[1] = 0;
    put16(icmp + 2, 0);
    put16(icmp + 4, PING_ID);
    put16(icmp + 6, seq);
    /* A payload that is not constant, so a reply that echoes the wrong request
     * or a buffer that was never written is visible. */
    for (int i = 0; i < PAYLOAD; i++) icmp[8 + i] = (uint8_t)(0x40 + ((seq + i) & 0x1f));
    put16(icmp + 2, checksum(icmp, 8 + PAYLOAD));

    uint64_t sent_us = micros();
    if (virtio_net_send(f, sizeof f) != 0) return -1;

    uint64_t deadline = timer_ticks() + timer_hz() * 2;
    uint8_t  r[1514];
    while (timer_ticks() < deadline) {
        int n = virtio_net_recv(r, sizeof r);
        if (n < 14 + 20 + 8) continue;
        if (get16(r + 12) != ET_IPV4) continue;
        uint8_t *rip = r + 14;
        unsigned ihl = (unsigned)(rip[0] & 0x0f) * 4;
        if ((rip[0] >> 4) != 4 || ihl < 20) continue;
        if (rip[9] != IP_ICMP) continue;
        if (get32(rip + 16) != my_ip) continue;       /* addressed to us */
        /* The header's own checksum, recomputed. A reply that reached us with
         * a bad one is a reply this kernel must not believe. */
        if (checksum(rip, ihl) != 0) continue;

        uint8_t *ric = rip + ihl;
        if (n < (int)(14 + ihl + 8 + PAYLOAD)) continue;
        if (ric[0] != 0) continue;                    /* echo reply */
        if (get16(ric + 4) != PING_ID) continue;
        if (get16(ric + 6) != seq) continue;
        if (checksum(ric, 8 + PAYLOAD) != 0) continue;

        int same = 1;
        for (int i = 0; i < PAYLOAD; i++)
            if (ric[8 + i] != (uint8_t)(0x40 + ((seq + i) & 0x1f))) same = 0;
        if (check) *check = same;
        uint64_t took = micros() - sent_us;
        return (int)(took > 0x7fffffff ? 0x7fffffff : took);
    }
    (void)gw_ip;
    return -1;
}
