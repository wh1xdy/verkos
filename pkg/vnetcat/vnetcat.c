/* vnetcat — VerkOS' own netcat (nc).
 *
 * The network multitool: connect to or listen on a TCP/UDP port and relay
 * stdin/stdout, or scan ports. Part of the VerkOS network toolbox; installed as
 * the familiar `nc`/`netcat`.
 *
 *   nc [-u] [-v] [-w SEC] HOST PORT          connect and relay
 *   nc -l [-u] [-v] [-p] PORT                listen and relay
 *   nc -z [-v] [-w SEC] HOST PORT[-PORT]     scan (TCP), report open ports
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int verbose;
static void vlog(const char *fmt, ...) {
    if (!verbose) return;
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap);
}

/* resolve host:port into a sockaddr_in; return 0 on success */
static int resolve(const char *host, int port, int udp, struct sockaddr_in *out) {
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET; out->sin_port = htons((uint16_t)port);
    if (host && inet_aton(host, &out->sin_addr)) return 0;
    struct addrinfo hints, *res; memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; hints.ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM;
    if (!host) { out->sin_addr.s_addr = INADDR_ANY; return 0; }
    if (getaddrinfo(host, NULL, &hints, &res) != 0) return -1;
    out->sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 0;
}

/* connect a TCP socket with an optional timeout; -1 on failure */
static int tcp_connect(struct sockaddr_in *sa, double wsec) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    if (wsec > 0) {
        fcntl(s, F_SETFL, O_NONBLOCK);
        if (connect(s, (struct sockaddr *)sa, sizeof *sa) < 0 && errno != EINPROGRESS) { close(s); return -1; }
        fd_set wf; FD_ZERO(&wf); FD_SET(s, &wf);
        struct timeval tv = { (long)wsec, (long)((wsec - (long)wsec) * 1e6) };
        if (select(s + 1, NULL, &wf, NULL, &tv) <= 0) { close(s); return -1; }
        int err = 0; socklen_t el = sizeof err;
        getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) { close(s); return -1; }
        fcntl(s, F_SETFL, 0);
    } else if (connect(s, (struct sockaddr *)sa, sizeof *sa) < 0) { close(s); return -1; }
    return s;
}

/* pump data both ways between fd and stdin/stdout until either side closes */
static void relay(int fd) {
    char buf[8192];
    for (;;) {
        fd_set rf; FD_ZERO(&rf); FD_SET(fd, &rf); FD_SET(STDIN_FILENO, &rf);
        int mx = fd > STDIN_FILENO ? fd : STDIN_FILENO;
        if (select(mx + 1, &rf, NULL, NULL, NULL) < 0) { if (errno == EINTR) continue; break; }
        if (FD_ISSET(fd, &rf)) {
            ssize_t n = read(fd, buf, sizeof buf);
            if (n <= 0) break;
            if (write(STDOUT_FILENO, buf, n) != n) break;
        }
        if (FD_ISSET(STDIN_FILENO, &rf)) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
            if (n <= 0) { shutdown(fd, SHUT_WR); continue; }
            if (write(fd, buf, n) != n) break;
        }
    }
}

int main(int argc, char **argv) {
    int listen_mode = 0, udp = 0, zscan = 0; double wsec = 0;
    const char *host = NULL; int p1 = 0, p2 = 0;
    char *pos[2]; int np = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) listen_mode = 1;
        else if (!strcmp(argv[i], "-u")) udp = 1;
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-z")) zscan = 1;
        else if (!strcmp(argv[i], "-p")) { /* accept the -p PORT form for listen */ }
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) wsec = atof(argv[++i]);
        else if (argv[i][0] != '-' && np < 2) pos[np++] = argv[i];
    }
    /* positionals: [HOST] PORT[-PORT] */
    const char *portspec = NULL;
    if (np == 2) { host = pos[0]; portspec = pos[1]; }
    else if (np == 1) { portspec = pos[0]; }
    else { fprintf(stderr, "usage: nc [-l] [-u] [-v] [-z] [-w sec] [HOST] PORT[-PORT]\n"); return 1; }
    if (sscanf(portspec, "%d-%d", &p1, &p2) < 1) { fprintf(stderr, "nc: bad port %s\n", portspec); return 1; }
    if (p2 < p1) p2 = p1;

    if (zscan) {                                    /* TCP port scan */
        int open = 0;
        for (int p = p1; p <= p2; p++) {
            struct sockaddr_in sa;
            if (resolve(host, p, 0, &sa) < 0) { fprintf(stderr, "nc: cannot resolve %s\n", host); return 1; }
            int s = tcp_connect(&sa, wsec > 0 ? wsec : 1.0);
            if (s >= 0) { printf("%s %d open\n", host ? host : "", p); close(s); open++; }
            else vlog("%s %d closed", host ? host : "", p);
        }
        return open ? 0 : 1;
    }

    struct sockaddr_in sa;
    if (resolve(host, p1, udp, &sa) < 0) { fprintf(stderr, "nc: cannot resolve %s\n", host); return 1; }

    if (listen_mode) {
        int s = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, 0);
        int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in bind_sa; memset(&bind_sa, 0, sizeof bind_sa);
        bind_sa.sin_family = AF_INET; bind_sa.sin_addr.s_addr = INADDR_ANY; bind_sa.sin_port = htons((uint16_t)p1);
        if (bind(s, (struct sockaddr *)&bind_sa, sizeof bind_sa) < 0) { perror("nc: bind"); return 1; }
        vlog("listening on %s port %d", udp ? "udp" : "tcp", p1);
        if (udp) {
            /* connect to the first peer we hear from, then relay */
            char probe[1]; struct sockaddr_in from; socklen_t fl = sizeof from;
            if (recvfrom(s, probe, 0, MSG_PEEK, (struct sockaddr *)&from, &fl) < 0 && errno != EAGAIN) { perror("nc: recv"); return 1; }
            connect(s, (struct sockaddr *)&from, sizeof from);
            relay(s);
        } else {
            listen(s, 1);
            int c = accept(s, NULL, NULL);
            if (c < 0) { perror("nc: accept"); return 1; }
            close(s); relay(c); close(c);
        }
        return 0;
    }

    if (udp) {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        connect(s, (struct sockaddr *)&sa, sizeof sa);   /* fix the peer so relay's read/write work */
        vlog("connected (udp) to %s:%d", host, p1);
        relay(s);
        return 0;
    }
    int s = tcp_connect(&sa, wsec);
    if (s < 0) { fprintf(stderr, "nc: connect to %s:%d failed: %s\n", host ? host : "", p1, strerror(errno)); return 1; }
    vlog("connected to %s:%d", host, p1);
    relay(s); close(s);
    return 0;
}
