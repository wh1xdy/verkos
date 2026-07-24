/* vtrace — VerkOS' own traceroute (ICMP, increasing TTL).
 *
 * Part of the VerkOS network toolbox. Sends ICMP echo probes with a rising IP
 * TTL: each intermediate router that discards a probe returns an ICMP
 * time-exceeded (revealing that hop); the destination returns an echo reply.
 *
 *   vtrace [-m MAXHOPS] [-q QUERIES] [-w WAIT] HOST
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>

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

static const char *optval(int argc, char **argv, int *i) {
    if (argv[*i][2]) return argv[*i] + 2;
    if (*i + 1 < argc) return argv[++*i];
    return "";
}

int main(int argc, char **argv) {
    int maxhops = 30, nq = 3; double wait = 2.0; const char *host = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "-m", 2)) maxhops = atoi(optval(argc, argv, &i));
        else if (!strncmp(argv[i], "-q", 2)) nq = atoi(optval(argc, argv, &i));
        else if (!strncmp(argv[i], "-w", 2)) wait = atof(optval(argc, argv, &i));
        else if (argv[i][0] != '-') host = argv[i];
    }
    if (!host) { fprintf(stderr, "usage: vtrace [-m maxhops] [-q queries] [-w wait] HOST\n"); return 1; }
    if (maxhops < 1 || maxhops > 64) maxhops = 30;

    struct addrinfo hints, *res; memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_RAW; hints.ai_protocol = IPPROTO_ICMP;
    if (getaddrinfo(host, NULL, &hints, &res) != 0) { fprintf(stderr, "vtrace: cannot resolve %s\n", host); return 1; }
    struct sockaddr_in dst = *(struct sockaddr_in *)res->ai_addr;
    freeaddrinfo(res);
    char dstip[16]; inet_ntop(AF_INET, &dst.sin_addr, dstip, sizeof dstip);

    int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s < 0) { fprintf(stderr, "vtrace: raw socket: %s (need root)\n", strerror(errno)); return 1; }
    int id = getpid() & 0xffff;
    printf("vtrace to %s (%s), %d hops max\n", host, dstip, maxhops);

    int done = 0;
    for (int ttl = 1; ttl <= maxhops && !done; ttl++) {
        setsockopt(s, IPPROTO_IP, IP_TTL, &ttl, sizeof ttl);
        printf("%2d ", ttl);
        char lasthop[16] = "";
        for (int q = 0; q < nq; q++) {
            uint16_t seq = (uint16_t)(ttl * 100 + q);
            uint8_t pkt[64]; memset(pkt, 0, sizeof pkt);
            struct icmphdr *ic = (struct icmphdr *)pkt;
            ic->type = ICMP_ECHO; ic->un.echo.id = htons(id); ic->un.echo.sequence = htons(seq);
            ic->checksum = 0; ic->checksum = cksum(pkt, sizeof pkt);
            double t0 = now_ms();
            if (sendto(s, pkt, sizeof pkt, 0, (struct sockaddr *)&dst, sizeof dst) < 0) { printf(" !send"); continue; }

            double deadline = now_ms() + wait * 1000.0; int got = 0;
            while (!got) {
                double left = deadline - now_ms(); if (left <= 0) break;
                fd_set rf; FD_ZERO(&rf); FD_SET(s, &rf);
                struct timeval tv = { (long)(left / 1000), (long)((long)left % 1000) * 1000 };
                if (select(s + 1, &rf, NULL, NULL, &tv) <= 0) break;
                uint8_t buf[1500]; struct sockaddr_in from; socklen_t fl = sizeof from;
                int n = recvfrom(s, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
                if (n <= 0) continue;
                struct iphdr *ip = (struct iphdr *)buf; int ihl = ip->ihl * 4;
                if (n < ihl + 8) continue;
                struct icmphdr *ric = (struct icmphdr *)(buf + ihl);
                int match = 0, reached = 0;
                if (ric->type == ICMP_ECHOREPLY && ntohs(ric->un.echo.id) == id && ntohs(ric->un.echo.sequence) == seq) {
                    match = 1; reached = 1;                 /* reply from the destination */
                } else if (ric->type == ICMP_TIME_EXCEEDED) {
                    /* the quoted original IP+ICMP is after our 8-byte ICMP error header */
                    struct iphdr *oip = (struct iphdr *)(buf + ihl + 8);
                    int oihl = oip->ihl * 4;
                    struct icmphdr *oic = (struct icmphdr *)(buf + ihl + 8 + oihl);
                    if (n >= ihl + 8 + oihl + 8 && ntohs(oic->un.echo.id) == id && ntohs(oic->un.echo.sequence) == seq)
                        match = 1;
                }
                if (!match) continue;
                double rtt = now_ms() - t0;
                char hop[16]; inet_ntop(AF_INET, &from.sin_addr, hop, sizeof hop);
                if (strcmp(hop, lasthop)) { printf(" %s", hop); snprintf(lasthop, sizeof lasthop, "%s", hop); }
                printf("  %.2f ms", rtt);
                got = 1; if (reached) done = 1;
            }
            if (!got) printf("  *");
        }
        printf("\n");
    }
    return 0;
}
