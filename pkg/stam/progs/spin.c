/* spin — counts forever, and says so often enough to be seen.
 *
 * It exists to be interrupted. Every other program here finishes on its own,
 * so none of them can show whether Ctrl-C works: a shell that regains its
 * prompt after a program exits proves nothing about a program that does not.
 *
 * It also never calls into the kernel except to write, which is the case that
 * matters — a process that only ever blocks would be interruptible for the
 * wrong reason.
 */
static long syscall3(long n, long a, long b, long c) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}
static long write_(const char *s, unsigned long n) { return syscall3(64, 1, (long)s, (long)n); }
static void exit_(int c) { syscall3(93, c, 0, 0); __builtin_unreachable(); }
static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void say(const char *s) { write_(s, slen(s)); }

void _start(void) {
    say("  [spin] counting; interrupt me\n");
    for (unsigned long round = 0; ; round++) {
        for (volatile unsigned long i = 0; i < 20000000UL; i++) { }
        say("  [spin] still here\n");
        if (round > 600) break;      /* so a test can never hang for ever */
    }
    say("  [spin] nobody stopped me\n");
    exit_(0);
}
