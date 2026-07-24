/* vdig — VerkOS' own DNS lookup tool (a dig/host/nslookup in one binary).
 *
 * Rounds out our network toolbox (vdhcp + vip + vdig): resolves names by
 * building DNS query packets and parsing the responses itself over UDP, rather
 * than leaning on the libc resolver — one more piece of the stack that's ours.
 *
 *   vdig NAME [TYPE]        look up NAME (default type A)
 *   vdig -x IP             reverse lookup (PTR) for an IPv4 address
 *   vdig @SERVER NAME [T]  query a specific server (else the first resolv.conf ns)
 *
 * Types: A AAAA CNAME MX NS TXT SOA PTR (names or numbers).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define T_A 1
#define T_NS 2
#define T_CNAME 5
#define T_SOA 6
#define T_PTR 12
#define T_MX 15
#define T_TXT 16
#define T_AAAA 28

static const struct { const char *name; int type; } TYPES[] = {
    {"A",T_A},{"NS",T_NS},{"CNAME",T_CNAME},{"SOA",T_SOA},
    {"PTR",T_PTR},{"MX",T_MX},{"TXT",T_TXT},{"AAAA",T_AAAA},{NULL,0}
};
static const char *type_name(int t) {
    for (int i = 0; TYPES[i].name; i++) if (TYPES[i].type == t) return TYPES[i].name;
    static char b[8]; sprintf(b, "TYPE%d", t); return b;
}
static int type_num(const char *s) {
    for (int i = 0; TYPES[i].name; i++) if (!strcasecmp(TYPES[i].name, s)) return TYPES[i].type;
    return atoi(s);
}

/* first nameserver in /etc/resolv.conf, or 127.0.0.1 */
static void default_server(char *out, size_t n) {
    snprintf(out, n, "127.0.0.1");
    FILE *f = fopen("/etc/resolv.conf", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char ns[128];
        if (sscanf(line, " nameserver %127s", ns) == 1) { snprintf(out, n, "%s", ns); break; }
    }
    fclose(f);
}

/* encode a domain name into buf as DNS labels; return bytes written */
static int encode_name(const char *name, uint8_t *buf) {
    uint8_t *p = buf; const char *s = name;
    while (*s) {
        const char *dot = strchr(s, '.');
        int len = dot ? (int)(dot - s) : (int)strlen(s);
        if (len > 63) len = 63;
        *p++ = (uint8_t)len;
        memcpy(p, s, len); p += len;
        if (!dot) break;
        s = dot + 1;
    }
    *p++ = 0;
    return (int)(p - buf);
}

/* decode a (possibly compressed) name at offset `off`; write dotted form to
 * out; return the offset just past the name in the packet (for sequential
 * parsing) or -1 on error. */
static int decode_name(const uint8_t *msg, int msglen, int off, char *out, size_t outsz) {
    size_t w = 0; int jumped = 0, ret = -1, guard = 0;
    while (off >= 0 && off < msglen) {
        uint8_t c = msg[off];
        if ((c & 0xc0) == 0xc0) {
            if (off + 1 >= msglen) return -1;
            if (!jumped) ret = off + 2;
            off = ((c & 0x3f) << 8) | msg[off + 1];
            if (++guard > 128) return -1;
            jumped = 1; continue;
        }
        if (c == 0) { if (!jumped) ret = off + 1; break; }
        off++;
        if (off + c > msglen) return -1;
        if (w && w < outsz - 1) out[w++] = '.';
        for (int i = 0; i < c && w < outsz - 1; i++) out[w++] = msg[off++];
    }
    out[w] = 0;
    return ret;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }

/* print one RR's RDATA according to its type */
static void print_rdata(const uint8_t *msg, int msglen, int type, int rdoff, int rdlen) {
    char nm[256];
    switch (type) {
        case T_A: if (rdlen == 4) { char b[16]; const uint8_t *d = msg + rdoff;
                  sprintf(b, "%u.%u.%u.%u", d[0], d[1], d[2], d[3]); printf("%s", b); } break;
        case T_AAAA: if (rdlen == 16) { char b[64]; inet_ntop(AF_INET6, msg + rdoff, b, sizeof b); printf("%s", b); } break;
        case T_CNAME: case T_NS: case T_PTR:
                  decode_name(msg, msglen, rdoff, nm, sizeof nm); printf("%s", nm); break;
        case T_MX: { int pref = rd16(msg + rdoff);
                  decode_name(msg, msglen, rdoff + 2, nm, sizeof nm); printf("%d %s", pref, nm); } break;
        case T_TXT: { int i = rdoff, end = rdoff + rdlen; printf("\"");
                  while (i < end) { int l = msg[i++]; for (int j = 0; j < l && i < end; j++) putchar(msg[i++]); }
                  printf("\""); } break;
        case T_SOA: { int o = rdoff; char m2[256];
                  o = decode_name(msg, msglen, o, nm, sizeof nm);
                  o = decode_name(msg, msglen, o, m2, sizeof m2);
                  if (o > 0 && o + 20 <= msglen)
                      printf("%s %s %u %u %u %u %u", nm, m2, rd32(msg+o), rd32(msg+o+4),
                             rd32(msg+o+8), rd32(msg+o+12), rd32(msg+o+16));
                  } break;
        default: printf("(%d bytes)", rdlen); break;
    }
}

int main(int argc, char **argv) {
    char server[128] = ""; const char *name = NULL; int qtype = T_A;
    char rev[128];

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '@') snprintf(server, sizeof server, "%s", argv[i] + 1);
        else if (!strcmp(argv[i], "-x") && i + 1 < argc) {
            struct in_addr in;
            if (!inet_aton(argv[++i], &in)) { fprintf(stderr, "vdig: bad address\n"); return 1; }
            const uint8_t *b = (const uint8_t *)&in.s_addr;
            snprintf(rev, sizeof rev, "%u.%u.%u.%u.in-addr.arpa", b[3], b[2], b[1], b[0]);
            name = rev; qtype = T_PTR;
        }
        else if (!name) name = argv[i];
        else qtype = type_num(argv[i]);
    }
    if (!name) { fprintf(stderr, "usage: vdig [@server] NAME [TYPE] | vdig -x IP\n"); return 1; }
    if (!server[0]) default_server(server, sizeof server);

    /* build query */
    uint8_t q[512]; memset(q, 0, sizeof q);
    srand((unsigned)(time(NULL) ^ getpid()));
    uint16_t id = (uint16_t)rand();
    q[0] = id >> 8; q[1] = id & 0xff;
    q[2] = 0x01;                     /* RD (recursion desired) */
    q[5] = 1;                        /* QDCOUNT = 1 */
    int qn = encode_name(name, q + 12);
    int qlen = 12 + qn;
    q[qlen++] = qtype >> 8; q[qlen++] = qtype & 0xff;   /* QTYPE */
    q[qlen++] = 0; q[qlen++] = 1;                        /* QCLASS = IN */

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("vdig: socket"); return 1; }
    struct sockaddr_in to; memset(&to, 0, sizeof to);
    to.sin_family = AF_INET; to.sin_port = htons(53);
    if (!inet_aton(server, &to.sin_addr)) { fprintf(stderr, "vdig: bad server %s\n", server); return 1; }

    uint8_t r[2048]; int n = -1;
    for (int try = 0; try < 3 && n < 0; try++) {
        if (sendto(s, q, qlen, 0, (struct sockaddr *)&to, sizeof to) < 0) { perror("vdig: send"); return 1; }
        fd_set rf; FD_ZERO(&rf); FD_SET(s, &rf);
        struct timeval tv = { 3, 0 };
        if (select(s + 1, &rf, NULL, NULL, &tv) > 0) n = recv(s, r, sizeof r, 0);
    }
    if (n < 12) { fprintf(stderr, "vdig: no response from %s\n", server); return 1; }

    int rcode = r[3] & 0x0f;
    int ancount = rd16(r + 6);
    const char *rc[] = {"NOERROR","FORMERR","SERVFAIL","NXDOMAIN","NOTIMP","REFUSED"};
    printf(";; server %s   status: %s   answers: %d\n", server,
           rcode < 6 ? rc[rcode] : "?", ancount);
    printf(";; QUESTION: %s %s\n", name, type_name(qtype));

    /* skip the question section */
    int off = 12;
    int qd = rd16(r + 4);
    char nm[256];
    for (int i = 0; i < qd; i++) {
        off = decode_name(r, n, off, nm, sizeof nm);
        if (off < 0 || off + 4 > n) { fprintf(stderr, "vdig: malformed question\n"); return 1; }
        off += 4;
    }
    /* answers */
    if (ancount) printf(";; ANSWER:\n");
    for (int i = 0; i < ancount; i++) {
        off = decode_name(r, n, off, nm, sizeof nm);
        if (off < 0 || off + 10 > n) break;
        int type = rd16(r + off); uint32_t ttl = rd32(r + off + 4);
        int rdlen = rd16(r + off + 8); off += 10;
        if (off + rdlen > n) break;
        printf("%-24s %-6u %-6s ", nm, ttl, type_name(type));
        print_rdata(r, n, type, off, rdlen);
        printf("\n");
        off += rdlen;
    }
    return rcode == 0 ? 0 : 1;
}
