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

/* ---- what runs on top of it ---------------------------------------------
 *
 * Enough to prove the card works both ways and nothing more: address
 * resolution, so a frame can be addressed to a machine named by an IP address,
 * and an echo, so the answer proves the whole path. There is no socket, no
 * routing table and no TCP; a program cannot reach any of this yet. */
int  net_init(const fdt_t *f);
int  net_present(void);
/* Ask who has this address, and wait. 0 if a reply came, and `out` is filled. */
int  net_arp_resolve(uint32_t ip, uint8_t out[6]);
/* Send one echo request and wait for its reply. Returns how many microseconds
 * it took, or -1. `check` is set to 1 when the payload came back
 * unchanged, and that is the claim worth making — a reply of the right shape
 * carrying different bytes is not an echo. */
int  net_ping(uint32_t ip, uint16_t seq, int *check);

#define IPV4(a,b,c,d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                       ((uint32_t)(c) << 8)  |  (uint32_t)(d))
#endif
