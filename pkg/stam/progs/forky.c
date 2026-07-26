/* fork: one program becomes two.
 *
 * The proof is the variable. Both processes see it at the same address, and
 * after the fork the child sets it to 222. If fork copied the address space,
 * the parent still sees 100. If it did not, the parent sees 222 and the whole
 * idea of a process is a fiction.
 *
 * The spinning is so the two are running at the same time when they check,
 * rather than one after the other.
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

static long fork_(void) { return syscall3(220, 0, 0, 0); }

/* Same address in both, different memory after the fork. */
static volatile long value = 100;

void _start(void) {
    say("  [forky] before the fork my variable is "); say_num(value); say("\n");

    long r = fork_();

    if (r == 0) {
        value = 222;
        say("  [child]  fork gave me 0, and I set the variable to 222\n");
        for (volatile long n = 0; n < 12000000; n++) { }
        say("  [child]  I am process "); say_num(getpid());
        say(" and my variable says "); say_num(value); say("\n");
        exit_(0);
    } else if (r > 0) {
        say("  [parent] fork gave me "); say_num(r); say(", the child's id\n");
        for (volatile long n = 0; n < 20000000; n++) { }
        say("  [parent] I am process "); say_num(getpid());
        say(" and my variable says "); say_num(value); say("\n");
        if (value != 100) {
            say("  [parent] THE CHILD REACHED INTO MY MEMORY\n");
            exit_(1);
        }
        exit_(0);
    }
    say("  [forky] fork failed\n");
    exit_(1);
}
