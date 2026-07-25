/* vdate — VerkOS' own date.
 *
 * Prints (or formats) the current time, a file's time, or an epoch. Installed as
 * `date` only if it passes the build-safety check.
 *
 *   date [-u] [-R] [-r FILE] [-d @EPOCH] [+FORMAT]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    int utc = 0, rfc = 0;
    const char *fmt = NULL;
    time_t t = time(NULL);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-u") || !strcmp(argv[i], "--utc")) utc = 1;
        else if (!strcmp(argv[i], "-R")) rfc = 1;
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) {
            /* A file we cannot stat must not silently leave the time as "now". */
            struct stat st;
            if (stat(argv[++i], &st) != 0) {
                fprintf(stderr, "date: %s: %s\n", argv[i], strerror(errno)); return 1;
            }
            t = st.st_mtime;
        }
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) {
            /* Only @epoch is understood. Ignoring every other form prints the
             * current time as if it were the requested one — a wrong answer
             * that looks exactly like a right one. */
            const char *s = argv[++i];
            if (*s != '@') {
                fprintf(stderr, "date: -d understands only @SECONDS, not '%s'\n", s); return 1;
            }
            t = (time_t)strtoll(s + 1, NULL, 10);
        }
        else if (argv[i][0] == '+') fmt = argv[i] + 1;
        /* An option we do not know must not be quietly treated as a file name:
         * that is how a typo turns into the wrong thing done silently. */
        else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "date: unknown option %s\n", argv[i]); return 1;
        }
    }
    struct tm *tm = utc ? gmtime(&t) : localtime(&t);
    char buf[512];
    const char *f = fmt ? fmt : rfc ? "%a, %d %b %Y %H:%M:%S %z" : "%a %b %e %H:%M:%S %Z %Y";
    strftime(buf, sizeof buf, f, tm);
    printf("%s\n", buf);
    return 0;
}
