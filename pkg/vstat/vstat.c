/* vstat — VerkOS' own stat.
 *
 * Reports file metadata, verbose by default or via a -c format string (a subset
 * of GNU's %-codes). Installed as `stat` only if it passes the build-safety
 * check.
 *
 *   stat [-L] [-c FORMAT] FILE...
 *     -L  follow symlinks    -c  custom format (%n %s %F %a %A %U %G %u %g %i %h %y %Y ...)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>
#include <sys/stat.h>

static const char *type_str(mode_t m) {
    if (S_ISREG(m)) return "regular file";
    if (S_ISDIR(m)) return "directory";
    if (S_ISLNK(m)) return "symbolic link";
    if (S_ISCHR(m)) return "character special file";
    if (S_ISBLK(m)) return "block special file";
    if (S_ISFIFO(m)) return "fifo";
    if (S_ISSOCK(m)) return "socket";
    return "unknown";
}
static void mode_rwx(mode_t m, char *out) {
    const char *t = S_ISDIR(m) ? "d" : S_ISLNK(m) ? "l" : S_ISCHR(m) ? "c" : S_ISBLK(m) ? "b" : S_ISFIFO(m) ? "p" : S_ISSOCK(m) ? "s" : "-";
    out[0] = t[0];
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) out[1 + i] = (m & (0400 >> i)) ? rwx[i] : '-';
    out[10] = 0;
}
static const char *uname_of(uid_t u) { struct passwd *p = getpwuid(u); return p ? p->pw_name : "UNKNOWN"; }
static const char *gname_of(gid_t g) { struct group *p = getgrgid(g); return p ? p->gr_name : "UNKNOWN"; }

static void tstr(time_t t, char *out, size_t n) {
    struct tm *tm = localtime(&t);
    strftime(out, n, "%Y-%m-%d %H:%M:%S", tm);
}

static void fmt_custom(const char *fmt, const char *name, struct stat *st) {
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { putchar(*p); continue; }
        p++;
        char tb[64];
        switch (*p) {
            case 'n': fputs(name, stdout); break;
            case 's': printf("%lld", (long long)st->st_size); break;
            case 'F': fputs(type_str(st->st_mode), stdout); break;
            case 'a': printf("%o", st->st_mode & 07777); break;
            case 'A': { char m[11]; mode_rwx(st->st_mode, m); fputs(m, stdout); break; }
            case 'U': fputs(uname_of(st->st_uid), stdout); break;
            case 'G': fputs(gname_of(st->st_gid), stdout); break;
            case 'u': printf("%u", st->st_uid); break;
            case 'g': printf("%u", st->st_gid); break;
            case 'i': printf("%llu", (unsigned long long)st->st_ino); break;
            case 'h': printf("%lu", (unsigned long)st->st_nlink); break;
            case 'b': printf("%lld", (long long)st->st_blocks); break;
            case 'B': printf("512"); break;
            case 'Y': printf("%lld", (long long)st->st_mtime); break;
            case 'X': printf("%lld", (long long)st->st_atime); break;
            case 'Z': printf("%lld", (long long)st->st_ctime); break;
            case 'y': tstr(st->st_mtime, tb, sizeof tb); fputs(tb, stdout); break;
            case 'x': tstr(st->st_atime, tb, sizeof tb); fputs(tb, stdout); break;
            case 'z': tstr(st->st_ctime, tb, sizeof tb); fputs(tb, stdout); break;
            case '%': putchar('%'); break;
            default: putchar('%'); putchar(*p); break;
        }
        if (!*p) break;
    }
    putchar('\n');
}

static void verbose(const char *name, struct stat *st) {
    char m[11]; mode_rwx(st->st_mode, m);
    char at[64], mt[64], ct[64];
    tstr(st->st_atime, at, sizeof at); tstr(st->st_mtime, mt, sizeof mt); tstr(st->st_ctime, ct, sizeof ct);
    printf("  File: %s\n", name);
    printf("  Size: %-10lld Blocks: %-8lld IO Block: %-6ld %s\n",
           (long long)st->st_size, (long long)st->st_blocks, (long)st->st_blksize, type_str(st->st_mode));
    printf("Device: %lxh   Inode: %-10llu Links: %lu\n",
           (unsigned long)st->st_dev, (unsigned long long)st->st_ino, (unsigned long)st->st_nlink);
    printf("Access: (%04o/%s)  Uid: (%5u/%8s)   Gid: (%5u/%8s)\n",
           st->st_mode & 07777, m, st->st_uid, uname_of(st->st_uid), st->st_gid, gname_of(st->st_gid));
    printf("Access: %s\nModify: %s\nChange: %s\n", at, mt, ct);
}

int main(int argc, char **argv) {
    int deref = 0; const char *cfmt = NULL;
    const char *files[256]; int nf = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-L")) deref = 1;
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) cfmt = argv[++i];
        else if (!strncmp(argv[i], "--format=", 9)) cfmt = argv[i] + 9;
        else if (argv[i][0] != '-' && nf < 256) files[nf++] = argv[i];
    }
    if (nf == 0) { fprintf(stderr, "usage: stat [-L] [-c FORMAT] FILE...\n"); return 1; }
    int ret = 0;
    for (int i = 0; i < nf; i++) {
        struct stat st;
        if ((deref ? stat : lstat)(files[i], &st) != 0) { fprintf(stderr, "stat: cannot stat '%s'\n", files[i]); ret = 1; continue; }
        if (cfmt) fmt_custom(cfmt, files[i], &st);
        else verbose(files[i], &st);
    }
    return ret;
}
