/* The first VerkOS program compiled as a program.
 *
 * Freestanding and static: no libc, no dynamic loader, nothing but what is
 * written here and the four system calls stam implements. It is built by the
 * ordinary compiler into an ordinary ELF, and the kernel loads it out of the
 * initrd — which is how a kernel starts its first program when it has no
 * filesystem yet, and what every Linux system still does.
 */

/* aarch64: the call number goes in x8, arguments in x0-x5, the result comes
 * back in x0. Linux's numbering, which stam deliberately shares. */
static long syscall3(long n, long a, long b, long c) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

static long write(int fd, const char *s, unsigned long n) { return syscall3(64, fd, (long)s, (long)n); }
static long getpid(void)                                  { return syscall3(172, 0, 0, 0); }
static void exit_(int code)                               { syscall3(93, code, 0, 0); __builtin_unreachable(); }

static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void say(const char *s) { write(1, s, slen(s)); }

static void say_num(long v) {
    char b[24]; int n = 0;
    if (v == 0) b[n++] = '0';
    while (v > 0) { b[n++] = (char)('0' + v % 10); v /= 10; }
    char out[24]; int o = 0;
    while (n--) out[o++] = b[n];
    write(1, out, (unsigned long)o);
}

/* The kernel starts a program the way every aarch64 Linux one is started: the
 * stack pointer holds argc, then argv, then a NULL, then the environment. There
 * is no libc here to unpack that, so _start is two instructions of assembly
 * that hand the stack pointer to a C function and never come back. */
__asm__(
    ".global _start\n"
    "_start:\n"
    "  mov x0, sp\n"
    "  b    start_c\n");

void start_c(unsigned long *sp) {
    long argc = (long)sp[0];
    char **argv = (char **)&sp[1];

    say("  [hello] I am a separately compiled ELF, loaded from the initrd.\n");
    say("  [hello] no libc, no loader, a handful of system calls.\n");
    say("  [hello] getpid() says ");
    say_num(getpid());
    say("\n");

    /* The point of this program now: say back what it was told. An argument
     * list that arrived wrong shows up here as a wrong word, not as a crash. */
    say("  [hello] argc = ");
    say_num(argc);
    say("\n");
    for (long i = 0; i < argc; i++) {
        say("  [hello] argv[");
        say_num(i);
        say("] = \"");
        say(argv[i]);
        say("\"\n");
    }
    /* argv must be NULL-terminated, and a program is entitled to rely on it. */
    say(argv[argc] == 0 ? "  [hello] argv ends with NULL, as it must\n"
                        : "  [hello] argv IS NOT NULL-TERMINATED\n");

    /* Ask for something the kernel does not implement, to show the answer is
     * an error code and not a crash. */
    long r = syscall3(999, 0, 0, 0);
    say("  [hello] an unknown call returned ");
    say_num(-r);
    say(" (as -errno)\n");

    /* Exit non-zero when the arguments did not survive, so a shell that runs
     * `hello one two` gets an answer it can check rather than prose a person
     * has to read. */
    exit_(argv[argc] == 0 ? 0 : 1);
}
