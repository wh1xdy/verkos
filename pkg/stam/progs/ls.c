/* ls — the names in a directory, off the medium.
 *
 * It exists to prove the interface rather than to be a good ls: no columns, no
 * sorting, no flags. What it shows is that a directory is a thing a program can
 * open and read through ordinary calls, with the kernel telling it nothing it
 * could not have asked for.
 */
#include "syscalls.h"

static long openat_(const char *p) { return syscall3(56, -100, (long)p, 0); }
static long getdents_(int fd, void *b, unsigned long n) { return syscall3(61, fd, (long)b, (long)n); }
static long close_(int fd) { return syscall3(57, fd, 0, 0); }

VERK_START(start_c) {
    long argc = (long)sp[0];
    char **argv = (char **)&sp[1];

    const char *path = (argc > 1) ? argv[1] : "/";
    long fd = openat_(path);
    if (fd < 0) { say("ls: cannot open "); say(path); say("\n"); exit_(1); }

    char buf[1024];
    int any = 0;
    for (;;) {
        long n = getdents_((int)fd, buf, sizeof buf);
        if (n <= 0) break;
        for (long at = 0; at < n; ) {
            unsigned reclen = (unsigned char)buf[at + 16] | ((unsigned char)buf[at + 17] << 8);
            if (!reclen) break;
            unsigned char type = (unsigned char)buf[at + 18];
            const char *name = &buf[at + 19];
            say("  ");
            say(name);
            /* A trailing slash rather than a column: it is one character and it
             * is the only thing about an entry a reader needs at a glance. */
            if (type == 4) say("/");
            say("\n");
            any = 1;
            at += reclen;
        }
    }
    close_((int)fd);
    if (!any) say("  (empty)\n");
    exit_(0);
}
