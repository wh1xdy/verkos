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
#include <fcntl.h>
#include <time.h>

static int verbose, gz;
/* tar restores the archived modification time unless -m says otherwise. */
static int keep_mtime = 1;

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
static int header_ok(const unsigned char *h) {
    long want = from_octal((const char *)h + 148, 8);
    long sum = 0, ssum = 0;
    for (int i = 0; i < 512; i++) {
        int c = (i >= 148 && i < 156) ? ' ' : h[i];      /* the field reads as spaces */
        sum += c;
        ssum += (i >= 148 && i < 156) ? ' ' : (signed char)h[i];
    }
    return sum == want || ssum == want;                  /* some old writers signed it */
}

static int do_extract(FILE *in, int listonly) {
    char h[512];
    char pending[4096] = "";      /* a long name announced by the previous header */
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
        /* Tell a header from garbage. The ustar magic at 257 is NOT the way:
         * archives written in the old pre-POSIX format have zeros there and are
         * perfectly valid — popt still ships one. Every format back to V7 has
         * an octal checksum at 148, computed over the header with that field
         * read as spaces, so check that instead. Without some check, handing
         * tar a file that is not a tar reads its bytes as headers, extracts
         * nothing, and exits 0. */
        if (!header_ok(h)) {
            fprintf(stderr, "tar: %s (bad header checksum)\n",
                    first ? "not a tar archive" : "archive is corrupt");
            return -1;
        }
        first = 0;
        count++;
        char type_c = h[156];
        long esize = from_octal(h + 124, 12);

        /* A tar header holds 100 bytes of name. Longer paths are carried three
         * different ways, and ignoring them does not merely lose files: the
         * next header still has a truncated name, so the contents land at the
         * wrong path. Extracting cmake this way silently lost 465 files and
         * wrote others over each other. */
        if (type_c == 'x' || type_c == 'g') {         /* pax extended header */
            char rec[8192];
            long want = esize < (long)sizeof rec - 1 ? esize : (long)sizeof rec - 1;
            long got = (long)fread(rec, 1, (size_t)want, in);
            rec[got > 0 ? got : 0] = 0;
            for (long skip = esize - got; skip > 0; skip--) fgetc(in);
            for (long pad = (512 - (esize % 512)) % 512; pad > 0; pad--) fgetc(in);
            /* records are "<len> key=value\n"; only the path matters here */
            for (char *r = rec; r < rec + got; ) {
                char *sp = strchr(r, ' ');
                if (!sp) break;
                long rlen = strtol(r, NULL, 10);
                if (rlen <= 0 || r + rlen > rec + got) break;
                if (!strncmp(sp + 1, "path=", 5)) {
                    long vlen = rlen - (sp + 6 - r) - 1;
                    if (vlen > 0 && vlen < (long)sizeof pending) {
                        memcpy(pending, sp + 6, (size_t)vlen);
                        pending[vlen] = 0;
                    }
                }
                r += rlen;
            }
            continue;
        }
        if (type_c == 'L' || type_c == 'K') {          /* GNU long name/linkname */
            char buf2[4096];
            long want = esize < (long)sizeof buf2 - 1 ? esize : (long)sizeof buf2 - 1;
            long got = (long)fread(buf2, 1, (size_t)want, in);
            buf2[got > 0 ? got : 0] = 0;
            for (long skip = esize - got; skip > 0; skip--) fgetc(in);
            for (long pad = (512 - (esize % 512)) % 512; pad > 0; pad--) fgetc(in);
            if (type_c == 'L') snprintf(pending, sizeof pending, "%s", buf2);
            continue;
        }

        char name[512];
        if (pending[0]) { snprintf(name, sizeof name, "%s", pending); pending[0] = 0; }
        else if (!memcmp(h + 257, "ustar", 5) && h[345]) {
            /* POSIX ustar splits a long path into a prefix and the name. */
            char pre[156];
            snprintf(pre, sizeof pre, "%.155s", h + 345);
            snprintf(name, sizeof name, "%s/%.100s", pre, h);
        }
        else snprintf(name, sizeof name, "%.100s", h);
        long size = from_octal(h + 124, 12);
        long mode = from_octal(h + 100, 8);
        /* Restoring the archived modification time is not cosmetic: make and
         * the autotools decide what to rebuild by comparing timestamps, so an
         * extraction that stamps everything "now" makes a source tree look
         * newer than its own generated files and triggers a regeneration that
         * needs tools the system does not have. */
        long mtime = from_octal(h + 136, 12);
        char type = h[156];
        if (listonly) { printf("%s\n", name); }
        if (type == '5' || (name[0] && name[strlen(name)-1] == '/')) {
            if (!listonly) { for (char *s = strchr(name, '/'); s; s = strchr(s + 1, '/')) { *s = 0; mkdir(name, 0755); *s = '/'; } mkdir(name, mode & 07777 ? mode & 07777 : 0755); }
        } else {
            FILE *of = NULL;
            if (!listonly) { for (char *s = strchr(name, '/'); s; s = strchr(s + 1, '/')) { *s = 0; mkdir(name, 0755); *s = '/'; } of = fopen(name, "wb"); if (verbose) fprintf(stderr, "%s\n", name); }
            long rem = size; char buf[512];
            while (rem > 0) { if (fread(buf, 1, 512, in) != 512) break; long w = rem < 512 ? rem : 512; if (of) fwrite(buf, 1, w, of); rem -= 512; }
            if (of) {
                fclose(of);
                chmod(name, mode & 07777);
                if (mtime && keep_mtime) {
                    struct timespec ts[2];
                    ts[0].tv_sec = mtime; ts[0].tv_nsec = 0;
                    ts[1].tv_sec = mtime; ts[1].tv_nsec = 0;
                    utimensat(AT_FDCWD, name, ts, 0);
                }
            }
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
        if (i == 1 && a[0] != '-' && a[0] && strspn(a, "cxtzjJvfpmok") == strlen(a)) {
            static char dashed[16];
            snprintf(dashed, sizeof dashed, "-%.14s", a);
            a = dashed;
        }
        if (a[0] == '-' && a[1] != 'f' && strchr(a, 'f') == NULL && a[1]) {
            for (char *p = a + 1; *p; p++) { if (*p=='c') mode='c'; else if (*p=='x') mode='x'; else if (*p=='t') mode='t'; else if (*p=='z') gz=1; else if (*p=='j') bz=1; else if (*p=='J') xzc=1; else if (*p=='v') verbose=1;
                /* p/o/k: permissions and ownership are already handled the way
                 * these ask for, so they are accepted rather than refused —
                 * GCC installs its headers with "tar xpf -". m means the
                 * opposite of what we now do by default. */
                else if (*p=='p' || *p=='o' || *p=='k') ;
                else if (*p=='m') keep_mtime=0;
                else if (*p=='f') ;
                else { fprintf(stderr, "tar: unknown option -%c\n", *p); return 1; } }
        } else if (a[0] == '-' && strchr(a, 'f')) {
            for (char *p = a + 1; *p; p++) { if (*p=='c') mode='c'; else if (*p=='x') mode='x'; else if (*p=='t') mode='t'; else if (*p=='z') gz=1; else if (*p=='j') bz=1; else if (*p=='J') xzc=1; else if (*p=='v') verbose=1;
                /* p/o/k: permissions and ownership are already handled the way
                 * these ask for, so they are accepted rather than refused —
                 * GCC installs its headers with "tar xpf -". m means the
                 * opposite of what we now do by default. */
                else if (*p=='p' || *p=='o' || *p=='k') ;
                else if (*p=='m') keep_mtime=0;
                else if (*p=='f') ;
                else { fprintf(stderr, "tar: unknown option -%c\n", *p); return 1; } }
            if (i + 1 < argc) arch = argv[++i];
        } else if (a[0] != '-') files[nf++] = a;
    }
    if (!mode) { fprintf(stderr, "usage: tar -c|x|t [z|j|J][v] -f ARCH [FILE...]\n"); return 1; }

    char cmd[1200];
    if (mode == 'c') {
        FILE *o;
        const char *comp = gz ? "gzip" : bz ? "bzip2" : xzc ? "xz" : NULL;
        /* "-" names standard input/output, as it does in every tar. Treating it
         * as a file name made "tar -cf - ." write an archive into a file called
         * "-", so the pipeline GCC installs its headers through moved nothing. */
        if (arch && !strcmp(arch, "-")) arch = NULL;
        if (comp) { snprintf(cmd, sizeof cmd, "%s > '%s'", comp, arch ? arch : "/dev/stdout"); o = popen(cmd, "w"); }
        else o = arch ? fopen(arch, "wb") : stdout;
        if (!o) { fprintf(stderr, "tar: cannot write archive\n"); return 1; }
        for (int i = 0; i < nf; i++) add_path(o, files[i]);
        char zeros[1024]; memset(zeros, 0, 1024); fwrite(zeros, 1, 1024, o);
        if (gz) pclose(o); else if (arch) fclose(o);
    } else {
        FILE *in;
        if (arch && !strcmp(arch, "-")) arch = NULL;      /* "-" is standard input */
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
