/* vdhcp — VerkOS' own minimal DHCPv4 client.
 *
 * Replaces dhcpcd for the common case: lease an IPv4 address on one interface,
 * apply the address/netmask/default-route, and write /etc/resolv.conf from the
 * offered DNS servers. Continues the "make the network ours" thread
 * (systemd-networkd -> dhcpcd -> vdhcp).
 *
 * Deliberately minimal: IPv4 only, single interface, no privsep, no DHCPv6.
 * It does the DISCOVER -> OFFER -> REQUEST -> ACK handshake, applies the lease
 * via ioctls, then stays running and renews at roughly half the lease time so
 * systemd sees a live service.
 *
 * The handshake runs over a raw AF_PACKET socket (building the IP+UDP headers
 * by hand) rather than a plain UDP socket, because the whole point of DHCP is
 * to get an address before one is configured: a UDP socket can't receive the
 * server's reply until the interface already has an IP, but AF_PACKET sees the
 * frame regardless. Config changes still go through ordinary AF_INET ioctls.
 *
 * Usage: vdhcp [INTERFACE]   (default: eth0)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>

#define DHCP_MAGIC    0x63825363u
#define BOOTREQUEST   1
#define BOOTREPLY     2
#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6

struct dhcp_msg {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint8_t  options[312];
} __attribute__((packed));

static const char *g_if = "eth0";
static int g_ifindex;
static int g_cfg;    /* AF_INET socket, for interface ioctls */

static void logmsg(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "vdhcp: "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

/* fetch the interface MAC into mac[6]; return 0 on success */
static int get_mac(uint8_t mac[6]) {
    struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", g_if);
    if (ioctl(g_cfg, SIOCGIFHWADDR, &ifr) < 0) return -1;
    memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    return 0;
}

/* bring the interface up */
static int if_up(void) {
    struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", g_if);
    if (ioctl(g_cfg, SIOCGIFFLAGS, &ifr) < 0) return -1;
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    return ioctl(g_cfg, SIOCSIFFLAGS, &ifr);
}

/* append a DHCP option; returns new write pointer */
static uint8_t *opt(uint8_t *p, uint8_t code, uint8_t len, const void *val) {
    *p++ = code; *p++ = len; memcpy(p, val, len); return p + len;
}

/* build a DHCP message of the given type; returns total length */
static int build(struct dhcp_msg *m, int type, uint32_t xid, const uint8_t mac[6],
                 uint32_t req_ip, uint32_t server_id) {
    memset(m, 0, sizeof *m);
    m->op = BOOTREQUEST; m->htype = 1; m->hlen = 6;
    m->xid = xid;
    m->flags = htons(0x8000);            /* ask the server to broadcast replies */
    memcpy(m->chaddr, mac, 6);
    uint8_t *p = m->options;
    uint32_t magic = htonl(DHCP_MAGIC); memcpy(p, &magic, 4); p += 4;
    uint8_t t = (uint8_t)type; p = opt(p, 53, 1, &t);
    if (req_ip)    p = opt(p, 50, 4, &req_ip);
    if (server_id) p = opt(p, 54, 4, &server_id);
    uint8_t prl[] = { 1, 3, 6, 15, 51 };  /* mask, router, dns, domain, lease */
    p = opt(p, 55, sizeof prl, prl);
    *p++ = 255;                           /* end */
    return (int)((uint8_t *)p - (uint8_t *)m);
}

/* parse a reply: return the DHCP message type, and fill outputs (network order) */
static int parse(const struct dhcp_msg *m, int len, uint32_t *yiaddr, uint32_t *server_id,
                 uint32_t *mask, uint32_t *router, uint32_t dns[8], int *ndns, uint32_t *lease) {
    if (len < (int)(offsetof(struct dhcp_msg, options) + 4)) return -1;
    uint32_t magic; memcpy(&magic, m->options, 4);
    if (ntohl(magic) != DHCP_MAGIC) return -1;
    *yiaddr = m->yiaddr;
    int type = 0; *ndns = 0;
    const uint8_t *p = m->options + 4;
    const uint8_t *end = (const uint8_t *)m + len;
    while (p < end && *p != 255) {
        if (*p == 0) { p++; continue; }
        uint8_t code = *p++; if (p >= end) break;
        uint8_t l = *p++; if (p + l > end) break;
        switch (code) {
            case 53: type = *p; break;
            case 54: memcpy(server_id, p, 4); break;
            case 1:  memcpy(mask, p, 4); break;
            case 3:  if (l >= 4) memcpy(router, p, 4); break;
            case 6:  for (int i = 0; i + 4 <= l && *ndns < 8; i += 4) memcpy(&dns[(*ndns)++], p + i, 4); break;
            case 51: if (l >= 4) { uint32_t v; memcpy(&v, p, 4); *lease = ntohl(v); } break;
        }
        p += l;
    }
    return type;
}

/* incremental ones-complement sum, and fold to a checksum */
static uint32_t sum16(const void *data, int len, uint32_t sum) {
    const uint8_t *p = data;
    while (len > 1) { uint16_t w; memcpy(&w, p, 2); sum += w; p += 2; len -= 2; }
    if (len) sum += *p;                 /* trailing odd byte, low-order */
    return sum;
}
static uint16_t fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

/* build IP + UDP + dhcp into frame; return total frame length */
static int build_frame(uint8_t *frame, const struct dhcp_msg *m, int dhcplen) {
    struct iphdr  *ip  = (struct iphdr  *)frame;
    struct udphdr *udp = (struct udphdr *)(frame + sizeof *ip);
    uint8_t *payload   = frame + sizeof *ip + sizeof *udp;
    memcpy(payload, m, dhcplen);
    int udplen = (int)sizeof *udp + dhcplen;
    int iplen  = (int)sizeof *ip + udplen;

    memset(ip, 0, sizeof *ip);
    ip->version = 4; ip->ihl = 5; ip->tot_len = htons(iplen);
    ip->ttl = 64; ip->protocol = IPPROTO_UDP;
    ip->saddr = htonl(INADDR_ANY);
    ip->daddr = htonl(INADDR_BROADCAST);
    ip->check = fold(sum16(ip, sizeof *ip, 0));

    memset(udp, 0, sizeof *udp);
    udp->source = htons(68); udp->dest = htons(67); udp->len = htons(udplen);
    struct __attribute__((packed)) {
        uint32_t src, dst; uint8_t zero, proto; uint16_t len;
    } ph = { ip->saddr, ip->daddr, 0, IPPROTO_UDP, htons(udplen) };
    uint32_t s = sum16(&ph, sizeof ph, 0);
    s = sum16(udp, udplen, s);
    udp->check = fold(s);
    if (udp->check == 0) udp->check = 0xffff;   /* 0 means "no checksum"; avoid it */
    return iplen;
}

/* send DISCOVER/REQUEST, wait up to 4s for a matching reply into `reply`;
 * returns dhcp payload length or -1 on timeout. */
static int exchange(int pkt, const struct dhcp_msg *m, int dhcplen,
                    struct dhcp_msg *reply, uint32_t xid) {
    uint8_t frame[1500];
    int flen = build_frame(frame, m, dhcplen);

    struct sockaddr_ll ll; memset(&ll, 0, sizeof ll);
    ll.sll_family = AF_PACKET; ll.sll_ifindex = g_ifindex;
    ll.sll_protocol = htons(ETH_P_IP);
    ll.sll_halen = 6; memset(ll.sll_addr, 0xff, 6);   /* broadcast MAC */
    if (sendto(pkt, frame, flen, 0, (struct sockaddr *)&ll, sizeof ll) < 0) {
        logmsg("send: %s", strerror(errno)); return -1;
    }

    struct timeval tv = { 4, 0 };
    setsockopt(pkt, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    for (;;) {
        uint8_t buf[1500];
        int n = recv(pkt, buf, sizeof buf, 0);
        if (n < 0) return -1;                              /* timeout */
        if (n < (int)sizeof(struct iphdr)) continue;
        struct iphdr *ip = (struct iphdr *)buf;
        if (ip->protocol != IPPROTO_UDP) continue;
        int ihl = ip->ihl * 4;
        if (n < ihl + (int)sizeof(struct udphdr)) continue;
        struct udphdr *udp = (struct udphdr *)(buf + ihl);
        if (ntohs(udp->dest) != 68) continue;              /* not a DHCP client packet */
        int dlen = n - ihl - (int)sizeof *udp;
        if (dlen < 4) continue;
        struct dhcp_msg *r = (struct dhcp_msg *)(buf + ihl + sizeof *udp);
        if (r->op != BOOTREPLY || r->xid != xid) continue; /* not ours */
        memcpy(reply, r, dlen > (int)sizeof *reply ? (int)sizeof *reply : dlen);
        return dlen;
    }
}

static void apply_lease(uint32_t ip, uint32_t mask, uint32_t router,
                        const uint32_t dns[8], int ndns) {
    struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", g_if);
    struct sockaddr_in *a = (struct sockaddr_in *)&ifr.ifr_addr;
    a->sin_family = AF_INET;
    a->sin_addr.s_addr = ip;   if (ioctl(g_cfg, SIOCSIFADDR, &ifr) < 0)    logmsg("set addr: %s", strerror(errno));
    a->sin_addr.s_addr = mask; if (ioctl(g_cfg, SIOCSIFNETMASK, &ifr) < 0) logmsg("set mask: %s", strerror(errno));

    if (router) {
        struct rtentry rt; memset(&rt, 0, sizeof rt);
        struct sockaddr_in *g = (struct sockaddr_in *)&rt.rt_gateway;
        struct sockaddr_in *d = (struct sockaddr_in *)&rt.rt_dst;
        struct sockaddr_in *m = (struct sockaddr_in *)&rt.rt_genmask;
        g->sin_family = d->sin_family = m->sin_family = AF_INET;
        g->sin_addr.s_addr = router;                 /* default via router */
        d->sin_addr.s_addr = 0; m->sin_addr.s_addr = 0;
        rt.rt_flags = RTF_UP | RTF_GATEWAY;
        rt.rt_dev = (char *)g_if;
        if (ioctl(g_cfg, SIOCADDRT, &rt) < 0 && errno != EEXIST) logmsg("route: %s", strerror(errno));
    }

    FILE *f = fopen("/etc/resolv.conf", "w");
    if (f) {
        for (int i = 0; i < ndns; i++) {
            struct in_addr in; in.s_addr = dns[i];
            fprintf(f, "nameserver %s\n", inet_ntoa(in));
        }
        fclose(f);
    }
    struct in_addr in; in.s_addr = ip;
    logmsg("%s: leased %s/%d", g_if, inet_ntoa(in), __builtin_popcount(mask));
}

int main(int argc, char **argv) {
    if (argc > 1 && argv[1][0] != '-') g_if = argv[1];

    g_cfg = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_cfg < 0) { logmsg("cfg socket: %s", strerror(errno)); return 1; }

    struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", g_if);
    if (ioctl(g_cfg, SIOCGIFINDEX, &ifr) < 0) { logmsg("no interface %s", g_if); return 1; }
    g_ifindex = ifr.ifr_ifindex;

    if (if_up() < 0) logmsg("could not bring %s up: %s", g_if, strerror(errno));
    uint8_t mac[6];
    if (get_mac(mac) < 0) { logmsg("no MAC for %s", g_if); return 1; }

    int pkt = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_IP));
    if (pkt < 0) { logmsg("packet socket: %s (need root)", strerror(errno)); return 1; }
    struct sockaddr_ll bl; memset(&bl, 0, sizeof bl);
    bl.sll_family = AF_PACKET; bl.sll_protocol = htons(ETH_P_IP); bl.sll_ifindex = g_ifindex;
    if (bind(pkt, (struct sockaddr *)&bl, sizeof bl) < 0) { logmsg("bind: %s", strerror(errno)); return 1; }

    srand((unsigned)(time(NULL) ^ getpid()));

    for (;;) {                                      /* (re)lease loop */
        uint32_t xid = (uint32_t)rand();
        struct dhcp_msg m, r;
        uint32_t yi = 0, sid = 0, mask = 0, router = 0, dns[8], lease = 3600;
        int ndns = 0, type;

        int n = exchange(pkt, &m, build(&m, DHCP_DISCOVER, xid, mac, 0, 0), &r, xid);
        if (n < 0) { logmsg("no DHCPOFFER, retrying"); sleep(3); continue; }
        type = parse(&r, n, &yi, &sid, &mask, &router, dns, &ndns, &lease);
        if (type != DHCP_OFFER || !yi) { logmsg("bad OFFER, retrying"); sleep(3); continue; }

        n = exchange(pkt, &m, build(&m, DHCP_REQUEST, xid, mac, yi, sid), &r, xid);
        if (n < 0) { logmsg("no DHCPACK, retrying"); sleep(3); continue; }
        type = parse(&r, n, &yi, &sid, &mask, &router, dns, &ndns, &lease);
        if (type == DHCP_NAK) { logmsg("got DHCPNAK, retrying"); sleep(3); continue; }
        if (type != DHCP_ACK || !yi) { logmsg("bad ACK, retrying"); sleep(3); continue; }

        if (!mask) mask = htonl(0xffffff00);        /* sane default /24 */
        apply_lease(yi, mask, router, dns, ndns);

        unsigned renew = lease > 20 ? lease / 2 : 30;   /* renew at ~T1 */
        sleep(renew);
    }
    return 0;
}
