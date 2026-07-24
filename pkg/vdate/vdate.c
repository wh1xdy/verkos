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
#include <time.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    int utc = 0, rfc = 0;
    const char *fmt = NULL;
    time_t t = time(NULL);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-u") || !strcmp(argv[i], "--utc")) utc = 1;
        else if (!strcmp(argv[i], "-R")) rfc = 1;
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) { struct stat st; if (stat(argv[++i], &st) == 0) t = st.st_mtime; }
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) { const char *s = argv[++i]; if (*s == '@') t = (time_t)strtoll(s + 1, NULL, 10); }
        else if (argv[i][0] == '+') fmt = argv[i] + 1;
    }
    struct tm *tm = utc ? gmtime(&t) : localtime(&t);
    char buf[512];
    const char *f = fmt ? fmt : rfc ? "%a, %d %b %Y %H:%M:%S %z" : "%a %b %e %H:%M:%S %Z %Y";
    strftime(buf, sizeof buf, f, tm);
    printf("%s\n", buf);
    return 0;
}
