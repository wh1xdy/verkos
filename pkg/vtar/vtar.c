/* vtar — VerkOS' own tar (ustar).
 *
 * Create/extract/list POSIX ustar archives; -z pipes through our gzip. Installed
 * as `tar`.
 *
 *   tar -c[z][v] -f ARCH FILE...     create
 *   tar -x[z][v] -f ARCH             extract
 *   tar -t[z][v] -f ARCH             list
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

static int verbose, gz;

/* ---- create ------------------------------------------------------------- */
static void octal(char *f, int width, long v) { char t[32]; snprintf(t, sizeof t, "%0*lo", width - 1, v); memcpy(f, t, width - 1); }
static void put_header(FILE *o, const char *name, struct stat *st, char type) {
    char h[512]; memset(h, 0, 512);
    char nm[255]; snprintf(nm, sizeof nm, "%s%s", name, (type == '5' && name[strlen(name)-1] != '/') ? "/" : "");
    snprintf(h, 100, "%.99s", nm);
    octal(h + 100, 8, st->st_mode & 07777);
    octal(h + 108, 8, (long)st->st_uid);
    octal(h + 116, 8, (long)st->st_gid);
    octal(h + 124, 12, type == '5' ? 0 : (long)st->st_size);
    octal(h + 136, 12, (long)st->st_mtime);
    memcpy(h + 148, "        ", 8);           /* checksum field = spaces while summing */
    h[156] = type;
    memcpy(h + 257, "ustar", 5); memcpy(h + 263, "00", 2);
    unsigned sum = 0; for (int i = 0; i < 512; i++) sum += (unsigned char)h[i];
    snprintf(h + 148, 8, "%06o", sum); h[154] = 0; h[155] = ' ';
    fwrite(h, 1, 512, o);
}
static void add_path(FILE *o, const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) { fprintf(stderr, "tar: %s: not found\n", path); return; }
    if (S_ISDIR(st.st_mode)) {
        if (verbose) fprintf(stderr, "%s/\n", path);
        put_header(o, path, &st, '5');
        DIR *d = opendir(path); if (!d) return;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char child[4096]; snprintf(child, sizeof child, "%s/%s", path, e->d_name);
            add_path(o, child);
        }
        closedir(d);
    } else if (S_ISREG(st.st_mode)) {
        if (verbose) fprintf(stderr, "%s\n", path);
        put_header(o, path, &st, '0');
        FILE *f = fopen(path, "rb"); if (!f) return;
        char buf[512]; size_t r; long written = 0;
        while ((r = fread(buf, 1, 512, f)) > 0) { if (r < 512) memset(buf + r, 0, 512 - r); fwrite(buf, 1, 512, o); written += 512; }
        fclose(f); (void)written;
    }
}

/* ---- extract / list ----------------------------------------------------- */
static long from_octal(const char *p, int n) { long v = 0; for (int i = 0; i < n && p[i]; i++) { if (p[i] < '0' || p[i] > '7') continue; v = v * 8 + (p[i] - '0'); } return v; }
static int do_extract(FILE *in, int listonly) {
    char h[512];
    for (;;) {
        if (fread(h, 1, 512, in) != 512) break;
        int zero = 1; for (int i = 0; i < 512; i++) if (h[i]) { zero = 0; break; }
        if (zero) break;
        char name[256]; snprintf(name, sizeof name, "%.100s", h);
        long size = from_octal(h + 124, 12);
        long mode = from_octal(h + 100, 8);
        char type = h[156];
        if (listonly) { printf("%s\n", name); }
        if (type == '5' || (name[0] && name[strlen(name)-1] == '/')) {
            if (!listonly) { for (char *s = strchr(name, '/'); s; s = strchr(s + 1, '/')) { *s = 0; mkdir(name, 0755); *s = '/'; } mkdir(name, mode & 07777 ? mode & 07777 : 0755); }
        } else {
            FILE *of = NULL;
            if (!listonly) { for (char *s = strchr(name, '/'); s; s = strchr(s + 1, '/')) { *s = 0; mkdir(name, 0755); *s = '/'; } of = fopen(name, "wb"); if (verbose) fprintf(stderr, "%s\n", name); }
            long rem = size; char buf[512];
            while (rem > 0) { if (fread(buf, 1, 512, in) != 512) break; long w = rem < 512 ? rem : 512; if (of) fwrite(buf, 1, w, of); rem -= 512; }
            if (of) { fclose(of); chmod(name, mode & 07777); }
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    int mode = 0; const char *arch = NULL; char *files[512]; int nf = 0;   /* mode: c/x/t */
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        /* tar's traditional form has no dash on the first argument — "tar xzf
         * foo.tar.gz" is how most people still write it. Treat a leading
         * operand made only of mode letters as if it had one. */
        if (i == 1 && a[0] != '-' && a[0] && strspn(a, "cxtzvf") == strlen(a)) {
            static char dashed[16];
            snprintf(dashed, sizeof dashed, "-%.14s", a);
            a = dashed;
        }
        if (a[0] == '-' && a[1] != 'f' && strchr(a, 'f') == NULL && a[1]) {
            for (char *p = a + 1; *p; p++) { if (*p=='c') mode='c'; else if (*p=='x') mode='x'; else if (*p=='t') mode='t'; else if (*p=='z') gz=1; else if (*p=='v') verbose=1; }
        } else if (a[0] == '-' && strchr(a, 'f')) {
            for (char *p = a + 1; *p; p++) { if (*p=='c') mode='c'; else if (*p=='x') mode='x'; else if (*p=='t') mode='t'; else if (*p=='z') gz=1; else if (*p=='v') verbose=1; }
            if (i + 1 < argc) arch = argv[++i];
        } else if (a[0] != '-') files[nf++] = a;
    }
    if (!mode) { fprintf(stderr, "usage: tar -c|x|t [z][v] -f ARCH [FILE...]\n"); return 1; }

    char cmd[1200];
    if (mode == 'c') {
        FILE *o;
        if (gz) { snprintf(cmd, sizeof cmd, "gzip > '%s'", arch ? arch : "/dev/stdout"); o = popen(cmd, "w"); }
        else o = arch ? fopen(arch, "wb") : stdout;
        if (!o) { fprintf(stderr, "tar: cannot write archive\n"); return 1; }
        for (int i = 0; i < nf; i++) add_path(o, files[i]);
        char zeros[1024]; memset(zeros, 0, 1024); fwrite(zeros, 1, 1024, o);
        if (gz) pclose(o); else if (arch) fclose(o);
    } else {
        FILE *in;
        if (gz) { snprintf(cmd, sizeof cmd, "gunzip < '%s'", arch ? arch : "/dev/stdin"); in = popen(cmd, "r"); }
        else in = arch ? fopen(arch, "rb") : stdin;
        if (!in) { fprintf(stderr, "tar: cannot read archive\n"); return 1; }
        do_extract(in, mode == 't');
        if (gz) pclose(in); else if (arch) fclose(in);
    }
    return 0;
}
