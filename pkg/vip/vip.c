/* vip — VerkOS' own network configuration tool (an ip/ifconfig in one binary).
 *
 * Continues "make the network ours" (vdhcp, and now the inspection/config tool
 * that pairs with it), replacing iproute2's `ip` for the common operations.
 *
 *   vip [addr]                       show interfaces and their addresses
 *   vip addr add  IP[/PREFIX] dev IF add an address (sets the primary address)
 *   vip addr del  dev IF             remove the interface's IPv4 address
 *   vip link                         show interfaces (state, MAC, MTU)
 *   vip link set  IF up|down         bring an interface up/down
 *   vip link set  IF mtu N           set the MTU
 *   vip route                        show the IPv4 routing table
 *   vip route add default via GW [dev IF] [metric N]
 *   vip route add DEST/PREFIX [via GW] dev IF [metric N]
 *   vip route del ... (same selectors)
 *
 * Addresses/routes go through the classic AF_INET ioctls (SIOCSIFADDR,
 * SIOCADDRT, ...), the same ones vdhcp uses; listing reads /proc and getifaddrs.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <ifaddrs.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <net/route.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int S;   /* AF_INET socket for ioctls */

static int die(const char *what) { fprintf(stderr, "vip: %s: %s\n", what, strerror(errno)); return 1; }

static int prefix_of(uint32_t mask) { return __builtin_popcount(mask); }
static uint32_t mask_of(int prefix) {
    if (prefix <= 0) return 0;
    if (prefix >= 32) return htonl(0xffffffffu);
    return htonl(~((1u << (32 - prefix)) - 1));
}
/* parse "1.2.3.4" or "1.2.3.4/24"; *prefix defaults to `defp` if absent */
static int parse_cidr(const char *s, uint32_t *addr, int *prefix, int defp) {
    char buf[64]; snprintf(buf, sizeof buf, "%s", s);
    char *slash = strchr(buf, '/');
    *prefix = defp;
    if (slash) { *slash = 0; *prefix = atoi(slash + 1); }
    struct in_addr in;
    if (!inet_aton(buf, &in)) return -1;
    *addr = in.s_addr;
    return 0;
}
static const char *ip4(uint32_t a, char *b) {
    const uint8_t *p = (const uint8_t *)&a; sprintf(b, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]); return b;
}

/* ---- listing ------------------------------------------------------------ */
static void flags_str(short f, char *out, size_t n) {
    out[0] = 0;
    struct { short bit; const char *name; } t[] = {
        {IFF_UP,"UP"},{IFF_BROADCAST,"BROADCAST"},{IFF_LOOPBACK,"LOOPBACK"},
        {IFF_POINTOPOINT,"POINTOPOINT"},{IFF_RUNNING,"RUNNING"},
        {IFF_NOARP,"NOARP"},{IFF_MULTICAST,"MULTICAST"},
    };
    int first = 1;
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        if (f & t[i].bit) {
            size_t l = strlen(out);
            snprintf(out + l, n - l, "%s%s", first ? "" : ",", t[i].name);
            first = 0;
        }
}

static void print_iface(const char *name, int show_addr) {
    struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
    short fl = 0; int mtu = 0;
    if (ioctl(S, SIOCGIFFLAGS, &ifr) == 0) fl = ifr.ifr_flags;
    if (ioctl(S, SIOCGIFMTU, &ifr) == 0) mtu = ifr.ifr_mtu;
    unsigned idx = if_nametoindex(name);
    char fs[128]; flags_str(fl, fs, sizeof fs);
    printf("%u: %s: <%s> mtu %d\n", idx, name, fs, mtu);

    if (ioctl(S, SIOCGIFHWADDR, &ifr) == 0) {
        const unsigned char *m = (unsigned char *)ifr.ifr_hwaddr.sa_data;
        if (ifr.ifr_hwaddr.sa_family == ARPHRD_LOOPBACK)
            printf("    link/loopback\n");
        else
            printf("    link/ether %02x:%02x:%02x:%02x:%02x:%02x\n", m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    if (!show_addr) return;

    struct ifaddrs *ifa, *p;
    if (getifaddrs(&ifa) != 0) return;
    for (p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || strcmp(p->ifa_name, name)) continue;
        if (p->ifa_addr->sa_family == AF_INET) {
            uint32_t a = ((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr;
            uint32_t m = p->ifa_netmask ? ((struct sockaddr_in *)p->ifa_netmask)->sin_addr.s_addr : 0;
            char b[16]; printf("    inet %s/%d\n", ip4(a, b), prefix_of(m));
        } else if (p->ifa_addr->sa_family == AF_INET6) {
            char b[64]; struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)p->ifa_addr;
            inet_ntop(AF_INET6, &s6->sin6_addr, b, sizeof b);
            int plen = 0;
            if (p->ifa_netmask) {
                const uint8_t *mm = ((struct sockaddr_in6 *)p->ifa_netmask)->sin6_addr.s6_addr;
                for (int i = 0; i < 16; i++) plen += __builtin_popcount(mm[i]);
            }
            printf("    inet6 %s/%d\n", b, plen);
        }
    }
    freeifaddrs(ifa);
}

static int show_all(int show_addr, const char *only) {
    struct if_nameindex *idx = if_nameindex(), *p;
    if (!idx) return die("if_nameindex");
    for (p = idx; p->if_index; p++)
        if (!only || !strcmp(only, p->if_name)) print_iface(p->if_name, show_addr);
    if_freenameindex(idx);
    return 0;
}

static int show_route(void) {
    FILE *f = fopen("/proc/net/route", "r");
    if (!f) return die("/proc/net/route");
    char line[512];
    if (!fgets(line, sizeof line, f)) { fclose(f); return 0; }   /* header */
    while (fgets(line, sizeof line, f)) {
        char iface[IFNAMSIZ]; unsigned long dest, gw, flags, mask; int refcnt, use, metric;
        if (sscanf(line, "%15s %lx %lx %lx %d %d %d %lx",
                   iface, &dest, &gw, &flags, &refcnt, &use, &metric, &mask) != 8) continue;
        if (!(flags & RTF_UP)) continue;
        char db[16], gb[16];
        if (dest == 0 && mask == 0) printf("default");
        else printf("%s/%d", ip4((uint32_t)dest, db), prefix_of((uint32_t)mask));
        if (gw) printf(" via %s", ip4((uint32_t)gw, gb));
        printf(" dev %s", iface);
        if (metric) printf(" metric %d", metric);
        printf("\n");
    }
    fclose(f);
    return 0;
}

/* ---- config ------------------------------------------------------------- */
static void set_sin(struct ifreq *ifr, uint32_t a) {
    struct sockaddr_in *s = (struct sockaddr_in *)&ifr->ifr_addr;
    s->sin_family = AF_INET; s->sin_addr.s_addr = a;
}

static int addr_add(int argc, char **argv) {
    /* addr add IP[/PREFIX] dev IF */
    const char *cidr = NULL, *dev = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "dev") && i + 1 < argc) dev = argv[++i];
        else if (!cidr) cidr = argv[i];
    }
    if (!cidr || !dev) { fprintf(stderr, "usage: vip addr add IP[/PREFIX] dev IF\n"); return 1; }
    uint32_t a; int prefix;
    if (parse_cidr(cidr, &a, &prefix, 24) < 0) { fprintf(stderr, "vip: bad address %s\n", cidr); return 1; }
    struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", dev);
    set_sin(&ifr, a);          if (ioctl(S, SIOCSIFADDR, &ifr) < 0)    return die("set address");
    set_sin(&ifr, mask_of(prefix)); if (ioctl(S, SIOCSIFNETMASK, &ifr) < 0) return die("set netmask");
    uint32_t bcast = (a & mask_of(prefix)) | ~mask_of(prefix);
    set_sin(&ifr, bcast);      ioctl(S, SIOCSIFBRDADDR, &ifr);
    return 0;
}

static int addr_del(int argc, char **argv) {
    /* addr del dev IF */
    const char *dev = NULL;
    for (int i = 0; i < argc; i++) if (!strcmp(argv[i], "dev") && i + 1 < argc) dev = argv[++i];
    if (!dev) { fprintf(stderr, "usage: vip addr del dev IF\n"); return 1; }
    struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", dev);
    set_sin(&ifr, 0);
    if (ioctl(S, SIOCSIFADDR, &ifr) < 0) return die("clear address");
    return 0;
}

static int link_set(int argc, char **argv) {
    /* link set IF up|down | mtu N */
    if (argc < 2) { fprintf(stderr, "usage: vip link set IF up|down|mtu N\n"); return 1; }
    const char *dev = argv[0];
    struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", dev);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "up") || !strcmp(argv[i], "down")) {
            if (ioctl(S, SIOCGIFFLAGS, &ifr) < 0) return die("get flags");
            if (!strcmp(argv[i], "up")) ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
            else ifr.ifr_flags &= ~IFF_UP;
            if (ioctl(S, SIOCSIFFLAGS, &ifr) < 0) return die("set flags");
        } else if (!strcmp(argv[i], "mtu") && i + 1 < argc) {
            ifr.ifr_mtu = atoi(argv[++i]);
            if (ioctl(S, SIOCSIFMTU, &ifr) < 0) return die("set mtu");
        } else { fprintf(stderr, "vip: unknown link option %s\n", argv[i]); return 1; }
    }
    return 0;
}

static int route_mod(int add, int argc, char **argv) {
    /* [default|DEST/PREFIX] [via GW] [dev IF] [metric N] */
    uint32_t dest = 0, gw = 0; int prefix = 0, metric = 0; const char *dev = NULL; int have_dest = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "default")) { dest = 0; prefix = 0; have_dest = 1; }
        else if (!strcmp(argv[i], "via") && i + 1 < argc) { struct in_addr in; if (inet_aton(argv[++i], &in)) gw = in.s_addr; }
        else if (!strcmp(argv[i], "dev") && i + 1 < argc) dev = argv[++i];
        else if (!strcmp(argv[i], "metric") && i + 1 < argc) metric = atoi(argv[++i]);
        else if (!have_dest) { if (parse_cidr(argv[i], &dest, &prefix, 32) == 0) have_dest = 1; }
    }
    if (!have_dest) { fprintf(stderr, "usage: vip route %s default|DEST/PREFIX [via GW] [dev IF]\n", add ? "add" : "del"); return 1; }
    uint32_t mask = mask_of(prefix);
    struct rtentry rt; memset(&rt, 0, sizeof rt);
    struct sockaddr_in *d = (struct sockaddr_in *)&rt.rt_dst;
    struct sockaddr_in *g = (struct sockaddr_in *)&rt.rt_gateway;
    struct sockaddr_in *m = (struct sockaddr_in *)&rt.rt_genmask;
    d->sin_family = g->sin_family = m->sin_family = AF_INET;
    d->sin_addr.s_addr = dest & mask; m->sin_addr.s_addr = mask; g->sin_addr.s_addr = gw;
    rt.rt_flags = RTF_UP | (gw ? RTF_GATEWAY : 0) | (mask == 0xffffffffu ? RTF_HOST : 0);
    if (metric) rt.rt_metric = (short)(metric + 1);
    if (dev) rt.rt_dev = (char *)dev;
    if (ioctl(S, add ? SIOCADDRT : SIOCDELRT, &rt) < 0) return die(add ? "add route" : "del route");
    return 0;
}

static int usage(void) {
    fprintf(stderr,
        "usage:\n"
        "  vip [addr]                         show interfaces and addresses\n"
        "  vip addr add IP[/PREFIX] dev IF    add an address\n"
        "  vip addr del dev IF                remove the address\n"
        "  vip link                           show interfaces\n"
        "  vip link set IF up|down|mtu N      change link state\n"
        "  vip route                          show routes\n"
        "  vip route add default via GW [dev IF] [metric N]\n"
        "  vip route add DEST/PREFIX [via GW] dev IF [metric N]\n"
        "  vip route del ...                  remove a route\n");
    return 1;
}

int main(int argc, char **argv) {
    S = socket(AF_INET, SOCK_DGRAM, 0);
    if (S < 0) return die("socket");

    const char *obj = argc > 1 ? argv[1] : "addr";
    if (!strcmp(obj, "-h") || !strcmp(obj, "help") || !strcmp(obj, "--help")) return usage();

    if (!strcmp(obj, "addr") || !strcmp(obj, "a") || !strcmp(obj, "address")) {
        if (argc > 2 && !strcmp(argv[2], "add")) return addr_add(argc - 3, argv + 3);
        if (argc > 2 && !strcmp(argv[2], "del")) return addr_del(argc - 3, argv + 3);
        return show_all(1, argc > 2 ? argv[2] : NULL);
    }
    if (!strcmp(obj, "link") || !strcmp(obj, "l")) {
        if (argc > 2 && !strcmp(argv[2], "set")) return link_set(argc - 3, argv + 3);
        return show_all(0, argc > 2 ? argv[2] : NULL);
    }
    if (!strcmp(obj, "route") || !strcmp(obj, "r")) {
        if (argc > 2 && !strcmp(argv[2], "add")) return route_mod(1, argc - 3, argv + 3);
        if (argc > 2 && !strcmp(argv[2], "del")) return route_mod(0, argc - 3, argv + 3);
        return show_route();
    }
    return usage();
}
