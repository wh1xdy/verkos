/* tcpget — an ordinary program that opens a connection to another machine.
 *
 * Connects to 10.0.2.2 on a port, writes a line, reads until the other end
 * closes, and prints what came back. `write` and `read` and nothing else: once
 * connect has returned, a stream is a stream, and this program contains no
 * line that knows it is talking to a network.
 *
 * 10.0.2.2 is the hypervisor's stand-in for the machine stam is running on, so
 * a server started there is reachable from inside. Which means the other end
 * of this conversation is a real program on a real TCP stack that has never
 * heard of us — the only way to find out whether what we send is actually TCP,
 * rather than something two halves of one codebase agree about.
 */
#include "syscalls.h"

#define PORT 15570

static long syscall6(long n, long a, long b, long c, long d, long e, long f) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    __asm__ volatile("svc #0" : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "memory");
    return x0;
}

VERK_START(main_) {
    (void)sp;

    int fd = (int)syscall3(198 /* socket */, 2 /* AF_INET */, 1 /* SOCK_STREAM */, 0);
    if (fd < 0) { say("tcpget: no socket ("); num(fd); say(")\n"); exit_(1); }

    unsigned char addr[16];
    for (int i = 0; i < 16; i++) addr[i] = 0;
    addr[0] = 2; addr[1] = 0;                  /* AF_INET             */
    addr[2] = (PORT >> 8) & 0xff;              /* port, network order */
    addr[3] = PORT & 0xff;
    addr[4] = 10; addr[5] = 0; addr[6] = 2; addr[7] = 2;   /* 10.0.2.2 */

    long e = syscall3(203 /* connect */, fd, (long)addr, 16);
    if (e < 0) { say("tcpget: cannot connect ("); num(e); say(")\n"); exit_(1); }
    say("tcpget connected\n");

    const char *req = "GET /verkos HTTP/1.0\r\n\r\n";
    long w = syscall3(64 /* write */, fd, (long)req, (long)slen(req));
    if (w < 0) { say("tcpget: cannot write ("); num(w); say(")\n"); exit_(1); }

    /* Until the other end closes, which is what a 0 from read means. A loop
     * that stopped at the first read would pass on a server that answered in
     * one segment and fail on one that did not — the difference being the
     * network's, not the program's. */
    char buf[512];
    long total = 0;
    for (;;) {
        long n = syscall3(63 /* read */, fd, (long)buf, sizeof buf);
        if (n <= 0) break;
        total += n;
        write_(buf, (unsigned long)n);
    }
    (void)syscall6(57 /* close */, fd, 0, 0, 0, 0, 0);

    say("\ntcpget read ");
    num(total);
    say(" bytes\n");
    exit_(total > 0 ? 0 : 1);
}
