/* The first program, and the shape every Unix has used since: fork, let the
 * child become something else, and wait for it.
 *
 * The parent now blocks in wait4 rather than spinning. The difference is
 * visible from the outside: a spinning parent burns every time slice it is
 * given, a blocked one is not scheduled at all until its child is gone.
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
static long fork_(void)  { return syscall3(220, 0, 0, 0); }
static long exec_(const char *p) { return syscall3(221, (long)p, 0, 0); }
static long wait_(int *status)    { return syscall3(260, -1, (long)status, 0); }
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

void _start(void) {
    say("  [init] I am process "); say_num(getpid()); say(", started from the initrd\n");

    long child = fork_();
    if (child == 0) {
        say("  [init] the child asks to become /hello\n");
        exec_("/hello");
        /* exec only returns when it failed — otherwise this is unreachable. */
        say("  [init] exec did not work\n");
        exit_(1);
    }

    say("  [init] forked "); say_num(child); say(", now waiting for it\n");

    int status = 0;
    long done = wait_(&status);
    say("  [init] process "); say_num(done);
    say(" finished with status "); say_num((status >> 8) & 0xff); say("\n");

    /* A second wait must say there is nobody left rather than block forever. */
    long none = wait_(&status);
    if (none < 0) say("  [init] no children left, and wait said so\n");
    else          say("  [init] wait invented a child\n");

    /* Then a program whose whole purpose is to lie to the kernel about where
     * its memory is. Run on every boot, because the check it exercises is one
     * whose absence looks exactly like its presence until something abuses it. */
    long bad = fork_();
    if (bad == 0) { exec_("/badptr"); say("  [init] badptr did not run\n"); exit_(1); }
    status = 0;
    wait_(&status);
    if (((status >> 8) & 0xff) == 0)
        say("  [init] the kernel refused every address that was not the caller's\n");
    else
        say("  [init] THE KERNEL ACCEPTED AN ADDRESS THAT WAS NOT THE CALLER'S\n");

    say("  [init] still process "); say_num(getpid()); say(", exiting\n");
    exit_(0);
}
