/* sigtest — raises a signal at a process and checks it died of it.
 *
 * The console path is the one a person uses, and it cannot be driven from a
 * script: Ctrl-C has to arrive at a terminal, and a script is read from a
 * file. So this exercises the mechanism underneath — raise, deliver, default
 * action, exit status — where a test can watch it, and the typing is left to
 * a person.
 *
 * It checks the status rather than the absence of output, because a child that
 * was never started and a child that was killed both stop printing.
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
static long fork_(void)            { return syscall3(220, 0, 0, 0); }
static long wait_(int *st)         { return syscall3(260, -1, (long)st, 0); }
static long kill_(int pid, int s)  { return syscall3(129, pid, s, 0); }
static long setpgid_(int p, int g) { return syscall3(154, p, g, 0); }
static void exit_(int c) { syscall3(93, c, 0, 0); __builtin_unreachable(); }

static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void say(const char *s) { write_(s, slen(s)); }
static void say_num(long v) {
    char b[24]; int n = 0;
    if (v <= 0) b[n++] = v == 0 ? '0' : '?';
    while (v > 0) { b[n++] = (char)('0' + v % 10); v /= 10; }
    char o[24]; int i = 0; while (n--) o[i++] = b[n];
    write_(o, (unsigned long)i);
}

#define SIGINT 2

void _start(void) {
    int bad = 0;

    /* A child that will not stop on its own. If signals did nothing, the wait
     * below would never return and the test would hang rather than fail —
     * which is why the child gives up eventually and says so. */
    long child = fork_();
    if (child == 0) {
        setpgid_(0, 0);
        for (unsigned long r = 0; r < 400; r++)
            for (volatile unsigned long i = 0; i < 20000000UL; i++) { }
        say("  [sigtest] the child was never interrupted\n");
        exit_(1);
    }
    setpgid_((int)child, (int)child);

    /* Let it get going, so this is delivery to a running process and not a
     * signal raised at something that has not started. */
    for (volatile unsigned long i = 0; i < 40000000UL; i++) { }

    if (kill_(-(int)child, SIGINT) != 0) { say("  [sigtest] kill was refused\n"); bad = 1; }

    int st = 0;
    long got = wait_(&st);
    int code = (st >> 8) & 0xff;
    say("  [sigtest] child "); say_num(got);
    say(" exited with "); say_num(code); say("\n");

    /* 128 + the signal, which is what a shell reports and what tells the two
     * apart: a program that chose to exit 130 and one that was killed by
     * SIGINT are different events, and only the second is what was asked for
     * here — but with no other way to exit 130 in this program, the status is
     * the evidence. */
    if (code != 128 + SIGINT) { say("  [sigtest] wrong status\n"); bad = 1; }

    /* And a signal to nobody must say so rather than quietly succeed. */
    if (kill_(9999, SIGINT) == 0) { say("  [sigtest] kill to nobody succeeded\n"); bad = 1; }

    say(bad ? "  [sigtest] FAILED\n" : "  [sigtest] a signal reached a running process and ended it\n");
    exit_(bad);
}
