/* rm — one name, off the medium. A directory has to be empty; removing a tree
 * means walking it, and a walk that stopped halfway would leave clusters
 * belonging to a directory that no longer exists. */
#include "syscalls.h"

static long unlinkat_(const char *p) { return syscall3(35, -100, (long)p, 0); }

VERK_START(start_c) {
    long argc = (long)sp[0];
    char **argv = (char **)&sp[1];

    if (argc < 2) { say("rm: a name, please\n"); exit_(1); }
    for (long i = 1; i < argc; i++) {
        long r = unlinkat_(argv[i]);
        if (r < 0) {
            say("rm: "); say(argv[i]); say(": "); num(-r); say("\n");
            exit_(1);
        }
    }
    exit_(0);
}
