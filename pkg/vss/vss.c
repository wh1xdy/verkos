/* vss — VerkOS' own socket lister (an ss/netstat).
 *
 * Part of the VerkOS network toolbox. Reads /proc/net/{tcp,tcp6,udp,udp6} and
 * prints listening/connected sockets, optionally resolving the owning process
 * by scanning /proc/<pid>/fd for the socket inode (like ss -p).
 *
 *   vss [-t] [-u] [-l] [-a] [-p] [-4] [-6] [-n]
 *     -t/-u  TCP / UDP (default: both)     -l  listening only
 *     -a     all (default: connected+listening)  -p  show process
 *     -4/-6  IPv4 / IPv6 only               -n  numeric (always on)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <arpa/inet.h>

static int want_tcp = 0, want_udp = 0, only_listen = 0, all = 0, show_proc = 0, only4 = 0, only6 = 0;

static const char *tcp_state(int st) {
    static const char *s[] = { "", "ESTAB", "SYN-SENT", "SYN-RECV", "FIN-WAIT1", "FIN-WAIT2",
        "TIME-WAIT", "CLOSE", "CLOSE-WAIT", "LAST-ACK", "LISTEN", "CLOSING" };
    return (st >= 1 && st <= 11) ? s[st] : "?";
}

/* format a hex address:port field from /proc/net into ip:port text */
static void fmt_addr(const char *hex, int v6, char *out, size_t n) {
    char a[64]; unsigned port;
    if (sscanf(hex, "%63[^:]:%x", a, &port) != 2) { snprintf(out, n, "?"); return; }
    if (!v6) {
        unsigned long ip = strtoul(a, NULL, 16);   /* little-endian network order */
        struct in_addr in; in.s_addr = (uint32_t)ip;
        char ips[16]; inet_ntop(AF_INET, &in, ips, sizeof ips);
        snprintf(out, n, "%s:%u", ips, port);
    } else {
        uint8_t b[16];
        for (int i = 0; i < 16; i++) { char t[3] = { a[i*2], a[i*2+1], 0 }; b[i] = (uint8_t)strtoul(t, NULL, 16); }
        /* each 32-bit word is byte-swapped in /proc */
        for (int w = 0; w < 4; w++) { uint8_t *p = b + w*4; uint8_t t; t=p[0];p[0]=p[3];p[3]=t; t=p[1];p[1]=p[2];p[2]=t; }
        char ips[64]; inet_ntop(AF_INET6, b, ips, sizeof ips);
        snprintf(out, n, "[%s]:%u", ips, port);
    }
}

/* find "pid/comm" owning socket inode; out[0]=0 if not found */
static void find_proc(unsigned long inode, char *out, size_t n) {
    out[0] = 0;
    char want[64]; snprintf(want, sizeof want, "socket:[%lu]", inode);
    DIR *proc = opendir("/proc"); if (!proc) return;
    struct dirent *de;
    while ((de = readdir(proc))) {
        if (de->d_name[0] < '1' || de->d_name[0] > '9') continue;
        char fdp[300]; snprintf(fdp, sizeof fdp, "/proc/%s/fd", de->d_name);
        DIR *fd = opendir(fdp); if (!fd) continue;
        struct dirent *fe; int hit = 0;
        while ((fe = readdir(fd))) {
            char lp[600], tgt[128];
            snprintf(lp, sizeof lp, "%s/%s", fdp, fe->d_name);
            ssize_t r = readlink(lp, tgt, sizeof tgt - 1);
            if (r > 0) { tgt[r] = 0; if (!strcmp(tgt, want)) { hit = 1; break; } }
        }
        closedir(fd);
        if (hit) {
            char cp[300], comm[64] = "";
            snprintf(cp, sizeof cp, "/proc/%s/comm", de->d_name);
            FILE *cf = fopen(cp, "r");
            if (cf) { if (fgets(comm, sizeof comm, cf)) comm[strcspn(comm, "\n")] = 0; fclose(cf); }
            snprintf(out, n, "%s/%s", de->d_name, comm);
            break;
        }
    }
    closedir(proc);
}

static void dump(const char *path, const char *netid, int v6, int is_tcp) {
    FILE *f = fopen(path, "r"); if (!f) return;
    char line[512];
    if (!fgets(line, sizeof line, f)) { fclose(f); return; }   /* header */
    while (fgets(line, sizeof line, f)) {
        int sl, st; char local[128], rem[128]; unsigned long inode = 0;
        /* sl: local rem st tx:rx tr:when retrnsmt uid timeout inode ... */
        if (sscanf(line, "%d: %127s %127s %x %*s %*s %*s %*d %*d %lu",
                   &sl, local, rem, &st, &inode) < 5) continue;
        int listen = is_tcp ? (st == 10) : 1;      /* UDP: treat "unconnected" rows as listening */
        if (only_listen && !listen) continue;
        if (!only_listen && !all && is_tcp && st != 1 && st != 10) continue;
        char la[80], ra[80]; fmt_addr(local, v6, la, sizeof la); fmt_addr(rem, v6, ra, sizeof ra);
        char proc[320] = "";
        if (show_proc) find_proc(inode, proc, sizeof proc);
        printf("%-5s %-11s %-24s %-24s %s\n", netid, is_tcp ? tcp_state(st) : "-", la, ra, proc);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        for (char *c = argv[i] + (argv[i][0] == '-' ? 1 : 0); argv[i][0] == '-' && *c; c++)
            switch (*c) {
                case 't': want_tcp = 1; break; case 'u': want_udp = 1; break;
                case 'l': only_listen = 1; break; case 'a': all = 1; break;
                case 'p': show_proc = 1; break; case '4': only4 = 1; break;
                case '6': only6 = 1; break; case 'n': break;
                default: fprintf(stderr, "vss: unknown flag -%c\n", *c); return 1;
            }
    if (!want_tcp && !want_udp) { want_tcp = want_udp = 1; }

    printf("%-5s %-11s %-24s %-24s %s\n", "Netid", "State", "Local", "Peer", show_proc ? "Process" : "");
    if (want_tcp && !only6) dump("/proc/net/tcp",  "tcp", 0, 1);
    if (want_tcp && !only4) dump("/proc/net/tcp6", "tcp", 1, 1);
    if (want_udp && !only6) dump("/proc/net/udp",  "udp", 0, 0);
    if (want_udp && !only4) dump("/proc/net/udp6", "udp", 1, 0);
    return 0;
}
