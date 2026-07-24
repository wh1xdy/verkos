/* vdhcp — VerkOS' own DHCPv4 client.
 *
 * Replaces dhcpcd: leases IPv4 addresses, applies them via ioctls, and keeps
 * /etc/resolv.conf in sync. Second step of making VerkOS' networking ours
 * (systemd-networkd -> dhcpcd -> vdhcp).
 *
 * Implements the RFC 2131 client state machine (INIT / INIT-REBOOT / SELECTING
 * / REQUESTING / BOUND / RENEWING / REBINDING) with exponential-backoff
 * retransmission, ARP duplicate-address detection (DECLINE + gratuitous ARP), a
 * persisted lease for fast INIT-REBOOT, hostname (opt 12) advertised to the
 * server, and a clean DHCPRELEASE + deconfigure on SIGTERM.
 *
 * Beyond a basic lease it also:
 *   - applies extended options: classless static routes (121), MTU (26),
 *     broadcast address (28), NTP servers (42), and a domain search list (119),
 *     alongside the usual address/mask/router/DNS/domain;
 *   - reads /etc/vdhcp.conf for the interface list, advertised hostname,
 *     requested lease time (51), vendor class (60), FQDN (81), a hook script,
 *     and extra requested option codes — all overridable on the command line;
 *   - manages several interfaces from one service, forking a worker per
 *     interface and supervising them; and
 *   - runs a hook script on BOUND/RENEW/REBIND/RELEASE/EXPIRE with the lease
 *     details in the environment (dhcpcd-style extensibility).
 *
 * The INIT/SELECTING/REQUESTING handshake uses a raw AF_PACKET socket (IP+UDP
 * built by hand) because DHCP must receive a reply before an address exists;
 * renew/rebind/release use an ordinary UDP socket. Config uses AF_INET ioctls.
 *
 * Usage: vdhcp [-1] [-h HOSTNAME] [-c CONF] [INTERFACE...]
 *   -1          one-shot: exit once bound (don't stay to renew)
 *   -h NAME     hostname to advertise (default: system hostname / conf)
 *   -c CONF     config file path (default: /etc/vdhcp.conf)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <linux/if_packet.h>

#ifndef ETH_P_IP
#define ETH_P_IP  0x0800
#endif
#ifndef ETH_P_ARP
#define ETH_P_ARP 0x0806
#endif

#define DHCP_MAGIC     0x63825363u
#define BOOTREQUEST    1
#define BOOTREPLY      2
#define DHCP_DISCOVER  1
#define DHCP_OFFER     2
#define DHCP_REQUEST   3
#define DHCP_DECLINE   4
#define DHCP_ACK       5
#define DHCP_NAK       6
#define DHCP_RELEASE   7

#define LEASE_DIR   "/var/lib/vdhcp"
#define DEFAULT_CONF "/etc/vdhcp.conf"
#define MAX_IF      8

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

struct sroute { uint32_t dest, gw; uint8_t width; };

/* one parsed/held lease (addresses in network byte order) */
struct lease {
    uint32_t ip, mask, router, server_id, bcast;
    uint32_t dns[8]; int ndns;
    uint32_t ntp[8]; int nntp;
    uint32_t lease, t1, t2, mtu;
    struct sroute sroute[16]; int nsroute;
    char     search[256];       /* space-separated search domains (opt 15 + 119) */
    char     srv_hostname[128];
    time_t   acquired;
};

/* configuration from /etc/vdhcp.conf + CLI */
struct conf {
    char     hostname[128];
    uint32_t req_lease;
    char     vendor[64];
    char     fqdn[128];
    char     hook[256];
    uint8_t  extra_req[32]; int n_extra;
    char     ifaces[MAX_IF][IFNAMSIZ]; int nif;
};
static struct conf g_conf;

/* per-worker (one process per interface, so plain globals are fine) */
static const char *g_if = "eth0";
static int   g_ifindex;
static int   g_cfg;             /* AF_INET socket for interface ioctls */
static uint8_t g_mac[6];
static int   g_oneshot;
static volatile sig_atomic_t g_term;

static void on_term(int sig) { (void)sig; g_term = 1; }

static void logmsg(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "vdhcp[%s]: ", g_if); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

static const char *ip4(uint32_t a, char *buf) {   /* buf >= 16, a in net order */
    const uint8_t *b = (const uint8_t *)&a;
    sprintf(buf, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

/* ---- ones-complement checksum ------------------------------------------- */
static uint32_t sum16(const void *data, int len, uint32_t sum) {
    const uint8_t *p = data;
    while (len > 1) { uint16_t w; memcpy(&w, p, 2); sum += w; p += 2; len -= 2; }
    if (len) sum += *p;
    return sum;
}
static uint16_t fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ---- interface helpers -------------------------------------------------- */
static int get_mac(void) {
    struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", g_if);
    if (ioctl(g_cfg, SIOCGIFHWADDR, &ifr) < 0) return -1;
    memcpy(g_mac, ifr.ifr_hwaddr.sa_data, 6);
    return 0;
}
static int if_up(void) {
    struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", g_if);
    if (ioctl(g_cfg, SIOCGIFFLAGS, &ifr) < 0) return -1;
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    return ioctl(g_cfg, SIOCSIFFLAGS, &ifr);
}
static void set_mtu(uint32_t mtu) {
    if (mtu < 576 || mtu > 9000) return;
    struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", g_if);
    ifr.ifr_mtu = (int)mtu;
    if (ioctl(g_cfg, SIOCSIFMTU, &ifr) < 0) logmsg("set mtu: %s", strerror(errno));
}
static void set_bcast(uint32_t b) {
    struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", g_if);
    struct sockaddr_in *a = (struct sockaddr_in *)&ifr.ifr_broadaddr;
    a->sin_family = AF_INET; a->sin_addr.s_addr = b;
    ioctl(g_cfg, SIOCSIFBRDADDR, &ifr);
}

static int wait_readable(int fd, int ms) {
    fd_set r; FD_ZERO(&r); FD_SET(fd, &r);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    int n = select(fd + 1, &r, NULL, NULL, &tv);
    if (n < 0) return g_term ? -2 : -1;
    return n == 0 ? -1 : 0;
}
static int nap(unsigned secs) {
    while (secs && !g_term) secs = sleep(secs);
    return g_term;
}

/* ---- config file -------------------------------------------------------- */
static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}
static void load_conf(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#'); if (h) *h = 0;
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq = 0;
        char *k = trim(line), *v = trim(eq + 1);
        if (!*k || !*v) continue;
        if (!strcmp(k, "hostname"))    snprintf(g_conf.hostname, sizeof g_conf.hostname, "%s", v);
        else if (!strcmp(k, "vendor")) snprintf(g_conf.vendor, sizeof g_conf.vendor, "%s", v);
        else if (!strcmp(k, "fqdn"))   snprintf(g_conf.fqdn, sizeof g_conf.fqdn, "%s", v);
        else if (!strcmp(k, "hook"))   snprintf(g_conf.hook, sizeof g_conf.hook, "%s", v);
        else if (!strcmp(k, "lease"))  g_conf.req_lease = (uint32_t)strtoul(v, NULL, 10);
        else if (!strcmp(k, "interfaces")) {
            for (char *t = strtok(v, " ,\t"); t && g_conf.nif < MAX_IF; t = strtok(NULL, " ,\t"))
                snprintf(g_conf.ifaces[g_conf.nif++], IFNAMSIZ, "%s", t);
        } else if (!strcmp(k, "request")) {
            for (char *t = strtok(v, " ,\t"); t && g_conf.n_extra < (int)sizeof g_conf.extra_req; t = strtok(NULL, " ,\t"))
                g_conf.extra_req[g_conf.n_extra++] = (uint8_t)strtoul(t, NULL, 10);
        }
    }
    fclose(f);
}

/* ---- DHCP message build/parse ------------------------------------------- */
static uint8_t *opt(uint8_t *p, uint8_t code, uint8_t len, const void *val) {
    *p++ = code; *p++ = len; memcpy(p, val, len); return p + len;
}

static int build(struct dhcp_msg *m, int type, uint32_t xid,
                 uint32_t ciaddr, uint32_t req_ip, uint32_t server_id, int bcast) {
    memset(m, 0, sizeof *m);
    m->op = BOOTREQUEST; m->htype = 1; m->hlen = 6;
    m->xid = xid; m->flags = bcast ? htons(0x8000) : 0; m->ciaddr = ciaddr;
    memcpy(m->chaddr, g_mac, 6);
    uint8_t *p = m->options;
    uint32_t magic = htonl(DHCP_MAGIC); memcpy(p, &magic, 4); p += 4;
    uint8_t t = (uint8_t)type; p = opt(p, 53, 1, &t);
    uint8_t cid[7] = { 1, g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5] };
    p = opt(p, 61, sizeof cid, cid);
    if (req_ip)    p = opt(p, 50, 4, &req_ip);
    if (server_id) p = opt(p, 54, 4, &server_id);
    if (type == DHCP_DISCOVER || type == DHCP_REQUEST) {
        if (g_conf.hostname[0]) p = opt(p, 12, (uint8_t)strlen(g_conf.hostname), g_conf.hostname);
        if (g_conf.vendor[0])   p = opt(p, 60, (uint8_t)strlen(g_conf.vendor), g_conf.vendor);
        if (g_conf.fqdn[0]) {                       /* RFC 4702, ASCII form */
            uint8_t f[131]; f[0] = 0x00; f[1] = 0; f[2] = 0;
            size_t l = strlen(g_conf.fqdn); if (l > 127) l = 127;
            memcpy(f + 3, g_conf.fqdn, l); p = opt(p, 81, (uint8_t)(l + 3), f);
        }
        if (g_conf.req_lease) { uint32_t v = htonl(g_conf.req_lease); p = opt(p, 51, 4, &v); }
        uint8_t prl[64]; int n = 0;
        uint8_t base[] = { 1, 3, 6, 15, 26, 28, 42, 51, 58, 59, 119, 121 };
        for (unsigned i = 0; i < sizeof base; i++) prl[n++] = base[i];
        for (int i = 0; i < g_conf.n_extra && n < (int)sizeof prl; i++) prl[n++] = g_conf.extra_req[i];
        p = opt(p, 55, (uint8_t)n, prl);
    }
    *p++ = 255;
    return (int)((uint8_t *)p - (uint8_t *)m);
}

/* decode a domain search list (RFC 3397 / DNS name encoding) into `out` */
static void decode_search(const uint8_t *o, int len, char *out, size_t outsz) {
    int pos = 0; size_t w = strlen(out);
    while (pos < len) {
        int p = pos, first = 1, jumps = 0; char name[256]; size_t nl = 0;
        for (;;) {
            if (p >= len) break;
            uint8_t c = o[p];
            if (c == 0) { if (jumps == 0) pos = p + 1; break; }
            if ((c & 0xc0) == 0xc0) {               /* compression pointer */
                if (p + 1 >= len || ++jumps > 8) { break; }
                if (jumps == 1) pos = p + 2;
                p = ((c & 0x3f) << 8) | o[p + 1]; continue;
            }
            if (p + 1 + c > len) break;
            if (!first && nl < sizeof name - 1) name[nl++] = '.';
            for (int i = 0; i < c && nl < sizeof name - 1; i++) name[nl++] = o[p + 1 + i];
            first = 0; p += 1 + c;
        }
        name[nl] = 0;
        if (nl && w + nl + 1 < outsz) { if (w) out[w++] = ' '; memcpy(out + w, name, nl); w += nl; out[w] = 0; }
        if (p >= len && (pos <= 0 || o[pos - 1] != 0)) break;   /* safety */
        if (pos <= 0) break;
    }
}

static int parse(const struct dhcp_msg *m, int len, struct lease *ls) {
    if (len < (int)(offsetof(struct dhcp_msg, options) + 4)) return -1;
    uint32_t magic; memcpy(&magic, m->options, 4);
    if (ntohl(magic) != DHCP_MAGIC) return -1;
    memset(ls, 0, sizeof *ls);
    ls->ip = m->yiaddr;
    int type = 0;
    const uint8_t *p = m->options + 4, *end = (const uint8_t *)m + len;
    while (p < end && *p != 255) {
        if (*p == 0) { p++; continue; }
        uint8_t code = *p++; if (p >= end) break;
        uint8_t l = *p++; if (p + l > end) break;
        switch (code) {
            case 53: type = *p; break;
            case 54: memcpy(&ls->server_id, p, 4); break;
            case 1:  memcpy(&ls->mask, p, 4); break;
            case 3:  if (l >= 4) memcpy(&ls->router, p, 4); break;
            case 28: if (l >= 4) memcpy(&ls->bcast, p, 4); break;
            case 26: if (l >= 2) { uint16_t v; memcpy(&v, p, 2); ls->mtu = ntohs(v); } break;
            case 6:  for (int i = 0; i + 4 <= l && ls->ndns < 8; i += 4) memcpy(&ls->dns[ls->ndns++], p + i, 4); break;
            case 42: for (int i = 0; i + 4 <= l && ls->nntp < 8; i += 4) memcpy(&ls->ntp[ls->nntp++], p + i, 4); break;
            case 51: if (l >= 4) { uint32_t v; memcpy(&v, p, 4); ls->lease = ntohl(v); } break;
            case 58: if (l >= 4) { uint32_t v; memcpy(&v, p, 4); ls->t1 = ntohl(v); } break;
            case 59: if (l >= 4) { uint32_t v; memcpy(&v, p, 4); ls->t2 = ntohl(v); } break;
            case 15: if (l && !ls->search[0]) { memcpy(ls->search, p, l); ls->search[l] = 0; } break;
            case 119: decode_search(p, l, ls->search, sizeof ls->search); break;
            case 12: if (l && l < sizeof ls->srv_hostname) { memcpy(ls->srv_hostname, p, l); ls->srv_hostname[l] = 0; } break;
            case 121: case 249: {                   /* classless static routes (RFC 3442) */
                int i = 0;
                while (i < l && ls->nsroute < 16) {
                    uint8_t w = p[i++]; if (w > 32) break;
                    int ob = (w + 7) / 8; if (i + ob + 4 > l) break;
                    uint32_t dest = 0; memcpy(&dest, p + i, ob); i += ob;
                    uint32_t gw; memcpy(&gw, p + i, 4); i += 4;
                    ls->sroute[ls->nsroute].dest = dest;
                    ls->sroute[ls->nsroute].gw = gw;
                    ls->sroute[ls->nsroute].width = w; ls->nsroute++;
                }
                break;
            }
        }
        p += l;
    }
    return type;
}

/* ---- raw AF_PACKET transport (no address yet) --------------------------- */
static int raw_send(int pkt, const struct dhcp_msg *m, int dhcplen, uint32_t srcip) {
    uint8_t frame[1500];
    struct iphdr  *ip  = (struct iphdr  *)frame;
    struct udphdr *udp = (struct udphdr *)(frame + sizeof *ip);
    memcpy(frame + sizeof *ip + sizeof *udp, m, dhcplen);
    int udplen = (int)sizeof *udp + dhcplen, iplen = (int)sizeof *ip + udplen;
    memset(ip, 0, sizeof *ip);
    ip->version = 4; ip->ihl = 5; ip->tot_len = htons(iplen);
    ip->ttl = 64; ip->protocol = IPPROTO_UDP;
    ip->saddr = srcip; ip->daddr = htonl(INADDR_BROADCAST);
    ip->check = fold(sum16(ip, sizeof *ip, 0));
    memset(udp, 0, sizeof *udp);
    udp->source = htons(68); udp->dest = htons(67); udp->len = htons(udplen);
    struct __attribute__((packed)) { uint32_t s, d; uint8_t z, pr; uint16_t l; } ph =
        { ip->saddr, ip->daddr, 0, IPPROTO_UDP, htons(udplen) };
    uint32_t s = sum16(&ph, sizeof ph, 0); s = sum16(udp, udplen, s);
    udp->check = fold(s); if (udp->check == 0) udp->check = 0xffff;
    struct sockaddr_ll ll; memset(&ll, 0, sizeof ll);
    ll.sll_family = AF_PACKET; ll.sll_ifindex = g_ifindex; ll.sll_protocol = htons(ETH_P_IP);
    ll.sll_halen = 6; memset(ll.sll_addr, 0xff, 6);
    if (sendto(pkt, frame, iplen, 0, (struct sockaddr *)&ll, sizeof ll) < 0) {
        logmsg("send: %s", strerror(errno)); return -1;
    }
    return 0;
}
static int raw_recv(int pkt, struct dhcp_msg *reply, uint32_t xid, int ms) {
    for (;;) {
        int w = wait_readable(pkt, ms);
        if (w < 0) return w;
        uint8_t buf[1500];
        int n = recv(pkt, buf, sizeof buf, 0);
        if (n < (int)sizeof(struct iphdr)) continue;
        struct iphdr *ip = (struct iphdr *)buf;
        if (ip->protocol != IPPROTO_UDP) continue;
        int ihl = ip->ihl * 4;
        if (n < ihl + (int)sizeof(struct udphdr)) continue;
        struct udphdr *udp = (struct udphdr *)(buf + ihl);
        if (ntohs(udp->dest) != 68) continue;
        int dlen = n - ihl - (int)sizeof *udp;
        if (dlen < 4) continue;
        struct dhcp_msg *r = (struct dhcp_msg *)(buf + ihl + sizeof *udp);
        if (r->op != BOOTREPLY || r->xid != xid) continue;
        memcpy(reply, r, dlen > (int)sizeof *reply ? (int)sizeof *reply : dlen);
        return dlen;
    }
}

/* ---- UDP transport (bound, for renew/rebind/release) -------------------- */
static int udp_send(int udp, const struct dhcp_msg *m, int len, uint32_t dst) {
    struct sockaddr_in to; memset(&to, 0, sizeof to);
    to.sin_family = AF_INET; to.sin_port = htons(67); to.sin_addr.s_addr = dst;
    if (sendto(udp, m, len, 0, (struct sockaddr *)&to, sizeof to) < 0) {
        logmsg("udp send: %s", strerror(errno)); return -1;
    }
    return 0;
}
static int udp_recv(int udp, struct dhcp_msg *reply, uint32_t xid, int ms) {
    for (;;) {
        int w = wait_readable(udp, ms);
        if (w < 0) return w;
        uint8_t buf[1500];
        int n = recv(udp, buf, sizeof buf, 0);
        if (n < 4) continue;
        struct dhcp_msg *r = (struct dhcp_msg *)buf;
        if (r->op != BOOTREPLY || r->xid != xid) continue;
        memcpy(reply, r, n > (int)sizeof *reply ? (int)sizeof *reply : n);
        return n;
    }
}

/* ---- ARP duplicate-address detection ------------------------------------ */
struct arp_pkt {
    uint16_t htype, ptype; uint8_t hlen, plen; uint16_t oper;
    uint8_t sha[6], spa[4], tha[6], tpa[4];
} __attribute__((packed));

static void arp_build(struct arp_pkt *a, int oper, uint32_t spa, uint32_t tpa) {
    a->htype = htons(1); a->ptype = htons(ETH_P_IP); a->hlen = 6; a->plen = 4;
    a->oper = htons(oper);
    memcpy(a->sha, g_mac, 6); memcpy(a->spa, &spa, 4);
    memset(a->tha, 0, 6);     memcpy(a->tpa, &tpa, 4);
}
static int arp_open(void) {
    int s = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_ARP));
    if (s < 0) return -1;
    struct sockaddr_ll ll; memset(&ll, 0, sizeof ll);
    ll.sll_family = AF_PACKET; ll.sll_protocol = htons(ETH_P_ARP); ll.sll_ifindex = g_ifindex;
    if (bind(s, (struct sockaddr *)&ll, sizeof ll) < 0) { close(s); return -1; }
    return s;
}
static void arp_xmit(int s, const struct arp_pkt *a) {
    struct sockaddr_ll ll; memset(&ll, 0, sizeof ll);
    ll.sll_family = AF_PACKET; ll.sll_ifindex = g_ifindex; ll.sll_protocol = htons(ETH_P_ARP);
    ll.sll_halen = 6; memset(ll.sll_addr, 0xff, 6);
    sendto(s, a, sizeof *a, 0, (struct sockaddr *)&ll, sizeof ll);
}
static int arp_in_use(uint32_t ip) {
    int s = arp_open();
    if (s < 0) return 0;
    struct arp_pkt req; arp_build(&req, 1, 0, ip);
    int used = 0;
    for (int i = 0; i < 3 && !used; i++) {
        arp_xmit(s, &req);
        for (;;) {
            if (wait_readable(s, 350) < 0) break;
            struct arp_pkt r;
            if (recv(s, &r, sizeof r, 0) < (int)sizeof r) continue;
            if (ntohs(r.oper) == 2 && memcmp(r.spa, &ip, 4) == 0 &&
                memcmp(r.sha, g_mac, 6) != 0) { used = 1; break; }
        }
    }
    close(s);
    return used;
}
static void arp_announce(uint32_t ip) {
    int s = arp_open();
    if (s < 0) return;
    struct arp_pkt a; arp_build(&a, 1, ip, ip);
    arp_xmit(s, &a); usleep(150000); arp_xmit(s, &a);
    close(s);
}

/* ---- apply / deconfigure / persist -------------------------------------- */
static void set_ifaddr(uint32_t ip, uint32_t mask) {
    struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", g_if);
    struct sockaddr_in *a = (struct sockaddr_in *)&ifr.ifr_addr;
    a->sin_family = AF_INET;
    a->sin_addr.s_addr = ip;   if (ioctl(g_cfg, SIOCSIFADDR, &ifr) < 0)    logmsg("set addr: %s", strerror(errno));
    if (mask) { a->sin_addr.s_addr = mask; if (ioctl(g_cfg, SIOCSIFNETMASK, &ifr) < 0) logmsg("set mask: %s", strerror(errno)); }
}
static void add_route(uint32_t dest, uint32_t mask, uint32_t gw) {
    struct rtentry rt; memset(&rt, 0, sizeof rt);
    struct sockaddr_in *d = (struct sockaddr_in *)&rt.rt_dst;
    struct sockaddr_in *g = (struct sockaddr_in *)&rt.rt_gateway;
    struct sockaddr_in *m = (struct sockaddr_in *)&rt.rt_genmask;
    d->sin_family = g->sin_family = m->sin_family = AF_INET;
    d->sin_addr.s_addr = dest & mask; m->sin_addr.s_addr = mask; g->sin_addr.s_addr = gw;
    rt.rt_flags = RTF_UP | (gw ? RTF_GATEWAY : 0) | (mask == 0xffffffff ? RTF_HOST : 0);
    rt.rt_dev = (char *)g_if;
    if (ioctl(g_cfg, SIOCADDRT, &rt) < 0 && errno != EEXIST) logmsg("route: %s", strerror(errno));
}
static void apply_lease(const struct lease *ls) {
    set_ifaddr(ls->ip, ls->mask ? ls->mask : htonl(0xffffff00));
    if (ls->bcast) set_bcast(ls->bcast);
    if (ls->mtu)   set_mtu(ls->mtu);
    if (ls->router) add_route(0, 0, ls->router);           /* default route */
    for (int i = 0; i < ls->nsroute; i++) {                 /* classless static routes */
        uint32_t w = ls->sroute[i].width;
        uint32_t mask = w ? htonl(~((w >= 32) ? 0u : ((1u << (32 - w)) - 1))) : 0;
        add_route(ls->sroute[i].dest, mask, ls->sroute[i].gw);
    }
    FILE *f = fopen("/etc/resolv.conf", "w");
    if (f) {
        if (ls->search[0]) fprintf(f, "search %s\n", ls->search);
        char b[16];
        for (int i = 0; i < ls->ndns; i++) fprintf(f, "nameserver %s\n", ip4(ls->dns[i], b));
        fclose(f);
    }
    if (ls->srv_hostname[0]) {
        char cur[128] = "";
        gethostname(cur, sizeof cur - 1);
        if (!cur[0] || !strcmp(cur, "localhost") || !strcmp(cur, "(none)"))
            sethostname(ls->srv_hostname, strlen(ls->srv_hostname));
    }
    char b[16];
    logmsg("leased %s/%d lease %us%s%s", ip4(ls->ip, b), __builtin_popcount(ls->mask),
           ls->lease, ls->nsroute ? " +routes" : "", ls->mtu ? " +mtu" : "");
}
static void deconfigure(void) { set_ifaddr(htonl(INADDR_ANY), 0); }

static void save_lease(const struct lease *ls) {
    mkdir(LEASE_DIR, 0755);
    char path[256]; snprintf(path, sizeof path, "%s/%s.lease", LEASE_DIR, g_if);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "ip=%u\nmask=%u\nrouter=%u\nserver=%u\nlease=%u\nacquired=%ld\n",
            ls->ip, ls->mask, ls->router, ls->server_id, ls->lease, (long)ls->acquired);
    fclose(f);
}
static int load_lease(struct lease *ls) {
    char path[256]; snprintf(path, sizeof path, "%s/%s.lease", LEASE_DIR, g_if);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    memset(ls, 0, sizeof *ls);
    char line[128]; long acq = 0; unsigned v;
    while (fgets(line, sizeof line, f)) {
        if      (sscanf(line, "ip=%u", &v) == 1)     ls->ip = v;
        else if (sscanf(line, "mask=%u", &v) == 1)   ls->mask = v;
        else if (sscanf(line, "router=%u", &v) == 1) ls->router = v;
        else if (sscanf(line, "server=%u", &v) == 1) ls->server_id = v;
        else if (sscanf(line, "lease=%u", &v) == 1)  ls->lease = v;
        else if (sscanf(line, "acquired=%ld", &acq) == 1) ls->acquired = acq;
    }
    fclose(f);
    if (!ls->ip) return 0;
    if (ls->lease && ls->acquired && time(NULL) - ls->acquired >= (time_t)ls->lease) return 0;
    return 1;
}
static void forget_lease(void) {
    char path[256]; snprintf(path, sizeof path, "%s/%s.lease", LEASE_DIR, g_if);
    unlink(path);
}

/* ---- hook script -------------------------------------------------------- */
static void run_hook(const char *reason, const struct lease *ls) {
    if (!g_conf.hook[0]) return;
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        char b[16];
        setenv("reason", reason, 1);
        setenv("interface", g_if, 1);
        if (ls) {
            setenv("ip", ip4(ls->ip, b), 1);
            setenv("mask", ip4(ls->mask, b), 1);
            if (ls->router) setenv("router", ip4(ls->router, b), 1);
            if (ls->server_id) setenv("server", ip4(ls->server_id, b), 1);
            if (ls->search[0]) setenv("domain", ls->search, 1);
            char n[16]; snprintf(n, sizeof n, "%u", ls->lease); setenv("lease", n, 1);
            char dns[160] = ""; for (int i = 0; i < ls->ndns; i++) { char x[16]; if (i) strcat(dns, " "); strcat(dns, ip4(ls->dns[i], x)); }
            setenv("dns", dns, 1);
            char ntp[160] = ""; for (int i = 0; i < ls->nntp; i++) { char x[16]; if (i) strcat(ntp, " "); strcat(ntp, ip4(ls->ntp[i], x)); }
            if (ls->nntp) setenv("ntp", ntp, 1);
        }
        execl(g_conf.hook, g_conf.hook, (char *)NULL);
        _exit(127);
    }
    waitpid(pid, NULL, 0);
}

/* ---- request helpers ---------------------------------------------------- */
static int retransmit(int fd, struct dhcp_msg *out, int outlen, uint32_t dst_or_src,
                      int use_udp, int udpfd, struct dhcp_msg *reply, uint32_t xid, int total_secs) {
    int wait = 4;
    time_t deadline = time(NULL) + total_secs;
    while (!g_term) {
        if (use_udp) { if (udp_send(udpfd, out, outlen, dst_or_src) < 0) return -1; }
        else         { if (raw_send(fd, out, outlen, dst_or_src) < 0) return -1; }
        int budget = (int)(deadline - time(NULL));
        int w = wait < budget ? wait : budget;
        if (w <= 0) return -1;
        int n = use_udp ? udp_recv(udpfd, reply, xid, w * 1000)
                        : raw_recv(fd, reply, xid, w * 1000);
        if (n > 0) return n;
        if (n == -2) return -2;
        if (time(NULL) >= deadline) return -1;
        wait = wait < 64 ? wait * 2 : 64;
    }
    return -2;
}
static void send_release(int udp, const struct lease *ls) {
    if (!ls->ip || !ls->server_id) return;
    struct dhcp_msg m;
    int len = build(&m, DHCP_RELEASE, (uint32_t)time(NULL), ls->ip, 0, ls->server_id, 0);
    udp_send(udp, &m, len, ls->server_id);
    char b[16]; logmsg("released %s", ip4(ls->ip, b));
}
static void send_decline(int pkt, const struct lease *ls) {
    struct dhcp_msg m;
    int len = build(&m, DHCP_DECLINE, (uint32_t)time(NULL), 0, ls->ip, ls->server_id, 0);
    raw_send(pkt, &m, len, htonl(INADDR_ANY));
    char b[16]; logmsg("declined %s (in use), retrying", ip4(ls->ip, b));
}
static void inherit(struct lease *ls, const struct lease *cur) {
    if (!ls->router) ls->router = cur->router;
    if (!ls->mask)   ls->mask = cur->mask;
    if (!ls->ndns) { memcpy(ls->dns, cur->dns, sizeof cur->dns); ls->ndns = cur->ndns; }
}
static void set_timers(struct lease *ls) {
    if (!ls->lease) ls->lease = 3600;
    if (!ls->t1 || ls->t1 >= ls->lease) ls->t1 = ls->lease / 2;
    if (!ls->t2 || ls->t2 >= ls->lease) ls->t2 = ls->lease - ls->lease / 8;
    ls->acquired = time(NULL);
}

/* ---- per-interface state machine ---------------------------------------- */
static int run_interface(const char *ifname) {
    g_if = ifname;
    g_cfg = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_cfg < 0) { logmsg("cfg socket: %s", strerror(errno)); return 1; }
    struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", g_if);
    if (ioctl(g_cfg, SIOCGIFINDEX, &ifr) < 0) { logmsg("no such interface"); return 1; }
    g_ifindex = ifr.ifr_ifindex;
    if (if_up() < 0) logmsg("could not bring up: %s", strerror(errno));
    if (get_mac() < 0) { logmsg("no MAC"); return 1; }

    int pkt = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_IP));
    if (pkt < 0) { logmsg("packet socket: %s (need root)", strerror(errno)); return 1; }
    struct sockaddr_ll bl; memset(&bl, 0, sizeof bl);
    bl.sll_family = AF_PACKET; bl.sll_protocol = htons(ETH_P_IP); bl.sll_ifindex = g_ifindex;
    if (bind(pkt, (struct sockaddr *)&bl, sizeof bl) < 0) { logmsg("bind pkt: %s", strerror(errno)); return 1; }

    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    setsockopt(udp, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(udp, SOL_SOCKET, SO_BINDTODEVICE, g_if, strlen(g_if) + 1);
    struct sockaddr_in cli; memset(&cli, 0, sizeof cli);
    cli.sin_family = AF_INET; cli.sin_port = htons(68); cli.sin_addr.s_addr = INADDR_ANY;
    if (bind(udp, (struct sockaddr *)&cli, sizeof cli) < 0) logmsg("bind :68: %s", strerror(errno));

    srand((unsigned)(time(NULL) ^ getpid() ^ g_ifindex));

    struct lease cur; memset(&cur, 0, sizeof cur);
    int bound = 0;
    struct lease saved; int try_reboot = load_lease(&saved);

    while (!g_term) {
        struct dhcp_msg m, r;
        struct lease ls; memset(&ls, 0, sizeof ls);
        uint32_t xid = (uint32_t)rand();
        int type, n;

        if (try_reboot) {
            char b[16]; logmsg("INIT-REBOOT: requesting previous lease %s", ip4(saved.ip, b));
            int len = build(&m, DHCP_REQUEST, xid, 0, saved.ip, 0, 1);
            n = retransmit(pkt, &m, len, htonl(INADDR_ANY), 0, 0, &r, xid, 8);
            try_reboot = 0;
            if (n <= 0) { if (n == -2) break; forget_lease(); continue; }
            type = parse(&r, n, &ls);
            if (type == DHCP_NAK) { forget_lease(); continue; }
            if (type != DHCP_ACK || !ls.ip) continue;
        } else {
            int len = build(&m, DHCP_DISCOVER, xid, 0, 0, 0, 1);
            n = retransmit(pkt, &m, len, htonl(INADDR_ANY), 0, 0, &r, xid, 60);
            if (n == -2) break;
            if (n <= 0) { logmsg("no DHCPOFFER, retrying"); if (nap(3)) break; continue; }
            type = parse(&r, n, &ls);
            if (type != DHCP_OFFER || !ls.ip) { if (nap(3)) break; continue; }
            uint32_t off_ip = ls.ip, off_sid = ls.server_id;
            len = build(&m, DHCP_REQUEST, xid, 0, off_ip, off_sid, 1);
            n = retransmit(pkt, &m, len, htonl(INADDR_ANY), 0, 0, &r, xid, 30);
            if (n == -2) break;
            if (n <= 0) { logmsg("no DHCPACK, retrying"); if (nap(3)) break; continue; }
            type = parse(&r, n, &ls);
            if (type == DHCP_NAK) { logmsg("DHCPNAK, retrying"); if (nap(3)) break; continue; }
            if (type != DHCP_ACK || !ls.ip) { if (nap(3)) break; continue; }
        }

        if (arp_in_use(ls.ip)) { send_decline(pkt, &ls); if (nap(10)) break; continue; }

        set_timers(&ls);
        apply_lease(&ls);
        arp_announce(ls.ip);
        save_lease(&ls);
        cur = ls; bound = 1;
        run_hook("BOUND", &cur);
        if (g_oneshot) break;

        int released = 0;
        for (;;) {
            if (nap(cur.t1)) { released = 1; break; }
            xid = (uint32_t)rand();
            int len = build(&m, DHCP_REQUEST, xid, cur.ip, 0, 0, 0);
            n = retransmit(0, &m, len, cur.server_id, 1, udp, &r, xid, (int)(cur.t2 - cur.t1));
            if (n == -2) { released = 1; break; }
            if (n > 0 && parse(&r, n, &ls) == DHCP_ACK && ls.ip) {
                inherit(&ls, &cur); set_timers(&ls); apply_lease(&ls); save_lease(&ls);
                cur = ls; run_hook("RENEW", &cur); continue;
            }
            logmsg("renew failed, rebinding");
            xid = (uint32_t)rand();
            len = build(&m, DHCP_REQUEST, xid, cur.ip, 0, 0, 1);
            n = retransmit(0, &m, len, htonl(INADDR_BROADCAST), 1, udp, &r, xid, (int)(cur.lease - cur.t2));
            if (n == -2) { released = 1; break; }
            if (n > 0 && parse(&r, n, &ls) == DHCP_ACK && ls.ip) {
                inherit(&ls, &cur); set_timers(&ls); apply_lease(&ls); save_lease(&ls);
                cur = ls; run_hook("REBIND", &cur); continue;
            }
            logmsg("lease expired, restarting");
            run_hook("EXPIRE", &cur);
            deconfigure(); forget_lease(); bound = 0;
            break;
        }
        if (released) break;
    }

    if (g_term && bound) { send_release(udp, &cur); run_hook("RELEASE", &cur); deconfigure(); }
    return 0;
}

/* ---- supervise one worker per interface --------------------------------- */
static char (*g_list)[IFNAMSIZ];
static int  g_nlist;

static int supervise(void) {
    pid_t pids[MAX_IF];
    for (int i = 0; i < g_nlist; i++) {
        pids[i] = fork();
        if (pids[i] == 0) _exit(run_interface(g_list[i]));
    }
    while (!g_term) {
        int st; pid_t d = waitpid(-1, &st, 0);
        if (d < 0) { if (errno == EINTR) continue; break; }
        if (g_term) break;
        for (int i = 0; i < g_nlist; i++)          /* restart a dead worker, throttled */
            if (pids[i] == d) {
                if (nap(5)) break;                 /* backoff: don't busy-loop on a missing/failing NIC */
                fprintf(stderr, "vdhcp: restarting worker for %s\n", g_list[i]);
                pids[i] = fork();
                if (pids[i] == 0) _exit(run_interface(g_list[i]));
            }
    }
    for (int i = 0; i < g_nlist; i++) kill(pids[i], SIGTERM);
    for (int i = 0; i < g_nlist; i++) waitpid(pids[i], NULL, 0);
    return 0;
}

int main(int argc, char **argv) {
    const char *conf = DEFAULT_CONF;
    char cli_if[MAX_IF][IFNAMSIZ]; int cli_nif = 0;
    char cli_host[128] = "";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-1")) g_oneshot = 1;
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) conf = argv[++i];
        else if (!strcmp(argv[i], "-h") && i + 1 < argc) snprintf(cli_host, sizeof cli_host, "%s", argv[++i]);
        else if (argv[i][0] != '-' && cli_nif < MAX_IF) snprintf(cli_if[cli_nif++], IFNAMSIZ, "%s", argv[i]);
    }

    load_conf(conf);
    if (cli_host[0]) snprintf(g_conf.hostname, sizeof g_conf.hostname, "%s", cli_host);  /* CLI wins */
    if (!g_conf.hostname[0]) {
        gethostname(g_conf.hostname, sizeof g_conf.hostname - 1);
        if (!strcmp(g_conf.hostname, "(none)")) g_conf.hostname[0] = 0;
    }

    /* interface list: CLI args > config > default eth0 */
    static char list[MAX_IF][IFNAMSIZ]; int nlist = 0;
    if (cli_nif) { for (int i = 0; i < cli_nif; i++) memcpy(list[nlist++], cli_if[i], IFNAMSIZ); }
    else if (g_conf.nif) { for (int i = 0; i < g_conf.nif; i++) memcpy(list[nlist++], g_conf.ifaces[i], IFNAMSIZ); }
    else snprintf(list[nlist++], IFNAMSIZ, "eth0");

    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL);
    signal(SIGCHLD, SIG_DFL);

    if (nlist == 1) return run_interface(list[0]);
    g_list = list; g_nlist = nlist;
    return supervise();
}
