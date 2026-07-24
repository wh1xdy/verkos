/* vgetty — VerkOS's own getty.
 *
 * A getty is a leaf process: systemd starts it as the ExecStart of getty@ /
 * serial-getty@ on a console, and its whole job is to prepare a terminal and
 * hand off to /bin/login. So it can be swapped for our own with zero PID-1
 * coupling and zero boot risk — the first step of making VerkOS' init userland
 * ours (in the spirit of vpk/verkbox; dhcpcd already replaced networkd).
 *
 * vgetty opens the tty, makes it the session's controlling terminal, applies
 * sane termios, and execs /bin/login (with -f <user> for the autologin path).
 * It accepts agetty-style arguments so the systemd unit reads naturally:
 *
 *   vgetty [--autologin USER] [--noclear] [--keep-baud] TTY [BAUD_LIST] [TERM]
 *
 * Unknown options and the baud list are ignored; the first non-option word is
 * the tty (systemd's %I), a following non-numeric word is $TERM.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <signal.h>

int main(int argc, char **argv) {
    const char *tty = NULL, *user = NULL, *term = NULL;
    int autologin = 0;

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!strcmp(a, "--autologin") || !strcmp(a, "-a")) {
            autologin = 1;
            if (i + 1 < argc) user = argv[++i];
        } else if (a[0] == '-') {
            /* --noclear, --keep-baud, -8, -J, ... — irrelevant to us, ignore */
        } else if (!tty) {
            tty = a;                       /* first bare word = tty (systemd %I) */
        } else if (isdigit((unsigned char)a[0])) {
            /* a baud list like "115200,38400,9600" — the kernel console already
             * set the speed for us, so ignore */
        } else {
            term = a;                      /* a bare non-numeric word = $TERM */
        }
    }
    if (!tty) { fprintf(stderr, "vgetty: no tty specified\n"); return 1; }

    char path[512];
    if (tty[0] == '/') snprintf(path, sizeof path, "%s", tty);
    else               snprintf(path, sizeof path, "/dev/%s", tty);

    /* Start a new session so this tty can become our controlling terminal.
     * (Harmless if we are already a session leader.) */
    setsid();

    int fd = open(path, O_RDWR | O_NOCTTY);
    if (fd < 0) { perror("vgetty: open tty"); return 1; }

    ioctl(fd, TIOCSCTTY, 1);               /* claim it as the controlling tty */

    struct termios t;
    if (tcgetattr(fd, &t) == 0) {          /* sane cooked-mode defaults for login */
        t.c_iflag |= ICRNL | IXON | BRKINT;
        t.c_oflag |= OPOST | ONLCR;
        t.c_lflag |= ICANON | ECHO | ECHOE | ECHOK | ISIG;
        t.c_cflag |= CREAD | HUPCL;
        t.c_cc[VERASE] = 0177;             /* DEL erases */
        tcsetattr(fd, TCSANOW, &t);
    }

    dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
    if (fd > 2) close(fd);
    tcflush(0, TCIOFLUSH);

    if (term) setenv("TERM", term, 1);
    else if (!getenv("TERM")) setenv("TERM", "linux", 1);

    signal(SIGHUP, SIG_DFL);

    /* Hand off to login. -f skips authentication for the named user (autologin);
     * without it, login prompts for the username as usual. */
    if (autologin && user) execl("/bin/login", "login", "-f", user, (char *)NULL);
    else                   execl("/bin/login", "login", (char *)NULL);

    perror("vgetty: exec /bin/login");
    return 1;
}
