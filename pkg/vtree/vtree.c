/* vtree — VerkOS' own tree (recursive indented directory listing).
 *
 *   tree [-a] [-d] [-L LEVEL] [PATH]
 *     -a all (incl. dotfiles)   -d dirs only   -L max depth
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

static int showall, dirsonly, maxlevel = -1;
static long ndirs, nfiles;

static int namecmp(const void *a, const void *b) { return strcmp(*(char **)a, *(char **)b); }

static void walk(const char *path, const char *prefix, int level) {
    if (maxlevel >= 0 && level > maxlevel) return;
    DIR *d = opendir(path); if (!d) return;
    char **names = NULL; int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!showall && e->d_name[0] == '.') continue;
        if (n >= cap) { cap = cap ? cap * 2 : 32; names = realloc(names, cap * sizeof *names); }
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof *names, namecmp);
    for (int i = 0; i < n; i++) {
        char full[4096]; snprintf(full, sizeof full, "%s/%s", path, names[i]);
        struct stat st; int isdir = (lstat(full, &st) == 0 && S_ISDIR(st.st_mode));
        if (dirsonly && !isdir) { free(names[i]); continue; }
        int last = (i == n - 1);
        printf("%s%s%s\n", prefix, last ? "└── " : "├── ", names[i]);
        if (isdir) {
            ndirs++;
            char np[4096]; snprintf(np, sizeof np, "%s%s", prefix, last ? "    " : "│   ");
            walk(full, np, level + 1);
        } else nfiles++;
        free(names[i]);
    }
    free(names);
}

int main(int argc, char **argv) {
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-a")) showall = 1;
        else if (!strcmp(argv[i], "-d")) dirsonly = 1;
        else if (!strcmp(argv[i], "-L") && i + 1 < argc) maxlevel = atoi(argv[++i]) - 1;
        else if (argv[i][0] != '-') path = argv[i];
    }
    if (!path) path = ".";
    printf("%s\n", path);
    walk(path, "", 0);
    printf("\n%ld director%s, %ld file%s\n", ndirs, ndirs == 1 ? "y" : "ies", nfiles, nfiles == 1 ? "" : "s");
    return 0;
}
