/* vping — VerkOS' own ping (ICMP echo).
 *
 * Part of the VerkOS network toolbox (vdhcp/vip/vdig/...). Sends ICMP echo
 * requests over a raw socket and reports round-trip times and loss, the way
 * ping has since forever.
 *
 *   vping [-c COUNT] [-i INTERVAL] [-W TIMEOUT] [-s SIZE] [-q] HOST
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>

static volatile sig_atomic_t stop;
static void on_int(int s) { (void)s; stop = 1; }

static uint16_t cksum(const void *data, int len) {
    const uint16_t *p = data; uint32_t s = 0;
    while (len > 1) { s += *p++; len -= 2; }
    if (len) s += *(const uint8_t *)p;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}
static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

/* value for a short option, attached (-c1) or separate (-c 1) */
static const char *optval(int argc, char **argv, int *i) {
    if (argv[*i][2]) return argv[*i] + 2;
    if (*i + 1 < argc) return argv[++*i];
    return "";
}

int main(int argc, char **argv) {
    long count = -1; double interval = 1.0; double wtimeout = 1.0; int psize = 56, quiet = 0;
    const char *host = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "-c", 2)) count = atol(optval(argc, argv, &i));
        else if (!strncmp(argv[i], "-i", 2)) interval = atof(optval(argc, argv, &i));
        else if (!strncmp(argv[i], "-W", 2)) wtimeout = atof(optval(argc, argv, &i));
        else if (!strncmp(argv[i], "-s", 2)) psize = atoi(optval(argc, argv, &i));
        else if (!strcmp(argv[i], "-q")) quiet = 1;
        else if (argv[i][0] != '-') host = argv[i];
    }
    if (!host) { fprintf(stderr, "usage: vping [-c N] [-i sec] [-W sec] [-s size] [-q] HOST\n"); return 1; }
    if (psize < 0) psize = 0;
    if (psize > 1400) psize = 1400;

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_RAW; hints.ai_protocol = IPPROTO_ICMP;
    if (getaddrinfo(host, NULL, &hints, &res) != 0) { fprintf(stderr, "vping: cannot resolve %s\n", host); return 1; }
    struct sockaddr_in dst = *(struct sockaddr_in *)res->ai_addr;
    freeaddrinfo(res);
    char dstip[16]; inet_ntop(AF_INET, &dst.sin_addr, dstip, sizeof dstip);

    int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s < 0) { fprintf(stderr, "vping: raw socket: %s (need root)\n", strerror(errno)); return 1; }

    signal(SIGINT, on_int);
    int id = getpid() & 0xffff;
    int total = psize + 8;                      /* ICMP header + payload */
    printf("VPING %s (%s): %d data bytes\n", host, dstip, psize);

    long sent = 0, recvd = 0; double rtt_min = 1e18, rtt_max = 0, rtt_sum = 0, rtt_sq = 0;
    for (long seq = 0; !stop && (count < 0 || seq < count); seq++) {
        uint8_t pkt[1500]; memset(pkt, 0, sizeof pkt);
        struct icmphdr *ic = (struct icmphdr *)pkt;
        ic->type = ICMP_ECHO; ic->un.echo.id = htons(id); ic->un.echo.sequence = htons((uint16_t)seq);
        double t0 = now_ms();
        memcpy(pkt + 8, &t0, sizeof t0);
        for (int k = 8 + (int)sizeof t0; k < total; k++) pkt[k] = (uint8_t)k;
        ic->checksum = 0; ic->checksum = cksum(pkt, total);
        if (sendto(s, pkt, total, 0, (struct sockaddr *)&dst, sizeof dst) < 0) { perror("vping: send"); }
        sent++;

        double deadline = now_ms() + wtimeout * 1000.0;
        int got = 0;
        while (!got && !stop) {
            double left = deadline - now_ms();
            if (left <= 0) break;
            fd_set rf; FD_ZERO(&rf); FD_SET(s, &rf);
            struct timeval tv = { (long)(left / 1000), (long)((long)left % 1000) * 1000 };
            if (select(s + 1, &rf, NULL, NULL, &tv) <= 0) break;
            uint8_t buf[1500]; struct sockaddr_in from; socklen_t fl = sizeof from;
            int n = recvfrom(s, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
            if (n <= 0) continue;
            struct iphdr *ip = (struct iphdr *)buf; int ihl = ip->ihl * 4;
            if (n < ihl + 8) continue;
            struct icmphdr *ric = (struct icmphdr *)(buf + ihl);
            if (ric->type != ICMP_ECHOREPLY || ntohs(ric->un.echo.id) != id) continue;
            double t1; memcpy(&t1, buf + ihl + 8, sizeof t1);
            double rtt = now_ms() - t1;
            recvd++; got = 1;
            if (rtt < rtt_min) rtt_min = rtt;
            if (rtt > rtt_max) rtt_max = rtt;
            rtt_sum += rtt; rtt_sq += rtt * rtt;
            if (!quiet)
                printf("%d bytes from %s: icmp_seq=%u ttl=%d time=%.2f ms\n",
                       n - ihl, dstip, ntohs(ric->un.echo.sequence), ip->ttl, rtt);
        }
        if (!got && !quiet && !stop) printf("Request timeout for icmp_seq=%ld\n", seq);
        if (!stop && (count < 0 || seq + 1 < count)) usleep((useconds_t)(interval * 1e6));
    }

    printf("\n--- %s ping statistics ---\n", host);
    printf("%ld packets transmitted, %ld received, %.0f%% packet loss\n",
           sent, recvd, sent ? (sent - recvd) * 100.0 / sent : 0.0);
    if (recvd) {
        double avg = rtt_sum / recvd, mdev = 0;
        double var = rtt_sq / recvd - avg * avg; if (var > 0) { double x = var; for (int k = 0; k < 20; k++) x = (x + var / x) / 2; mdev = x; }
        printf("rtt min/avg/max/mdev = %.2f/%.2f/%.2f/%.2f ms\n", rtt_min, avg, rtt_max, mdev);
    }
    return recvd ? 0 : 1;
}
