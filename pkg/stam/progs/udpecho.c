/* udpecho — an ordinary program that talks to another machine.
 *
 * Binds a port, waits for one datagram, sends the same bytes back to whoever
 * sent them, and says how many. No privileges, nothing the kernel knows about
 * it, and the same four system calls a Linux program would make with the same
 * numbers.
 *
 * The point is what the *other end* sees. Everything stam has printed about
 * the network so far, the kernel printed about itself. Here a process outside
 * the machine sends bytes it chose and gets them back, and the check is made
 * there — by a program that is not ours, in a language that is not ours, over
 * a socket this kernel had to serve correctly for the bytes to return at all.
 */
#include "syscalls.h"

#define PORT 5555

VERK_START(main_) {
    (void)sp;

    int fd = (int)syscall3(198 /* socket */, 2 /* AF_INET */, 2 /* SOCK_DGRAM */, 0);
    if (fd < 0) { say("udpecho: no socket ("); num(fd); say(")\n"); exit_(1); }

    /* sockaddr_in: family in the host's byte order, port and address in the
     * network's. Written out a byte at a time so that the two orders are
     * visible rather than hidden in a macro. */
    unsigned char addr[16];
    for (int i = 0; i < 16; i++) addr[i] = 0;
    addr[0] = 2; addr[1] = 0;                 /* AF_INET       */
    addr[2] = (PORT >> 8) & 0xff;             /* port, network order */
    addr[3] = PORT & 0xff;
    /* address left 0.0.0.0: whatever this machine's address turns out to be */

    long e = syscall3(200 /* bind */, fd, (long)addr, 16);
    if (e < 0) { say("udpecho: cannot bind ("); num(e); say(")\n"); exit_(1); }

    /* Printed before the wait, and the harness outside waits for this line
     * rather than for a length of time. A test that sleeps and hopes is the
     * one that fails every third run for reasons nobody can reproduce. */
    say("udpecho listening on port 5555\n");

    char buf[512];
    unsigned char from[16];
    unsigned int fromlen = 16;
    register long x8 __asm__("x8") = 207;     /* recvfrom */
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = sizeof buf;
    register long x3 __asm__("x3") = 0;
    register long x4 __asm__("x4") = (long)from;
    register long x5 __asm__("x5") = (long)&fromlen;
    __asm__ volatile("svc #0" : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "memory");
    long n = x0;
    if (n < 0) { say("udpecho: nothing arrived ("); num(n); say(")\n"); exit_(1); }

    /* Back to exactly where it came from — the address the kernel filled in,
     * not one this program knew. */
    register long y8 __asm__("x8") = 206;     /* sendto */
    register long y0 __asm__("x0") = fd;
    register long y1 __asm__("x1") = (long)buf;
    register long y2 __asm__("x2") = n;
    register long y3 __asm__("x3") = 0;
    register long y4 __asm__("x4") = (long)from;
    register long y5 __asm__("x5") = 16;
    __asm__ volatile("svc #0" : "+r"(y0)
                     : "r"(y8), "r"(y1), "r"(y2), "r"(y3), "r"(y4), "r"(y5)
                     : "memory");
    if (y0 < 0) { say("udpecho: cannot send back ("); num(y0); say(")\n"); exit_(1); }

    say("udpecho sent back ");
    num(n);
    say(" bytes\n");
    syscall3(57 /* close */, fd, 0, 0);
    exit_(0);
}
