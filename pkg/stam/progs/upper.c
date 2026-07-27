/* upper — reads standard input, writes it back in capitals.
 *
 * It exists to be the right-hand side of a pipe. Nothing about it knows what
 * fd 0 is attached to: a terminal when it is run alone, the other end of a
 * pipe when it is run after `|`, and the program is identical in both cases.
 * That is the whole claim being tested — a descriptor names an object, and the
 * object is the process's business rather than the program's.
 *
 * It stops when read returns 0, which only happens when every writer has
 * closed. A pipe that never reports end of file is a shell that hangs after
 * every command, so this program's ordinary exit is itself the evidence.
 */
static long syscall3(long n, long a, long b, long c) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}
static long read_(char *b, unsigned long n)        { return syscall3(63, 0, (long)b, (long)n); }
static long write_(const char *s, unsigned long n) { return syscall3(64, 1, (long)s, (long)n); }
static void exit_(int c) { syscall3(93, c, 0, 0); __builtin_unreachable(); }

void _start(void) {
    char buf[64];
    long n;
    while ((n = read_(buf, sizeof buf)) > 0) {
        for (long i = 0; i < n; i++)
            if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] = (char)(buf[i] - 32);
        write_(buf, (unsigned long)n);
    }
    /* n == 0 is end of file and the only ordinary way out. A negative n is an
     * error worth reporting as a status rather than swallowing. */
    exit_(n < 0 ? 1 : 0);
}
