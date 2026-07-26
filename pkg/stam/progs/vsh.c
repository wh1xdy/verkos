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
 * It passes arguments now. Still no pipes and no redirection.
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
static long exec_(const char *p, char *const *av) { return syscall3(221, (long)p, (long)av, 0); }
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

#define MAX_WORDS 16

/* Split the line in place: every run of spaces becomes a terminator, and each
 * word is pointed at where it already lies. Nothing is copied, so there is no
 * second buffer to size wrongly. Returns the number of words found. */
static int split(char *line, char *words[], int max) {
    int n = 0;
    char *p = line;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (n == max - 1) return -1;         /* one short: argv needs its NULL */
        words[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    words[n] = 0;
    return n;
}

int main_loop(void) {
    char line[128];
    char path[130];
    char *words[MAX_WORDS];

    say("\n  vsh — VerkOS shell, running on stam.\n");
    say("  Try: hello one two, forky, twice.  Also: help, pid, exit.\n\n");

    for (;;) {
        say("verk$ ");
        long n = read_(line, sizeof line);
        if (n <= 0) continue;
        line[n - 1] = 0;                      /* drop the newline */

        int nw = split(line, words, MAX_WORDS);
        if (nw < 0) { say("  vsh: too many words\n"); continue; }
        if (nw == 0) continue;
        char *cmd = words[0];

        if (same(cmd, "exit")) { say("  bye\n"); return 0; }
        if (same(cmd, "pid"))  { say("  I am process "); say_num(getpid()); say("\n"); continue; }
        if (same(cmd, "help")) {
            say("  built in: help, pid, exit\n");
            say("  anything else is looked for in the initrd and run,\n");
            say("  with the rest of the line as its arguments\n");
            continue;
        }

        /* Not a built-in: it must be a program. */
        path[0] = '/';
        unsigned long i = 0;
        for (; i < sizeof path - 2 && cmd[i]; i++) path[i + 1] = cmd[i];
        path[i + 1] = 0;

        long child = fork_();
        if (child == 0) {
            /* argv[0] is the name as typed, not the path we resolved — which is
             * what every shell does, and what a program printing its own name
             * expects to see. */
            exec_(path, words);
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
