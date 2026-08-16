#ifndef STAM_NET_H
#define STAM_NET_H
#include <stdint.h>
#include "fdt.h"

/* ---- the card ----------------------------------------------------------- */
int  virtio_net_init(const fdt_t *f);
int  virtio_net_present(void);
void virtio_net_mac(uint8_t out[6]);
/* One ethernet frame, without the virtio header and without the trailing
 * checksum — the card adds both. 0 on success. */
int  virtio_net_send(const void *frame, unsigned len);
/* The next frame that arrived, or -1 if none is waiting. Never blocks. */
int  virtio_net_recv(void *out, unsigned max);
/* From the interrupt handler: acknowledge the card and wake whoever is waiting
 * for a frame. */
void virtio_net_interrupt(void);
/* Whether an interrupt from the card has ever actually arrived. Until one has,
 * every wait polls — "the tree named an interrupt" is not the same claim as
 * "the interrupt arrives", and it is earned by one and then trusted (D-086). */
int  virtio_net_irq_works(void);
/* The address a thread waits on for a frame. Meaningless except as a token. */
const void *virtio_net_wait_token(void);

/* ---- what runs on top of it ---------------------------------------------
 *
 * Ethernet, ARP, IPv4, ICMP echo and UDP. Enough that a program can send a
 * datagram and get one back, and no more: there is no TCP, no routing table
 * beyond "the gateway takes everything not on this network", no fragmentation
 * and no options.
 *
 * The card is polled, so this stack only runs when something calls net_poll —
 * which the two blocking paths (waiting for a reply, waiting on a socket) do
 * in their own loops. Nothing arrives while every thread is busy elsewhere.
 * That is a real limit and it is written down rather than hidden: it is the
 * same stage the disk was at before it earned its interrupt (D-086). */
int  net_init(const fdt_t *f);
int  net_present(void);
/* Drain the card and dispatch whatever arrived. Cheap when nothing has. */
void net_poll(void);
/* Wait for something to arrive, up to `ticks`. Sleeps when the card's
 * interrupt has proved itself and spins when it has not, so a machine whose
 * card is silent still makes progress. */
void net_wait(uint64_t ticks);

/* One IPv4 packet with this payload, routed and addressed. The layer above
 * builds its own header; this one owns the network layer and below.
 * 0 on success. */
int net_ip_send(uint32_t dst, uint8_t proto, const void *payload, unsigned len);
/* The internet checksum over the pseudo-header TCP and UDP share — the two
 * addresses, the protocol and the length — folded together with the segment.
 * Returns 0 over a segment whose checksum is already correct. */
uint16_t net_ip_checksum_with_pseudo(uint32_t src, uint32_t dst, uint8_t proto,
                                     const void *seg, unsigned len);

/* Our own address, once DHCP has found one — 0 until then. */
uint32_t net_my_ip(void);
uint32_t net_gateway(void);

/* Ask who has this address, and wait. 0 if a reply came, and `out` is filled. */
int  net_arp_resolve(uint32_t ip, uint8_t out[6]);
/* Send one echo request and wait for its reply. Returns how many microseconds
 * it took, or -1. `check` is set to 1 when the payload came back unchanged,
 * and that is the claim worth making — a reply of the right shape carrying
 * different bytes is not an echo. */
int  net_ping(uint32_t ip, uint16_t seq, int *check);

/* ---- UDP ----------------------------------------------------------------
 *
 * A socket holds one datagram. Not a queue: a second arriving before the first
 * is read replaces nothing and is dropped, which is what UDP permits and what
 * this kernel can honestly do today. A program that reads in a loop between
 * sends — which is every program here — never notices. */
typedef struct udp_sock udp_sock_t;
udp_sock_t *udp_open(void);
void        udp_close(udp_sock_t *s);
/* 0, or -1 if the port is taken. Port 0 means "choose one". */
int  udp_bind(udp_sock_t *s, uint16_t port);
uint16_t udp_port(const udp_sock_t *s);
int  udp_sendto(udp_sock_t *s, uint32_t ip, uint16_t port,
                const void *buf, unsigned len);
/* The waiting datagram, or -1 if there is none. Never blocks; the caller polls
 * so that it, and not this file, decides how long to wait. */
int  udp_recvfrom(udp_sock_t *s, void *buf, unsigned max,
                  uint32_t *from_ip, uint16_t *from_port);

#define IPV4(a,b,c,d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                       ((uint32_t)(c) << 8)  |  (uint32_t)(d))
#endif
