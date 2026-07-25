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
/* Returns how many entries were handled, or -1 if this was not a tar archive. */
static int do_extract(FILE *in, int listonly) {
    char h[512];
    int count = 0, first = 1;
    for (;;) {
        size_t got = fread(h, 1, 512, in);
        if (got == 0) break;                    /* clean end of input */
        /* Anything that is not a whole block is not a tar archive. Breaking out
         * silently here is how "tar -xf" on a short non-archive used to extract
         * nothing and still exit 0. */
        if (got != 512) {
            fprintf(stderr, "tar: not a tar archive (%zu trailing bytes)\n", got);
            return -1;
        }
        int zero = 1; for (int i = 0; i < 512; i++) if (h[i]) { zero = 0; break; }
        if (zero) break;
        /* A ustar archive says so at offset 257. Without this check, handing
         * tar something that is not a tar — a still-compressed file, say —
         * reads compressed bytes as headers, extracts nothing, and exits 0.
         * A silent success is worse than any error message. */
        if (first && memcmp(h + 257, "ustar", 5)) {
            fprintf(stderr, "tar: not a tar archive (no ustar signature)\n");
            return -1;
        }
        first = 0;
        count++;
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
    return count;
}

/* What an archive was compressed with, read from its first bytes. GNU tar has
 * auto-detected for decades and every caller assumes it; requiring -z meant
 * "tar -xf foo.tar.xz" silently did nothing. */
static const char *sniff(const char *path) {
    if (!path) return NULL;                     /* stdin cannot be peeked; use a flag */
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char m[6] = {0};
    size_t n = fread(m, 1, sizeof m, f);
    fclose(f);
    if (n >= 2 && m[0] == 0x1f && m[1] == 0x8b)              return "gunzip";
    if (n >= 6 && !memcmp(m, "\xfd" "7zXZ\0", 6))            return "xz -dc";
    if (n >= 3 && !memcmp(m, "BZh", 3))                      return "bzip2 -dc";
    return NULL;
}

int main(int argc, char **argv) {
    int mode = 0, bz = 0, xzc = 0; const char *arch = NULL; char *files[512]; int nf = 0;   /* mode: c/x/t */
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        /* tar's traditional form has no dash on the first argument — "tar xzf
         * foo.tar.gz" is how most people still write it. Treat a leading
         * operand made only of mode letters as if it had one. */
        if (i == 1 && a[0] != '-' && a[0] && strspn(a, "cxtzjJvf") == strlen(a)) {
            static char dashed[16];
            snprintf(dashed, sizeof dashed, "-%.14s", a);
            a = dashed;
        }
        if (a[0] == '-' && a[1] != 'f' && strchr(a, 'f') == NULL && a[1]) {
            for (char *p = a + 1; *p; p++) { if (*p=='c') mode='c'; else if (*p=='x') mode='x'; else if (*p=='t') mode='t'; else if (*p=='z') gz=1; else if (*p=='j') bz=1; else if (*p=='J') xzc=1; else if (*p=='v') verbose=1; }
        } else if (a[0] == '-' && strchr(a, 'f')) {
            for (char *p = a + 1; *p; p++) { if (*p=='c') mode='c'; else if (*p=='x') mode='x'; else if (*p=='t') mode='t'; else if (*p=='z') gz=1; else if (*p=='j') bz=1; else if (*p=='J') xzc=1; else if (*p=='v') verbose=1; }
            if (i + 1 < argc) arch = argv[++i];
        } else if (a[0] != '-') files[nf++] = a;
    }
    if (!mode) { fprintf(stderr, "usage: tar -c|x|t [z|j|J][v] -f ARCH [FILE...]\n"); return 1; }

    char cmd[1200];
    if (mode == 'c') {
        FILE *o;
        const char *comp = gz ? "gzip" : bz ? "bzip2" : xzc ? "xz" : NULL;
        if (comp) { snprintf(cmd, sizeof cmd, "%s > '%s'", comp, arch ? arch : "/dev/stdout"); o = popen(cmd, "w"); }
        else o = arch ? fopen(arch, "wb") : stdout;
        if (!o) { fprintf(stderr, "tar: cannot write archive\n"); return 1; }
        for (int i = 0; i < nf; i++) add_path(o, files[i]);
        char zeros[1024]; memset(zeros, 0, 1024); fwrite(zeros, 1, 1024, o);
        if (gz) pclose(o); else if (arch) fclose(o);
    } else {
        FILE *in;
        const char *dec = gz ? "gunzip" : bz ? "bzip2 -dc" : xzc ? "xz -dc" : sniff(arch);
        if (dec) { snprintf(cmd, sizeof cmd, "%s < '%s'", dec, arch ? arch : "/dev/stdin"); in = popen(cmd, "r"); }
        else in = arch ? fopen(arch, "rb") : stdin;
        if (!in) { fprintf(stderr, "tar: cannot read archive\n"); return 1; }
        int n = do_extract(in, mode == 't');
        int prc = dec ? pclose(in) : (arch ? fclose(in) : 0);
        if (n < 0) return 1;
        /* Extraction stops at the end-of-archive marker without draining the
         * rest of the pipe, so the decompressor normally dies of SIGPIPE. That
         * is only a real failure if nothing came out of it. */
        if (dec && prc != 0 && n == 0) {
            fprintf(stderr, "tar: %s produced nothing from %s\n", dec, arch ? arch : "stdin");
            return 1;
        }
    }
    return 0;
}
