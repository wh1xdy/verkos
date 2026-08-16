/* stam — ethernet, ARP, IPv4, ICMP, UDP, and a DHCP client.
 *
 * The smallest stack that lets a *program* reach another machine, which is a
 * different claim from the one this file used to make. Before, the kernel
 * pinged a gateway during boot and nothing else could touch the card; that
 * proved the card worked and left the network outside the system. A program
 * with a socket is the network being part of it.
 *
 * ---- one loop, not one thread ----
 *
 * The card is polled, so frames arrive only when something calls net_poll.
 * Both blocking paths do it in their own loop: the kernel waiting for an echo,
 * and a process waiting in recvfrom. Nothing arrives while every thread is
 * busy elsewhere, and a datagram sent to a machine that is not currently
 * listening is dropped. That is a real limit, and it is the same stage the
 * disk was at before it earned its interrupt (D-086) — polled first, proved,
 * and only then slept on.
 *
 * ---- alignment ----
 *
 * Every multi-byte field is read and written a byte at a time. Not pedantry:
 * stam is built with -mstrict-align, and a packet is a stream of bytes at
 * offsets nobody chose for the compiler's benefit. An IPv4 source address sits
 * fourteen bytes into the frame plus twelve into the header — offset 26, which
 * is even and not a multiple of four. A `uint32_t *` there is an alignment
 * fault on some paths and correct on others, which is the worst kind; the byte
 * accessors cost nothing and cannot be wrong.
 *
 * ---- what is not here ----
 *
 * No TCP. No fragmentation, no IP options, no multicast, no netmask arithmetic
 * beyond "not on this network goes to the gateway". A socket holds exactly one
 * datagram. Every one of those is a thing a real stack does and this one does
 * not, listed here so that nobody has to find out by reading the code.
 */
#include "net.h"
#include "virtio.h"
#include "console.h"
#include "timer.h"
#include "sched.h"
#include "tcp.h"
#include <stdint.h>

#define ET_ARP   0x0806
#define ET_IPV4  0x0800
#define IP_ICMP  1
#define IP_UDP   17
#define IP_TCP   6

static uint8_t  my_mac[6];
static uint32_t my_ip;                 /* 0 until DHCP has answered */
static uint32_t my_mask;
static uint32_t gw_ip;
static uint32_t dns_ip;
static int      up;

static const uint8_t bcast_mac[6] = { 0xff,0xff,0xff,0xff,0xff,0xff };

/* Byte order on the wire is big-endian, and every field below is written by
 * these so that no line has to think about it. */
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

/* The internet checksum, in two halves so that a UDP pseudo-header can be
 * folded in with the datagram. An odd trailing byte is padded with zero on the
 * right, which is not the same as adding it as-is and is where a hand-written
 * one usually goes wrong. */
static uint32_t sum_add(uint32_t sum, const uint8_t *p, unsigned n) {
    unsigned i = 0;
    for (; i + 1 < n; i += 2) sum += get16(p + i);
    if (i < n) sum += (uint32_t)p[i] << 8;
    return sum;
}
static uint16_t sum_fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}
static uint16_t checksum(const uint8_t *p, unsigned n) { return sum_fold(sum_add(0, p, n)); }

/* Microseconds off the architected counter, not timer ticks. A round trip to a
 * hypervisor's built-in gateway is far shorter than one tick, so measuring in
 * ticks printed "0 ticks" — a number that is the same whether the reply took no
 * time or the clock is not running, which is the shape this project keeps
 * finding. */
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

/* ---- what arrived ------------------------------------------------------- */

/* An ARP cache of four. Four, because this kernel talks to a gateway and
 * perhaps one machine beside it; a table that has to be evicted from is a
 * table with a policy, and there is nothing here to justify one yet. */
#define ARP_N 4
static struct { uint32_t ip; uint8_t mac[6]; int used; } arp[ARP_N];

static void arp_learn(uint32_t ip, const uint8_t mac[6]) {
    for (int i = 0; i < ARP_N; i++)
        if (arp[i].used && arp[i].ip == ip) {
            for (int k = 0; k < 6; k++) arp[i].mac[k] = mac[k];
            return;
        }
    for (int i = 0; i < ARP_N; i++)
        if (!arp[i].used) {
            arp[i].used = 1; arp[i].ip = ip;
            for (int k = 0; k < 6; k++) arp[i].mac[k] = mac[k];
            return;
        }
    /* Full. The oldest is not tracked, so the first is replaced — said here
     * rather than pretended away. */
    arp[0].ip = ip;
    for (int k = 0; k < 6; k++) arp[0].mac[k] = mac[k];
}
static int arp_lookup(uint32_t ip, uint8_t out[6]) {
    for (int i = 0; i < ARP_N; i++)
        if (arp[i].used && arp[i].ip == ip) {
            for (int k = 0; k < 6; k++) out[k] = arp[i].mac[k];
            return 0;
        }
    return -1;
}

/* The one echo reply the kernel is waiting for, if it is waiting for one. */
#define PING_ID   0x5354            /* "ST" */
#define PAYLOAD   32
static volatile int  ping_seq = -1;
static volatile int  ping_got;
static volatile int  ping_intact;

/* ---- UDP sockets -------------------------------------------------------- */

#define NSOCK    8
#define DGRAM_MAX 1472              /* 1500 - 20 - 8 */

struct udp_sock {
    int      used;
    uint16_t port;                  /* host order; 0 = not bound */
    int      have;
    uint32_t from_ip;
    uint16_t from_port;
    unsigned len;
    uint8_t  data[DGRAM_MAX];
};
static struct udp_sock socks[NSOCK];
static uint16_t next_ephemeral = 49152;

static int port_taken(uint16_t p) {
    for (int i = 0; i < NSOCK; i++)
        if (socks[i].used && socks[i].port == p) return 1;
    return 0;
}

udp_sock_t *udp_open(void) {
    if (!up) return 0;
    for (int i = 0; i < NSOCK; i++)
        if (!socks[i].used) {
            socks[i].used = 1;
            socks[i].port = 0;
            socks[i].have = 0;
            return &socks[i];
        }
    return 0;
}
void udp_close(udp_sock_t *s) { if (s) { s->used = 0; s->port = 0; s->have = 0; } }

int udp_bind(udp_sock_t *s, uint16_t port) {
    if (!s || !s->used) return -1;
    if (port == 0) {
        for (int tries = 0; tries < 4096; tries++) {
            uint16_t p = next_ephemeral++;
            if (next_ephemeral == 0) next_ephemeral = 49152;
            if (!port_taken(p)) { s->port = p; return 0; }
        }
        return -1;
    }
    if (port_taken(port)) return -1;
    s->port = port;
    return 0;
}
uint16_t udp_port(const udp_sock_t *s) { return s ? s->port : 0; }

int udp_recvfrom(udp_sock_t *s, void *buf, unsigned max,
                 uint32_t *from_ip, uint16_t *from_port) {
    if (!s || !s->used || !s->have) return -1;
    unsigned n = s->len < max ? s->len : max;
    uint8_t *d = buf;
    for (unsigned i = 0; i < n; i++) d[i] = s->data[i];
    if (from_ip)   *from_ip   = s->from_ip;
    if (from_port) *from_port = s->from_port;
    s->have = 0;
    return (int)n;
}

/* Where a frame for this address has to go: the machine itself if it is on our
 * network, the gateway otherwise. The whole routing table. */
static int route_mac(uint32_t ip, uint8_t out[6]) {
    uint32_t next = ip;
    if (my_mask && ((ip ^ my_ip) & my_mask)) next = gw_ip;
    if (!next) return -1;
    if (arp_lookup(next, out) == 0) return 0;
    return net_arp_resolve(next, out);
}

/* One datagram out. `src_ip` is a parameter and not always my_ip because DHCP
 * has to send from 0.0.0.0 before it has an address — the one case where the
 * source is not the machine's own. */
static int udp_send_from(uint32_t src_ip, uint16_t src_port,
                         uint32_t dst_ip, uint16_t dst_port,
                         const uint8_t dst_mac[6],
                         const void *buf, unsigned len) {
    if (len > DGRAM_MAX) return -1;
    uint8_t f[14 + 20 + 8 + DGRAM_MAX];
    for (unsigned i = 0; i < 14 + 20 + 8; i++) f[i] = 0;
    eth_header(f, dst_mac, ET_IPV4);

    uint8_t *ip = f + 14;
    ip[0] = 0x45;
    put16(ip + 2, (uint16_t)(20 + 8 + len));
    put16(ip + 4, (uint16_t)(src_port ^ dst_port));   /* identification */
    ip[8] = 64;
    ip[9] = IP_UDP;
    put32(ip + 12, src_ip);
    put32(ip + 16, dst_ip);
    put16(ip + 10, checksum(ip, 20));

    uint8_t *udp = ip + 20;
    put16(udp + 0, src_port);
    put16(udp + 2, dst_port);
    put16(udp + 4, (uint16_t)(8 + len));
    put16(udp + 6, 0);
    const uint8_t *s = buf;
    for (unsigned i = 0; i < len; i++) udp[8 + i] = s[i];

    /* The checksum covers a pseudo-header that is not in the packet: the two
     * addresses, the protocol and the UDP length. Leaving it zero is legal in
     * IPv4 and is how a datagram that is subtly wrong gets accepted anyway, so
     * it is computed. A computed value of zero is sent as 0xffff, because zero
     * means "not computed" and the two must not be confused. */
    uint8_t pseudo[12];
    put32(pseudo + 0, src_ip);
    put32(pseudo + 4, dst_ip);
    pseudo[8] = 0; pseudo[9] = IP_UDP;
    put16(pseudo + 10, (uint16_t)(8 + len));
    uint32_t sum = sum_add(0, pseudo, sizeof pseudo);
    sum = sum_add(sum, udp, 8 + len);
    uint16_t c = sum_fold(sum);
    put16(udp + 6, c ? c : 0xffff);

    return virtio_net_send(f, 14 + 20 + 8 + len);
}

int udp_sendto(udp_sock_t *s, uint32_t ip, uint16_t port,
               const void *buf, unsigned len) {
    if (!s || !s->used || !up) return -1;
    if (s->port == 0 && udp_bind(s, 0) != 0) return -1;
    uint8_t mac[6];
    if (route_mac(ip, mac) != 0) return -1;
    return udp_send_from(my_ip, s->port, ip, port, mac, buf, len);
}

/* ---- what the layers above use ------------------------------------------ */

uint16_t net_ip_checksum_with_pseudo(uint32_t src, uint32_t dst, uint8_t proto,
                                     const void *seg, unsigned len) {
    uint8_t pseudo[12];
    put32(pseudo + 0, src);
    put32(pseudo + 4, dst);
    pseudo[8] = 0; pseudo[9] = proto;
    put16(pseudo + 10, (uint16_t)len);
    uint32_t sum = sum_add(0, pseudo, sizeof pseudo);
    sum = sum_add(sum, seg, len);
    return sum_fold(sum);
}

int net_ip_send(uint32_t dst, uint8_t proto, const void *payload, unsigned len) {
    if (len > 1480) return -1;
    uint8_t mac[6];
    if (route_mac(dst, mac) != 0) return -1;

    uint8_t f[14 + 20 + 1480];
    for (unsigned i = 0; i < 14 + 20; i++) f[i] = 0;
    eth_header(f, mac, ET_IPV4);
    uint8_t *ip = f + 14;
    ip[0] = 0x45;
    put16(ip + 2, (uint16_t)(20 + len));
    /* An identification that varies, because two packets from this machine
     * with the same one and the same addresses are, to anything that
     * reassembles, one packet. Nothing here fragments, but the middle of a
     * network does not know that. */
    static uint16_t ident;
    put16(ip + 4, ident++);
    ip[8] = 64;
    ip[9] = proto;
    put32(ip + 12, my_ip);
    put32(ip + 16, dst);
    put16(ip + 10, checksum(ip, 20));
    const uint8_t *s = payload;
    for (unsigned i = 0; i < len; i++) f[14 + 20 + i] = s[i];
    return virtio_net_send(f, 14 + 20 + len);
}

/* ---- the dispatcher ----------------------------------------------------- */

static void handle_arp(const uint8_t *r, int n) {
    if (n < 42) return;
    if (get16(r + 16) != ET_IPV4 || r[18] != 6 || r[19] != 4) return;
    uint16_t op = get16(r + 20);
    uint32_t sender = get32(r + 28);
    arp_learn(sender, r + 22);
    if (op != 1) return;                       /* a request; the rest is a reply */
    if (!my_ip || get32(r + 38) != my_ip) return;

    /* Answer it. A machine that asks and never answers is reachable only until
     * the other end's cache expires, which turns into "the network worked for
     * a minute" — the hardest kind of fault to read. */
    uint8_t f[60];
    for (unsigned i = 0; i < sizeof f; i++) f[i] = 0;
    eth_header(f, r + 22, ET_ARP);
    put16(f + 14, 1);
    put16(f + 16, ET_IPV4);
    f[18] = 6; f[19] = 4;
    put16(f + 20, 2);                          /* reply */
    for (int i = 0; i < 6; i++) f[22 + i] = my_mac[i];
    put32(f + 28, my_ip);
    for (int i = 0; i < 6; i++) f[32 + i] = r[22 + i];
    put32(f + 38, sender);
    virtio_net_send(f, sizeof f);
}

static void handle_icmp(const uint8_t *ic, unsigned n, uint32_t src) {
    if (n < 8) return;
    if (checksum(ic, n) != 0) return;

    if (ic[0] == 0) {                          /* echo reply */
        if (ping_seq < 0) return;
        if (get16(ic + 4) != PING_ID) return;
        if (get16(ic + 6) != (uint16_t)ping_seq) return;
        if (n < 8 + PAYLOAD) return;
        int same = 1;
        for (int i = 0; i < PAYLOAD; i++)
            if (ic[8 + i] != (uint8_t)(0x40 + ((ping_seq + i) & 0x1f))) same = 0;
        ping_intact = same;
        ping_got = 1;
        return;
    }
    if (ic[0] == 8 && my_ip) {                 /* echo request: answer it */
        unsigned len = n > 14 + 20 + 8 + DGRAM_MAX ? DGRAM_MAX : n;
        uint8_t f[14 + 20 + 8 + 64];
        if (len > sizeof f - 14 - 20) return;
        uint8_t mac[6];
        if (route_mac(src, mac) != 0) return;
        for (unsigned i = 0; i < sizeof f; i++) f[i] = 0;
        eth_header(f, mac, ET_IPV4);
        uint8_t *ip = f + 14;
        ip[0] = 0x45;
        put16(ip + 2, (uint16_t)(20 + len));
        ip[8] = 64; ip[9] = IP_ICMP;
        put32(ip + 12, my_ip);
        put32(ip + 16, src);
        put16(ip + 10, checksum(ip, 20));
        uint8_t *o = ip + 20;
        for (unsigned i = 0; i < len; i++) o[i] = ic[i];
        o[0] = 0;                              /* reply */
        put16(o + 2, 0);
        put16(o + 2, checksum(o, len));
        virtio_net_send(f, 14 + 20 + len);
    }
}

static void handle_udp(const uint8_t *u, unsigned n, uint32_t src) {
    if (n < 8) return;
    uint16_t sport = get16(u + 0), dport = get16(u + 2);
    uint16_t ulen  = get16(u + 4);
    if (ulen < 8 || ulen > n) return;
    unsigned payload = (unsigned)ulen - 8;
    for (int i = 0; i < NSOCK; i++) {
        if (!socks[i].used || socks[i].port != dport) continue;
        /* Dropped rather than queued if one is already waiting; UDP permits it
         * and this kernel holds one. */
        if (socks[i].have) return;
        unsigned c = payload > DGRAM_MAX ? DGRAM_MAX : payload;
        for (unsigned k = 0; k < c; k++) socks[i].data[k] = u[8 + k];
        socks[i].len = c;
        socks[i].from_ip = src;
        socks[i].from_port = sport;
        socks[i].have = 1;
        return;
    }
}

static void handle_ipv4(const uint8_t *r, int n) {
    if (n < 14 + 20) return;
    const uint8_t *ip = r + 14;
    unsigned ihl = (unsigned)(ip[0] & 0x0f) * 4;
    if ((ip[0] >> 4) != 4 || ihl < 20) return;
    if ((unsigned)n < 14 + ihl) return;
    /* The header's own checksum, recomputed. A packet that reached us with a
     * bad one is a packet this kernel must not believe. */
    if (checksum(ip, ihl) != 0) return;

    unsigned total = get16(ip + 2);
    if (total < ihl || (unsigned)n < 14 + total) return;
    uint32_t src = get32(ip + 12), dst = get32(ip + 16);
    /* Ours, or a broadcast — the second because DHCP's own reply arrives
     * before this machine has an address to be addressed by. */
    if (my_ip && dst != my_ip && dst != 0xffffffffu) return;

    const uint8_t *payload = ip + ihl;
    unsigned plen = total - ihl;
    if (ip[9] == IP_ICMP) handle_icmp(payload, plen, src);
    else if (ip[9] == IP_UDP) handle_udp(payload, plen, src);
    else if (ip[9] == IP_TCP) tcp_input(payload, plen, src);
}

/* Wait for something to arrive.
 *
 * Sleeps once the card's interrupt has proved itself, and spins before that —
 * on the PCI transport, where there is no interrupt number to enable, it spins
 * forever, which is why the boot line says which of the two this machine got.
 *
 * The bound is the caller's, and there always is one: every wait in this file
 * ends in an error rather than in a machine that stops. */
void net_wait(uint64_t ticks) {
    if (virtio_net_irq_works()) {
        uint64_t s = sched_lock_irq();
        thread_block_on_pipe_until(virtio_net_wait_token(), ticks);
        sched_unlock_irq(s);
    } else {
        thread_yield();
    }
}

void net_poll(void) {
    if (!virtio_net_present()) return;
    uint8_t r[1514];
    for (int guard = 0; guard < 32; guard++) {
        int n = virtio_net_recv(r, sizeof r);
        if (n < 14) return;
        uint16_t type = get16(r + 12);
        if (type == ET_ARP)       handle_arp(r, n);
        else if (type == ET_IPV4) handle_ipv4(r, n);
    }
}

/* ---- ARP, asked rather than answered ------------------------------------ */

int net_arp_resolve(uint32_t ip, uint8_t out[6]) {
    if (!virtio_net_present()) return -1;
    if (arp_lookup(ip, out) == 0) return 0;

    uint8_t f[60];
    for (unsigned i = 0; i < sizeof f; i++) f[i] = 0;
    eth_header(f, bcast_mac, ET_ARP);
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
    while (timer_ticks() < deadline) {
        net_poll();
        if (arp_lookup(ip, out) == 0) return 0;
        net_wait(1);
    }
    return -1;
}

/* ---- ICMP echo ---------------------------------------------------------- */

int net_ping(uint32_t ip, uint16_t seq, int *check) {
    if (check) *check = 0;
    if (!virtio_net_present()) return -1;

    uint8_t gw[6];
    if (route_mac(ip, gw) != 0 && net_arp_resolve(ip, gw) != 0) return -1;

    uint8_t f[14 + 20 + 8 + PAYLOAD];
    for (unsigned i = 0; i < sizeof f; i++) f[i] = 0;
    eth_header(f, gw, ET_IPV4);

    uint8_t *ipv4 = f + 14;
    ipv4[0] = 0x45;                        /* version 4, 5 words of header */
    put16(ipv4 + 2, 20 + 8 + PAYLOAD);
    put16(ipv4 + 4, seq);                  /* identification */
    ipv4[8]  = 64;                         /* ttl   */
    ipv4[9]  = IP_ICMP;
    put32(ipv4 + 12, my_ip);
    put32(ipv4 + 16, ip);
    put16(ipv4 + 10, checksum(ipv4, 20));

    uint8_t *icmp = ipv4 + 20;
    icmp[0] = 8;                           /* echo request */
    put16(icmp + 4, PING_ID);
    put16(icmp + 6, seq);
    /* A payload that is not constant, so a reply that echoes the wrong request
     * or a buffer that was never written is visible. */
    for (int i = 0; i < PAYLOAD; i++) icmp[8 + i] = (uint8_t)(0x40 + ((seq + i) & 0x1f));
    put16(icmp + 2, checksum(icmp, 8 + PAYLOAD));

    ping_got = 0; ping_intact = 0; ping_seq = (int)seq;
    uint64_t sent_us = micros();
    if (virtio_net_send(f, sizeof f) != 0) { ping_seq = -1; return -1; }

    uint64_t deadline = timer_ticks() + timer_hz() * 2;
    while (timer_ticks() < deadline) {
        net_poll();
        if (ping_got) {
            uint64_t took = micros() - sent_us;
            if (check) *check = ping_intact;
            ping_seq = -1;
            return (int)(took > 0x7fffffff ? 0x7fffffff : took);
        }
        net_wait(1);
    }
    ping_seq = -1;
    return -1;
}

/* ---- DHCP ---------------------------------------------------------------
 *
 * Four messages: discover, offer, request, acknowledge. The address in this
 * kernel used to be a constant with a comment saying it was QEMU's, which is
 * true of QEMU and of nothing else. Asking for it is both more correct and a
 * better proof: an address that came back in an option field was not compiled
 * in.
 */
#define DHCP_MAGIC 0x63825363u

static uint32_t dhcp_option(const uint8_t *p, unsigned n, uint8_t want, unsigned *len_out) {
    unsigned i = 240;                          /* past the fixed part + cookie */
    while (i + 1 < n) {
        uint8_t code = p[i];
        if (code == 255) break;                /* end   */
        if (code == 0) { i++; continue; }       /* pad   */
        unsigned l = p[i + 1];
        if (i + 2 + l > n) break;
        if (code == want) { if (len_out) *len_out = l; return i + 2; }
        i += 2 + l;
    }
    if (len_out) *len_out = 0;
    return 0;
}

static void dhcp_fill(uint8_t *m, unsigned n, uint32_t xid, uint8_t type,
                      uint32_t server, uint32_t requested) {
    for (unsigned i = 0; i < n; i++) m[i] = 0;
    m[0] = 1;                    /* boot request        */
    m[1] = 1;                    /* ethernet            */
    m[2] = 6;                    /* address length      */
    put32(m + 4, xid);
    put16(m + 10, 0x8000);       /* ask for a broadcast reply */
    for (int i = 0; i < 6; i++) m[28 + i] = my_mac[i];
    put32(m + 236, DHCP_MAGIC);
    unsigned o = 240;
    m[o++] = 53; m[o++] = 1; m[o++] = type;
    if (server)    { m[o++] = 54; m[o++] = 4; put32(m + o, server);    o += 4; }
    if (requested) { m[o++] = 50; m[o++] = 4; put32(m + o, requested); o += 4; }
    m[o++] = 55; m[o++] = 3; m[o++] = 1; m[o++] = 3; m[o++] = 6;  /* mask, router, dns */
    m[o++] = 255;
}

#define DHCP_LEN 300             /* the fixed part plus room for the options */

static int dhcp_wait(udp_sock_t *s, uint32_t xid, uint8_t want, uint8_t *m, unsigned n) {
    uint64_t deadline = timer_ticks() + timer_hz() * 2;
    while (timer_ticks() < deadline) {
        net_poll();
        uint32_t from; uint16_t fport;
        int got = udp_recvfrom(s, m, n, &from, &fport);
        if (got < 0) { net_wait(1); continue; }
        if (got < 240) continue;
        if (m[0] != 2) continue;                   /* a reply       */
        if (get32(m + 4) != xid) continue;         /* to ours       */
        if (get32(m + 236) != DHCP_MAGIC) continue;
        unsigned l = 0;
        unsigned t = dhcp_option(m, (unsigned)got, 53, &l);
        if (!t || l != 1 || m[t] != want) continue;
        return got;
    }
    return -1;
}

static int dhcp_configure(void) {
    udp_sock_t *s = udp_open();
    if (!s) return -1;
    if (udp_bind(s, 68) != 0) { udp_close(s); return -1; }

    /* The transaction id has to differ between boots, or a server that
     * remembers the last one answers a question nobody asked. The counter is
     * the only thing here that is not the same on every boot. */
    uint64_t c; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(c));
    uint32_t xid = (uint32_t)c ^ 0x5354414du;

    uint8_t msg[DHCP_LEN], reply[DGRAM_MAX];
    dhcp_fill(msg, sizeof msg, xid, 1 /* discover */, 0, 0);
    if (udp_send_from(0, 68, 0xffffffffu, 67, bcast_mac, msg, sizeof msg) != 0) {
        udp_close(s); return -1;
    }
    int n = dhcp_wait(s, xid, 2 /* offer */, reply, sizeof reply);
    if (n < 0) { udp_close(s); return -1; }

    uint32_t offered = get32(reply + 16);          /* yiaddr */
    unsigned l = 0, o = dhcp_option(reply, (unsigned)n, 54, &l);
    uint32_t server = (o && l == 4) ? get32(reply + o) : 0;

    dhcp_fill(msg, sizeof msg, xid, 3 /* request */, server, offered);
    if (udp_send_from(0, 68, 0xffffffffu, 67, bcast_mac, msg, sizeof msg) != 0) {
        udp_close(s); return -1;
    }
    n = dhcp_wait(s, xid, 5 /* ack */, reply, sizeof reply);
    if (n < 0) { udp_close(s); return -1; }

    my_ip = get32(reply + 16);
    o = dhcp_option(reply, (unsigned)n, 1, &l);  if (o && l == 4) my_mask = get32(reply + o);
    o = dhcp_option(reply, (unsigned)n, 3, &l);  if (o && l >= 4) gw_ip   = get32(reply + o);
    o = dhcp_option(reply, (unsigned)n, 6, &l);  if (o && l >= 4) dns_ip  = get32(reply + o);
    udp_close(s);
    return my_ip ? 0 : -1;
}

/* ---- setting up --------------------------------------------------------- */

static void print_ip(const char *label, uint32_t ip) {
    kprintf("%s%u.%u.%u.%u", label, (ip >> 24) & 0xff, (ip >> 16) & 0xff,
            (ip >> 8) & 0xff, ip & 0xff);
}

int net_init(const fdt_t *f) {
    if (virtio_net_init(f) != 0) return -1;
    virtio_net_mac(my_mac);
    up = 1;

    if (dhcp_configure() != 0) {
        /* Named as an assumption, out loud, so that nobody reads the address
         * below as something the machine found out. */
        my_ip = IPV4(10,0,2,15); my_mask = IPV4(255,255,255,0); gw_ip = IPV4(10,0,2,2);
        kprintf("  net       no DHCP answer; assuming 10.0.2.15 via 10.0.2.2\n");
        return 0;
    }
    print_ip("  net       ", my_ip);
    print_ip("/", my_mask);
    print_ip(" via ", gw_ip);
    if (dns_ip) print_ip(", dns ", dns_ip);
    kprintf(" — from DHCP, not compiled in\n");
    return 0;
}

int net_present(void) { return up; }
uint32_t net_my_ip(void)   { return my_ip; }
uint32_t net_gateway(void) { return gw_ip; }
