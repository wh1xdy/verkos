/* Run twice, in two address spaces, from one binary.
 *
 * Both instances have their code and their data at exactly the same virtual
 * addresses. `mine` below therefore lives at one address and holds two
 * different values at once — one per process — which is only possible if the
 * two are genuinely separate. If the address spaces were shared, the second
 * instance would overwrite the first's copy and the check at the end would
 * catch it.
 *
 * The spinning is deliberate: it gives the timer time to take the CPU away
 * mid-run, so the two really do interleave rather than run one after the other.
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
static long getpid(void) { return syscall3(172, 0, 0, 0); }
static void exit_(int c) { syscall3(93, c, 0, 0); __builtin_unreachable(); }
static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void say(const char *s) { write_(s, slen(s)); }
static void say_num(long v) {
    char b[24]; int n = 0;
    if (v == 0) b[n++] = '0';
    while (v > 0) { b[n++] = (char)('0' + v % 10); v /= 10; }
    char o[24]; int i = 0; while (n--) o[i++] = b[n];
    write_(o, (unsigned long)i);
}

/* At the same address in both processes. */
static volatile long mine;

void _start(void) {
    mine = getpid();
    for (int round = 0; round < 3; round++) {
        for (volatile long n = 0; n < 15000000; n++) { }
        say("  [prog "); say_num(mine); say("] round "); say_num(round + 1);
        say(", my variable still says "); say_num(mine); say("\n");
    }
    if (mine != getpid()) {
        say("  [prog] MY MEMORY WAS OVERWRITTEN BY THE OTHER PROCESS\n");
        exit_(1);
    }
    exit_(0);
}
