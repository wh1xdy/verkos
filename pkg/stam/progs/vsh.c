/* vsh — a shell, which is the smallest program that makes an operating system
 * usable rather than merely working.
 *
 * It does what every shell has done since the sixth edition: print a prompt,
 * read a line, and if the line names a program, fork, let the child become it,
 * and wait. Everything it needs from the kernel is already there — read, write,
 * fork, execve, wait4, exit — which is the point. A shell is not a feature of
 * the kernel, it is a program like any other, and being able to write it as one
 * is the evidence that the interface beneath it is real.
 *
 * No pipes, no redirection, no arguments — argv is not passed yet.
 */
static long syscall3(long n, long a, long b, long c) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}
static long read_(char *b, unsigned long n)  { return syscall3(63, 0, (long)b, (long)n); }
static long write_(const char *s, unsigned long n) { return syscall3(64, 1, (long)s, (long)n); }
static long getpid(void)          { return syscall3(172, 0, 0, 0); }
static long fork_(void)           { return syscall3(220, 0, 0, 0); }
static long exec_(const char *p)  { return syscall3(221, (long)p, 0, 0); }
static long wait_(int *st)        { return syscall3(260, -1, (long)st, 0); }
static void exit_(int c)          { syscall3(93, c, 0, 0); __builtin_unreachable(); }

static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void say(const char *s) { write_(s, slen(s)); }
static void say_num(long v) {
    char b[24]; int n = 0;
    if (v <= 0) b[n++] = v == 0 ? '0' : '?';
    while (v > 0) { b[n++] = (char)('0' + v % 10); v /= 10; }
    char o[24]; int i = 0; while (n--) o[i++] = b[n];
    write_(o, (unsigned long)i);
}
static int same(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int main_loop(void) {
    char line[128];
    char path[130];

    say("\n  vsh — VerkOS shell, running on stam.\n");
    say("  Try: hello, forky, twice.  Also: help, pid, exit.\n\n");

    for (;;) {
        say("verk$ ");
        long n = read_(line, sizeof line);
        if (n <= 0) continue;
        line[n - 1] = 0;                      /* drop the newline */

        /* trim leading spaces; take the first word */
        char *cmd = line;
        while (*cmd == ' ') cmd++;
        char *e = cmd;
        while (*e && *e != ' ') e++;
        *e = 0;
        if (!*cmd) continue;

        if (same(cmd, "exit")) { say("  bye\n"); return 0; }
        if (same(cmd, "pid"))  { say("  I am process "); say_num(getpid()); say("\n"); continue; }
        if (same(cmd, "help")) {
            say("  built in: help, pid, exit\n");
            say("  anything else is looked for in the initrd and run\n");
            continue;
        }

        /* Not a built-in: it must be a program. */
        path[0] = '/';
        unsigned long i = 0;
        for (; i < sizeof path - 2 && cmd[i]; i++) path[i + 1] = cmd[i];
        path[i + 1] = 0;

        long child = fork_();
        if (child == 0) {
            exec_(path);
            /* Only reached when exec failed: the kernel has already said why. */
            say("  vsh: could not run ");
            say(cmd);
            say("\n");
            exit_(127);
        }
        if (child < 0) { say("  vsh: cannot fork\n"); continue; }

        int status = 0;
        wait_(&status);
        int code = (status >> 8) & 0xff;
        if (code) { say("  ["); say_num(code); say("]\n"); }
    }
}

void _start(void) { exit_(main_loop()); }
