/* verkbox — VerkOS's own multi-call core utilities.
 *
 * One small, self-contained binary that implements several classic userland
 * tools as "applets" (the BusyBox model): symlink each applet name to this
 * binary and it dispatches on argv[0]. Also callable as `verkbox <applet> ...`.
 *
 * This is the first piece of VerkOS's own userland — the start of replacing the
 * GNU coreutils with tools that are ours, in the same spirit as vpk: plain C,
 * no dependencies, readable end to end. Applets aim to match GNU behaviour for
 * common usage (verified by differential tests against coreutils).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <pwd.h>
#include <errno.h>

#define VERKBOX_VERSION "0.1"

/* ------------------------------------------------------------- helpers ---- */
/* copy an open stream to stdout; returns 0 on success */
static int slurp(FILE *f) {
    char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        if (fwrite(buf, 1, n, stdout) != n) return 1;
    return ferror(f) ? 1 : 0;
}

/* ------------------------------------------------------------- applets ---- */
static int a_true(int c, char **v)  { (void)c; (void)v; return 0; }
static int a_false(int c, char **v) { (void)c; (void)v; return 1; }

static int a_pwd(int c, char **v) {
    (void)c; (void)v;
    char b[4096];
    if (!getcwd(b, sizeof b)) { perror("pwd"); return 1; }
    puts(b);
    return 0;
}

static int a_whoami(int c, char **v) {
    (void)c; (void)v;
    struct passwd *p = getpwuid(geteuid());
    if (!p) { fprintf(stderr, "whoami: cannot find name for uid %u\n", geteuid()); return 1; }
    puts(p->pw_name);
    return 0;
}

static int a_nproc(int c, char **v) {
    (void)c; (void)v;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    printf("%ld\n", n);
    return 0;
}

/* echo [-neE] [args...] */
static int a_echo(int c, char **v) {
    int nflag = 0, eflag = 0, i = 1;
    for (; i < c; i++) {
        if (v[i][0] != '-' || !v[i][1]) break;
        int ok = 1;
        for (char *p = v[i] + 1; *p; p++)
            if (*p != 'n' && *p != 'e' && *p != 'E') { ok = 0; break; }
        if (!ok) break;
        for (char *p = v[i] + 1; *p; p++) {
            if (*p == 'n') nflag = 1;
            else if (*p == 'e') eflag = 1;
            else if (*p == 'E') eflag = 0;
        }
    }
    for (int j = i; j < c; j++) {
        if (j > i) putchar(' ');
        if (!eflag) { fputs(v[j], stdout); continue; }
        for (char *p = v[j]; *p; p++) {
            if (*p != '\\') { putchar(*p); continue; }
            p++;
            switch (*p) {
                case 'n': putchar('\n'); break;
                case 't': putchar('\t'); break;
                case 'r': putchar('\r'); break;
                case '\\': putchar('\\'); break;
                case 'a': putchar('\a'); break;
                case 'b': putchar('\b'); break;
                case 'f': putchar('\f'); break;
                case 'v': putchar('\v'); break;
                case 'e': putchar('\033'); break;
                case 'c': return 0;                 /* stop, no trailing newline */
                case '0': {                         /* \0NNN octal */
                    int val = 0, k = 0; p++;
                    while (k < 3 && *p >= '0' && *p <= '7') { val = val*8 + (*p-'0'); p++; k++; }
                    p--; putchar(val); break;
                }
                case '\0': putchar('\\'); p--; break;
                default: putchar('\\'); putchar(*p); break;
            }
        }
    }
    if (!nflag) putchar('\n');
    return 0;
}

/* cat [files...] ; '-' or no args = stdin */
static int a_cat(int c, char **v) {
    int rc = 0;
    if (c < 2) return slurp(stdin);
    for (int i = 1; i < c; i++) {
        if (!strcmp(v[i], "-")) { rc |= slurp(stdin); continue; }
        FILE *f = fopen(v[i], "rb");
        if (!f) { fprintf(stderr, "cat: %s: %s\n", v[i], strerror(errno)); rc = 1; continue; }
        rc |= slurp(f);
        fclose(f);
    }
    return rc;
}

/* yes [string...] — repeat forever */
static int a_yes(int c, char **v) {
    for (;;) {
        if (c < 2) { if (puts("y") == EOF) return 0; continue; }
        for (int i = 1; i < c; i++) {
            if (fputs(v[i], stdout) == EOF) return 0;
            putchar(i + 1 < c ? ' ' : '\n');
        }
    }
}

/* basename path [suffix] */
static int a_basename(int c, char **v) {
    if (c < 2) { fprintf(stderr, "basename: missing operand\n"); return 1; }
    char *s = v[1]; size_t len = strlen(s);
    while (len > 1 && s[len-1] == '/') s[--len] = 0;      /* strip trailing / */
    char *base = strrchr(s, '/'); base = base ? base + 1 : s;
    if (c > 2) {                                          /* strip suffix */
        size_t bl = strlen(base), sl = strlen(v[2]);
        if (sl && sl < bl && !strcmp(base + bl - sl, v[2])) base[bl - sl] = 0;
    }
    puts(*base ? base : "/");
    return 0;
}

/* dirname path */
static int a_dirname(int c, char **v) {
    if (c < 2) { fprintf(stderr, "dirname: missing operand\n"); return 1; }
    char *s = v[1]; size_t len = strlen(s);
    while (len > 1 && s[len-1] == '/') s[--len] = 0;
    char *slash = strrchr(s, '/');
    if (!slash) { puts("."); return 0; }
    if (slash == s) { puts("/"); return 0; }
    while (slash > s && *(slash-1) == '/') slash--;       /* collapse //// */
    *slash = 0;
    puts(s);
    return 0;
}

/* head [-n N] [files...] — first N lines (default 10) */
static int a_head(int c, char **v) {
    long nlines = 10; int i = 1;
    for (; i < c; i++) {
        if (!strcmp(v[i], "-n") && i+1 < c) { nlines = atol(v[++i]); }
        else if (v[i][0] == '-' && v[i][1] == 'n' && v[i][2]) { nlines = atol(v[i]+2); }
        else if (v[i][0] == '-' && isdigit((unsigned char)v[i][1])) { nlines = atol(v[i]+1); }
        else break;
    }
    int nfiles = c - i, rc = 0, first = 1;
    if (nfiles <= 0) {                                   /* stdin */
        long k = 0; int ch;
        while (k < nlines && (ch = getchar()) != EOF) { putchar(ch); if (ch == '\n') k++; }
        return 0;
    }
    for (; i < c; i++) {
        FILE *f = fopen(v[i], "rb");
        if (!f) { fprintf(stderr, "head: %s: %s\n", v[i], strerror(errno)); rc = 1; continue; }
        if (nfiles > 1) { printf("%s==> %s <==\n", first ? "" : "\n", v[i]); first = 0; }
        long k = 0; int ch;
        while (k < nlines && (ch = getc(f)) != EOF) { putchar(ch); if (ch == '\n') k++; }
        fclose(f);
    }
    return rc;
}

/* wc [-lwc] [files...] — GNU-compatible column formatting (each count is
 * right-justified to a shared field width = digits of the largest count). */
static int digits(long x) { int d = 1; if (x < 0) x = -x; while (x >= 10) { x /= 10; d++; } return d; }
static int a_wc(int c, char **v) {
    int wl = 0, ww = 0, wc = 0, i = 1;
    for (; i < c; i++) {
        if (v[i][0] != '-' || !v[i][1]) break;
        int ok = 1; for (char *p = v[i]+1; *p; p++) if (*p!='l'&&*p!='w'&&*p!='c') { ok = 0; break; }
        if (!ok) break;
        for (char *p = v[i]+1; *p; p++) { if(*p=='l')wl=1; else if(*p=='w')ww=1; else if(*p=='c')wc=1; }
    }
    if (!wl && !ww && !wc) { wl = ww = wc = 1; }          /* default: all */
    int nfiles = c - i, rc = 0, n = nfiles > 0 ? nfiles : 1;
    long *La = calloc(n, sizeof(long)), *Wa = calloc(n, sizeof(long)), *Ca = calloc(n, sizeof(long));
    const char **lab = calloc(n, sizeof(char*));
    int *ok = calloc(n, sizeof(int));
    if (!La||!Wa||!Ca||!lab||!ok) { free(La);free(Wa);free(Ca);free(lab);free(ok); return 1; }
    long tl = 0, tw = 0, tc = 0, maxv = 0, maxall = 0;
    for (int k = 0; k < n; k++) {
        FILE *f = stdin;
        if (nfiles > 0) {
            f = fopen(v[i+k], "rb");
            if (!f) { fprintf(stderr, "wc: %s: %s\n", v[i+k], strerror(errno)); rc = 1; continue; }
            lab[k] = v[i+k];
        }
        long L = 0, W = 0, C = 0; int inword = 0, ch;
        while ((ch = getc(f)) != EOF) {
            C++;
            if (ch == '\n') L++;
            if (isspace(ch)) inword = 0; else if (!inword) { inword = 1; W++; }
        }
        if (nfiles > 0) fclose(f);
        La[k]=L; Wa[k]=W; Ca[k]=C; ok[k]=1;
        tl+=L; tw+=W; tc+=C;
        if (wl && L>maxv) maxv=L;
        if (ww && W>maxv) maxv=W;
        if (wc && C>maxv) maxv=C;
        if (L>maxall) maxall=L;
        if (W>maxall) maxall=W;
        if (C>maxall) maxall=C;
    }
    if (nfiles > 1) {
        if (wl&&tl>maxv) maxv=tl;
        if (ww&&tw>maxv) maxv=tw;
        if (wc&&tc>maxv) maxv=tc;
        if (tl>maxall) maxall=tl;
        if (tw>maxall) maxall=tw;
        if (tc>maxall) maxall=tc;
    }
    /* GNU: a single column is sized to its own value; multiple columns share the
     * width of the largest of all three counts (so columns align even when a
     * count like bytes isn't printed). */
    int nflags = wl + ww + wc;
    int width = digits(nflags == 1 ? maxv : maxall);
    for (int k = 0; k < n; k++) {
        if (nfiles > 0 && !ok[k]) continue;
        int first = 1;
        if (wl) { if(!first)putchar(' '); printf("%*ld", width, La[k]); first=0; }
        if (ww) { if(!first)putchar(' '); printf("%*ld", width, Wa[k]); first=0; }
        if (wc) { if(!first)putchar(' '); printf("%*ld", width, Ca[k]); first=0; }
        if (lab[k]) printf(" %s", lab[k]);
        putchar('\n');
    }
    if (nfiles > 1) {
        int first = 1;
        if (wl) { if(!first)putchar(' '); printf("%*ld", width, tl); first=0; }
        if (ww) { if(!first)putchar(' '); printf("%*ld", width, tw); first=0; }
        if (wc) { if(!first)putchar(' '); printf("%*ld", width, tc); first=0; }
        printf(" total\n");
    }
    free(La);free(Wa);free(Ca);free(lab);free(ok);
    return rc;
}

/* seq [FIRST [STEP]] LAST — integers */
static int a_seq(int c, char **v) {
    long first = 1, step = 1, last;
    if (c == 2) { last = atol(v[1]); }
    else if (c == 3) { first = atol(v[1]); last = atol(v[2]); }
    else if (c == 4) { first = atol(v[1]); step = atol(v[2]); last = atol(v[3]); }
    else { fprintf(stderr, "usage: seq [first [step]] last\n"); return 1; }
    if (step == 0) { fprintf(stderr, "seq: step cannot be 0\n"); return 1; }
    if (step > 0) for (long i = first; i <= last; i += step) printf("%ld\n", i);
    else          for (long i = first; i >= last; i += step) printf("%ld\n", i);
    return 0;
}

/* ------------------------------------------------------------ dispatch ---- */
struct applet { const char *name; int (*fn)(int, char **); };
static const struct applet applets[] = {
    {"true",a_true},{"false",a_false},{"echo",a_echo},{"cat",a_cat},{"pwd",a_pwd},
    {"whoami",a_whoami},{"nproc",a_nproc},{"yes",a_yes},{"basename",a_basename},
    {"dirname",a_dirname},{"head",a_head},{"wc",a_wc},{"seq",a_seq},{NULL,NULL}
};

static int list_applets(void) {
    printf("verkbox %s — VerkOS core utilities\napplets:", VERKBOX_VERSION);
    for (const struct applet *a = applets; a->name; a++) printf(" %s", a->name);
    putchar('\n');
    return 0;
}

int main(int argc, char **argv) {
    const char *b = strrchr(argv[0], '/'); b = b ? b + 1 : argv[0];
    if (!strcmp(b, "verkbox")) {
        if (argc < 2) return list_applets();
        if (!strcmp(argv[1], "--list")) return list_applets();
        b = argv[1]; argc--; argv++;                      /* shift to applet */
    }
    for (const struct applet *a = applets; a->name; a++)
        if (!strcmp(b, a->name)) return a->fn(argc, argv);
    fprintf(stderr, "verkbox: unknown applet '%s'\n", b);
    return 127;
}
