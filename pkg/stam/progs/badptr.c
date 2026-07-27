/* badptr — hands the kernel addresses it has no business dereferencing, and
 * checks that it is told no.
 *
 * The MMU protects a program from the kernel's memory. It says nothing about
 * the kernel touching an address a program *gave* it: at EL1 the same load is
 * permitted, and an unmapped one is not a program being killed but the machine
 * stopping. So every pointer arriving through `svc` has to be checked, and this
 * is the program that decides whether it is.
 *
 * It exits non-zero on the first case that does not come back -EFAULT, so the
 * answer is a status a shell can act on rather than prose a person must read.
 * A kernel without the checks does not fail this test — it stops running it.
 */
static long syscall3(long n, long a, long b, long c) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}
static long write_(int fd, const char *s, unsigned long n) { return syscall3(64, fd, (long)s, (long)n); }
static void exit_(int c) { syscall3(93, c, 0, 0); __builtin_unreachable(); }

static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void say(const char *s) { write_(1, s, slen(s)); }
static void say_num(long v) {
    char b[24]; int n = 0; int neg = v < 0;
    if (neg) v = -v;
    if (v == 0) b[n++] = '0';
    while (v > 0) { b[n++] = (char)('0' + v % 10); v /= 10; }
    char o[26]; int i = 0;
    if (neg) o[i++] = '-';
    while (n--) o[i++] = b[n];
    write_(1, o, (unsigned long)i);
}

#define EFAULT 14

static int failures;

/* Every case is "the kernel must refuse this". Reporting the number it actually
 * returned matters: -EFAULT and -EBADF are both refusals and only one of them
 * means the pointer was checked. */
static void must_refuse(const char *what, long r) {
    say("  [badptr] ");
    say(what);
    say(" -> ");
    say_num(r);
    if (r == -EFAULT) { say("  (-EFAULT, refused)\n"); return; }
    say("  NOT REFUSED AS A BAD ADDRESS\n");
    failures++;
}

void _start(void) {
    say("  [badptr] handing the kernel addresses that are not mine\n");

    /* The kernel's own half. Reachable by EL1, which is the point: without a
     * check the kernel would happily print its own memory to the console. */
    must_refuse("write from a kernel address",
                syscall3(64, 1, (long)0xffffff8040080000ul, 32));

    /* Never mapped, in the user half. Without a check this faults at EL1. */
    must_refuse("write from an unmapped user address",
                syscall3(64, 1, (long)0x600000000ul, 16));

    /* Above the 512 GiB the low half can describe at all. */
    must_refuse("write from beyond the user half",
                syscall3(64, 1, (long)0x8000000000ul, 8));

    /* A read whose destination is not ours: this one would corrupt the kernel
     * rather than merely reveal it. */
    must_refuse("read into a kernel address",
                syscall3(63, 0, (long)0xffffff8040080000ul, 16));

    /* wait4's status pointer is written after a child is collected, which is
     * the worst moment to discover it is unmapped. */
    must_refuse("wait4 with a status pointer that is not mine",
                syscall3(260, -1, (long)0xffffff8040080000ul, 0));

    /* execve's path is a string, so the kernel must walk it without running
     * off the end of what is mapped. */
    must_refuse("execve with a path that is not mine",
                syscall3(221, (long)0x600000000ul, 0, 0));

    /* And the argv vector itself, which is a second level of indirection: the
     * array may be mapped while the strings it points at are not. */
    {
        const char *av[2];
        av[0] = (const char *)0x600000000ul;
        av[1] = 0;
        must_refuse("execve with an argv string that is not mine",
                    syscall3(221, (long)"/hello", (long)av, 0));
    }

    /* A length that wraps past the end of the address space. Every byte of it
     * would be "mapped" to a check that only looked at the first. */
    must_refuse("write with a length that wraps",
                syscall3(64, 1, (long)0x7f00100000ul, (long)0xffffffffffffff00ul));

    if (failures == 0)
        say("  [badptr] every bad address was refused. the kernel is still here.\n");
    else {
        say("  [badptr] ");
        say_num(failures);
        say(" case(s) were not refused\n");
    }
    exit_(failures);
}
