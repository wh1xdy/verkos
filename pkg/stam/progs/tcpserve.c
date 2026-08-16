/* tcpserve — an ordinary program that answers a connection.
 *
 * The other way round from `tcpget`. There, stam called out and something
 * answered; here something calls *in*, and the kernel has to do the half of
 * TCP that is harder: hold a port open, answer a SYN it did not ask for, build
 * a connection out of it, and hand that connection to a program as an ordinary
 * descriptor.
 *
 * socket, bind, listen, accept, read, write, close — Linux's numbers, and
 * after accept returns there is nothing in this program that knows it is a
 * network. It reads a request and writes an answer, exactly as it would to a
 * pipe.
 */
#include "syscalls.h"

#define PORT 5580

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
    if (fd < 0) { say("tcpserve: no socket ("); num(fd); say(")\n"); exit_(1); }

    unsigned char addr[16];
    for (int i = 0; i < 16; i++) addr[i] = 0;
    addr[0] = 2; addr[1] = 0;                  /* AF_INET             */
    addr[2] = (PORT >> 8) & 0xff;              /* port, network order */
    addr[3] = PORT & 0xff;
    /* address left 0.0.0.0: whatever this machine's address turns out to be */

    long e = syscall3(200 /* bind */, fd, (long)addr, 16);
    if (e < 0) { say("tcpserve: cannot bind ("); num(e); say(")\n"); exit_(1); }
    e = syscall3(201 /* listen */, fd, 4, 0);
    if (e < 0) { say("tcpserve: cannot listen ("); num(e); say(")\n"); exit_(1); }

    /* Printed before the wait, and the harness outside waits for this line
     * rather than for a length of time. A test that sleeps and hopes is the
     * one that fails every third run for reasons nobody can reproduce. */
    say("tcpserve listening on port 5580\n");

    unsigned char peer[16];
    unsigned int peerlen = 16;
    long c = syscall3(202 /* accept */, fd, (long)peer, (long)&peerlen);
    if (c < 0) { say("tcpserve: nobody connected ("); num(c); say(")\n"); exit_(1); }
    /* The address the kernel filled in, printed as four numbers so that a
     * kernel which accepted a connection and invented the peer is visible. */
    say("tcpserve accepted from ");
    num(peer[4]); say("."); num(peer[5]); say(".");
    num(peer[6]); say("."); num(peer[7]);
    say("\n");

    char buf[512];
    long n = syscall3(63 /* read */, (long)c, (long)buf, sizeof buf);
    if (n <= 0) { say("tcpserve: nothing arrived ("); num(n); say(")\n"); exit_(1); }

    /* The answer carries the request back inside it, so the far end can check
     * that what arrived here is what it sent — the claim is not "a connection
     * happened" but "these bytes crossed it". */
    const char *head = "verkos-tcp-echo:";
    (void)syscall3(64 /* write */, (long)c, (long)head, (long)slen(head));
    (void)syscall3(64 /* write */, (long)c, (long)buf, (unsigned long)n);
    (void)syscall6(57 /* close */, (long)c, 0, 0, 0, 0, 0);
    (void)syscall6(57 /* close */, fd, 0, 0, 0, 0, 0);

    say("tcpserve answered ");
    num(n);
    say(" bytes\n");
    exit_(0);
}
