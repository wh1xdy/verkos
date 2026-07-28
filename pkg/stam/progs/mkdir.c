/* mkdir — one directory, on the medium. */
#include "syscalls.h"

static long mkdirat_(const char *p) { return syscall3(34, -100, (long)p, 0755); }

VERK_START(start_c) {
    long argc = (long)sp[0];
    char **argv = (char **)&sp[1];

    if (argc < 2) { say("mkdir: a name, please\n"); exit_(1); }
    for (long i = 1; i < argc; i++) {
        long r = mkdirat_(argv[i]);
        if (r < 0) {
            say("mkdir: "); say(argv[i]); say(": "); num(-r); say("\n");
            exit_(1);
        }
    }
    exit_(0);
}
