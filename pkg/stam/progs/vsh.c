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
 * It passes arguments, runs a | b | c, redirects with > >> and <,\n * and puts a job in the background with &.
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
static long close_(int fd)              { return syscall3(57, fd, 0, 0); }
static long pipe_(int fds[2])           { return syscall3(59, (long)fds, 0, 0); }
static long dup3_(int old, int newfd)   { return syscall3(24, old, newfd, 0); }
/* openat(AT_FDCWD, path, flags): -100 is AT_FDCWD, which the kernel ignores
 * because there are no directories. 0 is read-only; O_WRONLY|O_CREAT is 0101. */
/* 0 read, 0101 = O_WRONLY|O_CREAT, 02101 adds O_APPEND. */
static long setpgid_(int pid, int pgid) { return syscall3(154, pid, pgid, 0); }
/* TIOCSPGRP: tell the terminal which group its characters are addressed to.
 * The real interface, not one of our own — a shell that used a private call
 * could not be replaced by any other shell. */
static long fg_(int pgid) { int p = pgid; return syscall3(29, 0, 0x5410, (long)&p); }

static long open_(const char *p, int mode) {
    return syscall3(56, -100, (long)p, mode == 0 ? 0 : (mode == 1 ? 0101 : 02101));
}
static long getpid(void)          { return syscall3(172, 0, 0, 0); }
static long fork_(void)           { return syscall3(220, 0, 0, 0); }
static long exec_(const char *p, char *const *av) { return syscall3(221, (long)p, (long)av, 0); }
static long wait_(int *st)        { return syscall3(260, -1, (long)st, 0); }
/* WNOHANG. Background jobs are collected here rather than waited for, because
 * a shell that never collects them leaves zombies holding process slots. */
static long reap_(int *st)        { return syscall3(260, -1, (long)st, 1); }
static void exit_(int c)          { syscall3(93, c, 0, 0); __builtin_unreachable(); }

/* One line at a time, from whatever fd 0 turns out to be.
 *
 * The console hands over exactly one line per read, because the line discipline
 * is in the kernel. A file hands over as much as fits in the buffer, so the
 * shell has to do its own splitting — and once it does, the same loop reads a
 * script and a person without knowing which it has. That is the point: a shell
 * that can only be typed at is a shell no test can drive, and every attempt to
 * drive this one over a serial line has been intermittent (D-062).
 *
 * Returns the line length, or 0 at end of input. */
static char  inbuf[256];
static int   inlen, inpos;

static long get_line(char *out, int max) {
    int n = 0;
    for (;;) {
        if (inpos >= inlen) {
            long got = read_(inbuf, sizeof inbuf);
            if (got <= 0) return n;          /* end of input, or an error */
            inlen = (int)got; inpos = 0;
        }
        while (inpos < inlen) {
            char c = inbuf[inpos++];
            if (c == '\n') { out[n] = 0; return n + 1; }
            if (n < max - 1) out[n++] = c;
        }
    }
}

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

#define MAX_WORDS 24
#define MAX_SEG    4

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

/* Open a name and put it at fd 0 or 1. Runs in the child, after the fork and
 * before the exec, which is the only moment at which the descriptors can be
 * arranged for a program that does not know they were. */
static int redirect(const char *name, int fd, int mode) {
    long f = open_(name, mode);
    if (f < 0) {
        say("  vsh: cannot open ");
        say(name);
        say("\n");
        return -1;
    }
    dup3_((int)f, fd);
    close_((int)f);
    return 0;
}

/* Resolve a command name to a path in the initrd and become it. Only returns
 * when exec failed, and then the process must not carry on being a copy of the
 * shell — a second prompt from a forked child is a shell nobody can quit. */
static void run_or_die(char *path, unsigned long size, char **argv) {
    path[0] = '/';
    unsigned long i = 0;
    for (; i < size - 2 && argv[0][i]; i++) path[i + 1] = argv[0][i];
    path[i + 1] = 0;
    /* argv[0] is the name as typed, not the path we resolved — which is what
     * every shell does, and what a program printing its own name expects. */
    exec_(path, argv);
    say("  vsh: could not run ");
    say(argv[0]);
    say("\n");
    exit_(127);
}

static int scripted;

int main_loop(void) {
    char line[128];
    char path[130];
    char *words[MAX_WORDS];

    /* A script, if the initrd carries one. Opening it onto fd 0 means the loop
     * below does not change at all — the shell does not know whether it is
     * being typed at, which is what makes the script path worth having rather
     * than a second code path that could drift from the first. */
    /* The shell is its own group, and owns the terminal until it hands it to
     * a job. Without this the foreground group is zero and Ctrl-C reaches
     * nobody at all. */
    setpgid_(0, 0);
    fg_(getpid());

    scripted = 0;
    {
        long f = open_("/rc", 0);
        if (f >= 0) { dup3_((int)f, 0); close_((int)f); scripted = 1; }
    }

    say("\n  vsh — VerkOS shell, running on stam.\n");
    say("  Try: hello one two, forky, twice.  Also: help, pid, exit.\n\n");

    for (;;) {
        /* Collect anything that finished in the background. Done at the
         * prompt because that is where the shell is idle and where a person
         * can be told, and because the alternative is a slot leak. */
        for (;;) {
            int st = 0;
            long done = reap_(&st);
            if (done <= 0) break;
            say("  [done "); say_num(done); say("]\n");
        }
        say("verk$ ");
        /* A script's lines are not echoed by a terminal, so the shell echoes
         * them: a log that shows the answers and not the questions cannot be
         * read, and this log exists to be read by a test. */
        long n = get_line(line, sizeof line);
        if (n == 0) { say("\n"); return 0; }  /* end of a script, or of input */
        if (n == 1) continue;                 /* a blank line */

        if (scripted) { say(line); say("\n"); }

        int nw = split(line, words, MAX_WORDS);
        if (nw < 0) { say("  vsh: too many words\n"); continue; }
        if (nw == 0) continue;

        if (same(words[0], "exit")) { say("  bye\n"); return 0; }
        if (same(words[0], "pid"))  { say("  I am process "); say_num(getpid()); say("\n"); continue; }
        if (same(words[0], "help")) {
            say("  built in: help, pid, exit\n");
            say("  anything else is looked for in the initrd and run,\n");
            say("  with the rest of the line as its arguments.\n");
            say("  a | b | c   each reads what the one before it writes\n");
            say("  a > f  a >> f  a < f   redirect, truncating or appending\n");
            say("  a &    run it in the background\n");
            continue;
        }

        /* Pull the redirections and the trailing & out of the words before
         * anything else looks at them, so a filename is never mistaken for an
         * argument. Doing it here rather than in the child means a bad
         * filename is reported at the prompt instead of by a process that has
         * already forked and can only exit with a number. */
        char *out_name = 0, *in_name = 0;
        int out_append = 0, background = 0;
        {
            if (nw > 0 && same(words[nw - 1], "&")) { background = 1; words[--nw] = 0; }
            int w = 0;
            for (int i = 0; i < nw; i++) {
                if (same(words[i], ">>") && i + 1 < nw) { out_name = words[++i]; out_append = 1; continue; }
                if (same(words[i], ">")  && i + 1 < nw) { out_name = words[++i]; out_append = 0; continue; }
                if (same(words[i], "<")  && i + 1 < nw) { in_name  = words[++i]; continue; }
                words[w++] = words[i];
            }
            words[w] = 0;
            nw = w;
            if (nw == 0) { say("  vsh: no command\n"); continue; }
        }

        /* Split on | into segments. A chain is the same three system calls in
         * a loop, and writing the loop is what shows the descriptors are
         * general rather than a special case with two sides. */
        char **seg[MAX_SEG];
        int nseg = 0;
        seg[nseg++] = words;
        for (int i = 0; i < nw; i++)
            if (same(words[i], "|")) {
                words[i] = 0;
                if (nseg == MAX_SEG) { nseg = -1; break; }
                seg[nseg++] = &words[i + 1];
            }
        if (nseg < 0) { say("  vsh: too many pipes\n"); continue; }
        {
            int empty = 0;
            for (int i = 0; i < nseg; i++) if (!*seg[i]) empty = 1;
            if (empty) { say("  vsh: empty command in the pipeline\n"); continue; }
        }

        /* One pipe at a time, and the parent keeps exactly one descriptor
         * between iterations: the read end the next program will use. Holding
         * any more means a pipe whose writer has finished still has an open
         * write end somewhere, and its reader never sees end of file. */
        int prev_read = -1, failed = 0;
        long last = -1, job = 0;      /* the group every stage of this job joins */
        for (int i = 0; i < nseg; i++) {
            int pfd[2] = { -1, -1 };
            if (i + 1 < nseg && pipe_(pfd) < 0) { say("  vsh: cannot make a pipe\n"); failed = 1; break; }

            long pid = fork_();
            if (pid == 0) {
                /* Set in the child as well as the parent, because either may
                 * run first and the group has to exist before the terminal is
                 * pointed at it. Doing it in one place only is a race whose
                 * symptom is a Ctrl-C that sometimes reaches nothing. */
                setpgid_(0, (int)job);
                if (prev_read >= 0) { dup3_(prev_read, 0); close_(prev_read); }
                else if (in_name && redirect(in_name, 0, 0) < 0) exit_(1);

                if (i + 1 < nseg) { dup3_(pfd[1], 1); close_(pfd[0]); close_(pfd[1]); }
                else if (out_name && redirect(out_name, 1, out_append ? 2 : 1) < 0) exit_(1);

                run_or_die(path, sizeof path, seg[i]);
            }
            if (!job) job = pid;              /* the first stage names the job */
            setpgid_((int)pid, (int)job);
            if (prev_read >= 0) close_(prev_read);
            if (pid < 0) {
                say("  vsh: cannot fork\n");
                if (i + 1 < nseg) { close_(pfd[0]); close_(pfd[1]); }
                failed = 1;
                break;
            }
            if (i + 1 < nseg) { close_(pfd[1]); prev_read = pfd[0]; }
            else prev_read = -1;
            last = pid;
        }
        if (prev_read >= 0) close_(prev_read);
        if (failed) continue;

        if (!background && job) fg_((int)job);
        if (background) {
            /* Not waited for. Collected later, at a prompt, because a zombie
             * holds a process slot and sixteen of those is a machine that
             * cannot fork. */
            say("  ["); say_num(last); say("]\n");
            continue;
        }

        int status = 0;
        for (int i = 0; i < nseg; i++) wait_(&status);   /* the last one's code wins */
        /* The shell takes the terminal back before it prompts again, or the
         * next Ctrl-C would go to a group whose processes are all gone. */
        fg_(getpid());
        int code = (status >> 8) & 0xff;
        if (code == 128 + 2) say("  interrupted\n");
        else if (code) { say("  ["); say_num(code); say("]\n"); }

    }
}

void _start(void) { exit_(main_loop()); }
