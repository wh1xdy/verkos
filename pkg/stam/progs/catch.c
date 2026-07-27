/* catch — installs a signal handler, gets interrupted, and carries on.
 *
 * The thing being tested is not "did the handler run". A handler that runs and
 * then leaves the program somewhere else is worse than no handler at all, and
 * a check that only watches for the handler's own output would pass on it. So
 * the child spins in a loop with four values in caller-saved registers, and
 * after the handler returns it compares them against what it put there. The
 * handler is an ordinary C function and the compiler is entitled to destroy
 * every one of those registers; nothing but the kernel's saved frame can bring
 * them back.
 *
 * The loop makes no system calls either, so the only way the signal reaches it
 * is delivery on the way out of a timer interrupt (D-075).
 *
 * The exit status carries the verdict: 7 only if the handler ran and everything
 * came back. Without a handler the child would die of the signal and exit 130,
 * which is a different number, so "it worked" and "it was never installed" are
 * not the same result.
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
static long fork_(void)             { return syscall3(220, 0, 0, 0); }
static long wait_(int *st)          { return syscall3(260, -1, (long)st, 0); }
static long kill_(int pid, int s)   { return syscall3(129, pid, s, 0); }
static long sigaction_(int s, void *h, void *r) { return syscall3(134, s, (long)h, (long)r); }
static void exit_(int c) { syscall3(93, c, 0, 0); __builtin_unreachable(); }

static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void say(const char *s) { write_(s, slen(s)); }
static void num(long v) {
    char b[24]; int i = 23; b[i--] = 0;
    if (v < 0) { say("-"); v = -v; }
    if (!v) b[i--] = '0';
    while (v) { b[i--] = '0' + (char)(v % 10); v /= 10; }
    say(&b[i + 1]);
}

#define SIGINT   2
#define SIGKILL  9

/* Where the handler returns to. There is no vDSO to keep it in, so the program
 * carries its own — two instructions, and the kernel refuses a sigaction that
 * does not name one. */
__asm__(".globl sigrestorer\n"
        "sigrestorer:\n"
        "    mov x8, #139\n"        /* rt_sigreturn */
        "    svc #0\n");
extern void sigrestorer(void);

static volatile int caught;
static volatile int caught_sig;

static void handler(int sig) {
    caught_sig = sig;
    caught = 1;
}

static void child(void) {
    long ok = -1;
    /* x9-x12 are caller-saved: the handler may destroy all four and the
     * compiler will not put anything back. The loop reads `caught` from memory
     * every time round rather than caching it, because the handler writes it
     * and this code never calls anything that would make the compiler reload. */
    __asm__ volatile(
        "   mov  x9,  #0x1111\n"
        "   mov  x10, #0x2222\n"
        "   mov  x11, #0x3333\n"
        "   mov  x12, #0x4444\n"
        "1: ldr  w0, [%1]\n"
        "   cbz  w0, 1b\n"
        "   mov  %0, #0\n"
        "   mov  x13, #0x1111\n cmp x9,  x13\n b.ne 2f\n"
        "   mov  x13, #0x2222\n cmp x10, x13\n b.ne 2f\n"
        "   mov  x13, #0x3333\n cmp x11, x13\n b.ne 2f\n"
        "   mov  x13, #0x4444\n cmp x12, x13\n b.ne 2f\n"
        "   mov  %0, #1\n"
        "2:\n"
        : "=&r"(ok)
        : "r"(&caught)
        : "x0", "x9", "x10", "x11", "x12", "x13", "cc", "memory");

    if (caught_sig != SIGINT) {
        say("  [catch] the handler ran for signal "); num(caught_sig); say(", not SIGINT\n");
        exit_(4);
    }
    if (!ok) {
        say("  [catch] the handler ran and the registers did not come back\n");
        exit_(3);
    }
    say("  [catch] interrupted, handled, and resumed with every register intact\n");
    exit_(7);
}

void _start(void) {
    say("  [catch] catching a signal instead of dying of it\n");

    /* What may not be installed, checked before anything depends on it. */
    if (sigaction_(SIGKILL, (void *)handler, (void *)sigrestorer) != -22) {
        say("  [catch] SIGKILL was catchable — it must not be\n");
        exit_(5);
    }
    if (sigaction_(SIGINT, (void *)handler, (void *)0) != -14) {
        say("  [catch] a handler with no way back was accepted\n");
        exit_(5);
    }
    if (sigaction_(SIGINT, (void *)0x10, (void *)sigrestorer) != -14) {
        say("  [catch] a handler at an address that is not mine was accepted\n");
        exit_(5);
    }
    say("  [catch] SIGKILL, a missing restorer and a bad handler were all refused\n");

    if (sigaction_(SIGINT, (void *)handler, (void *)sigrestorer) != 0) {
        say("  [catch] the handler would not install\n");
        exit_(5);
    }

    long pid = fork_();
    if (pid == 0) child();
    if (pid < 0) { say("  [catch] fork failed\n"); exit_(5); }

    /* The child inherits the handler across fork, which is the other half of
     * what is being checked: nothing here installs it again. */
    kill_((int)pid, SIGINT);

    int st = -1;
    wait_(&st);
    int code = (st >> 8) & 0xff;      /* the same encoding wait(2) uses */
    if (code != 7) {
        say("  [catch] the child exited "); num(code);
        say(" — 130 means it died of the signal, 3 means the frame did not come back\n");
        exit_(1);
    }
    say("  [catch] a handler ran in a loop that makes no system calls, and the program lived\n");
    exit_(0);
}
