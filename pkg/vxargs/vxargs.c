/* vxargs — VerkOS' own xargs.
 *
 * Reads items from stdin and builds/executes command lines from them. Installed
 * as `xargs`.
 *
 *   xargs [-0] [-n MAX] [-I REPLSTR] [CMD [ARG...]]
 *     -0   items are NUL-separated        -n MAX  at most MAX items per command
 *     -I S replace occurrences of S with one item (implies -n 1)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int run(char **argv) {
    pid_t p = fork();
    if (p == 0) { execvp(argv[0], argv); perror("xargs"); _exit(127); }
    int st = 0; if (p > 0) waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

/* return a malloc'd copy of s with every occurrence of `from` replaced by `to` */
static char *str_replace(const char *s, const char *from, const char *to) {
    size_t fl = strlen(from), tl = strlen(to);
    if (!fl) return strdup(s);
    size_t cap = strlen(s) + 1, n = 0; char *out = malloc(cap);
    for (const char *p = s; *p; ) {
        if (!strncmp(p, from, fl)) {
            while (n + tl + 1 > cap) { cap *= 2; out = realloc(out, cap); }
            memcpy(out + n, to, tl); n += tl; p += fl;
        } else {
            if (n + 2 > cap) { cap *= 2; out = realloc(out, cap); }
            out[n++] = *p++;
        }
    }
    out[n] = 0;
    return out;
}

/* read one item (whitespace- or NUL-delimited); returns malloc'd string or NULL */
static char *read_item(int nul) {
    char *b = NULL; size_t cap = 0, n = 0; int c;
    /* skip leading separators */
    while ((c = getchar()) != EOF) { if (nul ? c != '\0' : (c != ' ' && c != '\t' && c != '\n')) break; }
    if (c == EOF) return NULL;
    do {
        if (nul ? c == '\0' : (c == ' ' || c == '\t' || c == '\n')) break;
        if (n + 1 >= cap) { cap = cap ? cap * 2 : 64; b = realloc(b, cap); }
        b[n++] = (char)c;
    } while ((c = getchar()) != EOF);
    if (b) b[n] = 0;
    return b;
}

int main(int argc, char **argv) {
    int nul = 0, maxn = 0; const char *repl = NULL;
    char *base[256]; int nb = 0;
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-0")) nul = 1;
        else if (!strncmp(argv[i], "-n", 2)) maxn = atoi(argv[i][2] ? argv[i] + 2 : (i + 1 < argc ? argv[++i] : "0"));
        else if (!strncmp(argv[i], "-I", 2)) { repl = argv[i][2] ? argv[i] + 2 : (i + 1 < argc ? argv[++i] : "{}"); maxn = 1; }
        else break;
    }
    for (; i < argc && nb < 250; i++) base[nb++] = argv[i];
    if (nb == 0) base[nb++] = (char *)"echo";           /* default command */

    int ret = 0;
    if (repl) {                                          /* -I: one item per run, substituted */
        char *item;
        while ((item = read_item(nul))) {
            char *av[256]; int n = 0;
            for (int k = 0; k < nb; k++) av[n++] = str_replace(base[k], repl, item);   /* substring replace */
            av[n] = NULL;
            ret |= run(av);
            for (int k = 0; k < nb; k++) free(av[k]);
            free(item);
        }
        return ret;
    }

    char *items[4096]; int ni = 0; char *item;
    while ((item = read_item(nul))) {
        items[ni++] = item;
        int batch_full = maxn && ni >= maxn;
        if (batch_full || ni >= 4000) {
            char *av[4300]; int n = 0;
            for (int k = 0; k < nb; k++) av[n++] = base[k];
            for (int k = 0; k < ni; k++) av[n++] = items[k];
            av[n] = NULL; ret |= run(av);
            for (int k = 0; k < ni; k++) free(items[k]);
            ni = 0;
        }
    }
    if (ni > 0) {
        char *av[4300]; int n = 0;
        for (int k = 0; k < nb; k++) av[n++] = base[k];
        for (int k = 0; k < ni; k++) av[n++] = items[k];
        av[n] = NULL; ret |= run(av);
        for (int k = 0; k < ni; k++) free(items[k]);
    }
    return ret;
}
