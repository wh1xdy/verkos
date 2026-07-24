/* vfind — VerkOS' own find.
 *
 * Walks one or more directory trees and prints (or -exec's) entries matching a
 * few common tests. Installed as `find`.
 *
 *   find [PATH...] [-maxdepth N] [-type f|d|l] [-name GLOB] [-iname GLOB]
 *                  [-print|-print0|-exec CMD {} ;]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int maxdepth = -1, typ = 0, icase = 0, action = 0; /* action: 0 print, 1 print0, 2 exec */
static const char *pat = NULL;
static char **exec_argv; static int exec_n, exec_slot;    /* exec template, {} at exec_slot */

static void do_exec(const char *path) {
    char *av[256]; int n = 0;
    for (int i = 0; i < exec_n && n < 255; i++) av[n++] = (i == exec_slot) ? (char *)path : exec_argv[i];
    av[n] = NULL;
    pid_t p = fork();
    if (p == 0) { execvp(av[0], av); _exit(127); }
    if (p > 0) waitpid(p, NULL, 0);
}

static void emit(const char *path) {
    if (action == 1) { fputs(path, stdout); fputc('\0', stdout); }
    else if (action == 2) do_exec(path);
    else { fputs(path, stdout); fputc('\n', stdout); }
}

static int type_ok(const struct stat *st) {
    if (!typ) return 1;
    if (typ == 'f') return S_ISREG(st->st_mode);
    if (typ == 'd') return S_ISDIR(st->st_mode);
    if (typ == 'l') return S_ISLNK(st->st_mode);
    return 1;
}
static int name_ok(const char *base) {
    if (!pat) return 1;
    return fnmatch(pat, base, icase ? FNM_CASEFOLD : 0) == 0;
}

static void walk(const char *path, int depth) {
    struct stat st;
    if (lstat(path, &st) < 0) return;
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    if (type_ok(&st) && name_ok(base)) emit(path);
    if (!S_ISDIR(st.st_mode)) return;
    if (maxdepth >= 0 && depth >= maxdepth) return;
    DIR *d = opendir(path); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[4096];
        snprintf(child, sizeof child, "%s%s%s", path, path[strlen(path)-1] == '/' ? "" : "/", e->d_name);
        walk(child, depth + 1);
    }
    closedir(d);
}

int main(int argc, char **argv) {
    const char *paths[64]; int np = 0;
    int i = 1;
    for (; i < argc && argv[i][0] != '-'; i++) if (np < 64) paths[np++] = argv[i];
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-maxdepth") && i + 1 < argc) maxdepth = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-type") && i + 1 < argc) typ = argv[++i][0];
        else if (!strcmp(argv[i], "-name") && i + 1 < argc) pat = argv[++i];
        else if (!strcmp(argv[i], "-iname") && i + 1 < argc) { pat = argv[++i]; icase = 1; }
        else if (!strcmp(argv[i], "-print")) action = 0;
        else if (!strcmp(argv[i], "-print0")) action = 1;
        else if (!strcmp(argv[i], "-exec")) {
            action = 2; exec_argv = &argv[i + 1]; exec_n = 0; exec_slot = -1;
            int j = i + 1;
            for (; j < argc && strcmp(argv[j], ";") && strcmp(argv[j], "\\;"); j++) {
                if (!strcmp(argv[j], "{}")) exec_slot = exec_n;
                exec_n++;
            }
            i = j;
        }
        else { fprintf(stderr, "find: unknown test %s\n", argv[i]); return 1; }
    }
    if (np == 0) paths[np++] = ".";
    for (int k = 0; k < np; k++) walk(paths[k], 0);
    return 0;
}
