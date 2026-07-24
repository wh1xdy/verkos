/* vpager — VerkOS' own pager (a small less/more).
 *
 * VerkOS' last non-native base tool was GNU less; vpager replaces it so the
 * whole userland is ours. Installed as less/more/pg. Reads a file or a pipe
 * (keys then come from /dev/tty), pages a terminal, and falls back to plain
 * cat when stdout is not a tty.
 *
 * Keys:  SPACE/f/^F/PgDn forward screen · b/^B/PgUp back · j/Enter/Down line
 *        k/Up line back · d/u half screen · g/< top · G/> end · Left/Right pan
 *        /pat search fwd · ?pat search back · n/N repeat · :n/:p next/prev file
 *        -N line numbers · Left/Right pan · h help · q quit
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <regex.h>

enum { K_UP = 256, K_DOWN, K_LEFT, K_RIGHT, K_PGUP, K_PGDN, K_HOME, K_END, K_DEL };

static struct {
    char **row; int nrows, cap;      /* content lines (no trailing newline) */
    int top, hoff;                   /* first visible line, horizontal offset */
    int rows_scr, cols_scr;          /* text rows (screen - status), columns */
    int tin;                         /* tty fd for keystrokes */
    struct termios orig; int raw;
    char name[256];                  /* current source name */
    char **files; int nfiles, cur;   /* file list + index (-1 = stdin) */
    regex_t re; int have_re, re_dir; char pat[256];
    int numbers;                     /* show line numbers */
    char msg[256];
} P;

/* ---- append buffer (batch one screen into a single write) --------------- */
struct ab { char *b; int len, cap; };
static void ab_add(struct ab *a, const char *s, int n) {
    if (a->len + n > a->cap) { a->cap = (a->len + n) * 2 + 256; a->b = realloc(a->b, a->cap); }
    memcpy(a->b + a->len, s, n); a->len += n;
}
static void ab_str(struct ab *a, const char *s) { ab_add(a, s, strlen(s)); }

/* ---- content model ------------------------------------------------------ */
static void free_rows(void) {
    for (int i = 0; i < P.nrows; i++) free(P.row[i]);
    free(P.row); P.row = NULL; P.nrows = P.cap = 0;
}
static void add_row(const char *s, int len) {
    if (P.nrows >= P.cap) { P.cap = P.cap ? P.cap * 2 : 256; P.row = realloc(P.row, P.cap * sizeof *P.row); }
    char *r = malloc(len + 1); memcpy(r, s, len); r[len] = 0; P.row[P.nrows++] = r;
}
static void load_stream(FILE *f, const char *name) {
    free_rows();
    snprintf(P.name, sizeof P.name, "%s", name);
    char *line = NULL; size_t cap = 0; ssize_t n;
    while ((n = getline(&line, &cap, f)) != -1) {
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) n--;
        /* expand tabs to 8-col stops so columns line up like a real pager */
        char buf[8192]; int p = 0, col = 0;
        for (ssize_t i = 0; i < n && p < (int)sizeof buf - 8; i++) {
            if (line[i] == '\t') { int t = 8 - (col % 8); while (t-- && p < (int)sizeof buf - 1) buf[p++] = ' ', col++; }
            else { buf[p++] = line[i]; col++; }
        }
        add_row(buf, p);
    }
    free(line);
    P.top = P.hoff = 0;
}
static int load_file(const char *fn) {
    FILE *f = fopen(fn, "r");
    if (!f) { snprintf(P.msg, sizeof P.msg, "vpager: %s: cannot open", fn); return -1; }
    load_stream(f, fn); fclose(f); return 0;
}

/* ---- terminal ----------------------------------------------------------- */
static void restore(void) {
    if (P.raw) tcsetattr(P.tin, TCSAFLUSH, &P.orig);
    if (isatty(STDOUT_FILENO)) { char q[] = "\033[2J\033[H"; ssize_t w = write(STDOUT_FILENO, q, sizeof q - 1); (void)w; }
}
static void raw_on(void) {
    if (tcgetattr(P.tin, &P.orig) < 0) return;
    struct termios r = P.orig;
    r.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    r.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    r.c_cc[VMIN] = 1; r.c_cc[VTIME] = 0;
    if (tcsetattr(P.tin, TCSAFLUSH, &r) == 0) P.raw = 1;
}
static void get_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row) { P.rows_scr = ws.ws_row - 1; P.cols_scr = ws.ws_col; }
    else { P.rows_scr = 23; P.cols_scr = 80; }
    if (P.rows_scr < 1) P.rows_scr = 1;
}
static int getkey(void) {
    unsigned char c; if (read(P.tin, &c, 1) != 1) return -1;
    if (c != 27) return c;
    unsigned char s[2];
    if (read(P.tin, &s[0], 1) != 1) return 27;
    if (s[0] != '[' && s[0] != 'O') return 27;
    if (read(P.tin, &s[1], 1) != 1) return 27;
    if (s[1] >= '0' && s[1] <= '9') {           /* ESC [ n ~ */
        unsigned char t; if (read(P.tin, &t, 1) != 1) return 27;
        switch (s[1]) { case '5': return K_PGUP; case '6': return K_PGDN;
                        case '1': case '7': return K_HOME; case '4': case '8': return K_END;
                        case '3': return K_DEL; }
        return 27;
    }
    switch (s[1]) { case 'A': return K_UP; case 'B': return K_DOWN; case 'C': return K_RIGHT;
                    case 'D': return K_LEFT; case 'H': return K_HOME; case 'F': return K_END; }
    return 27;
}

/* ---- geometry ----------------------------------------------------------- */
static int maxtop(void) { return P.nrows > P.rows_scr ? P.nrows - P.rows_scr : 0; }
static void clamp(void) {
    if (P.top > maxtop()) P.top = maxtop();
    if (P.top < 0) P.top = 0;
    if (P.hoff < 0) P.hoff = 0;
}

/* ---- rendering ---------------------------------------------------------- */
static void draw(void) {
    struct ab a = {0};
    ab_str(&a, "\033[H");
    int gutter = P.numbers ? snprintf((char[16]){0}, 16, "%d", P.nrows) + 1 : 0;
    for (int y = 0; y < P.rows_scr; y++) {
        ab_str(&a, "\033[K");
        int fr = P.top + y;
        if (fr < P.nrows) {
            char num[16] = "";
            if (P.numbers) { snprintf(num, sizeof num, "%*d ", gutter - 1, fr + 1); ab_str(&a, num); }
            const char *line = P.row[fr];
            int llen = strlen(line);
            int avail = P.cols_scr - gutter; if (avail < 0) avail = 0;
            int start = P.hoff; if (start > llen) start = llen;
            int vis = llen - start; if (vis > avail) vis = avail;
            /* highlight regex matches inside the visible slice */
            if (P.have_re && vis > 0) {
                int base = start, printed = 0;
                regmatch_t m;
                const char *sp = line + base;
                int off = 0, remain = vis;
                while (remain > 0 && regexec(&P.re, sp + off, 1, &m, off ? REG_NOTBOL : 0) == 0) {
                    int ms = off + m.rm_so, me = off + m.rm_eo;
                    if (m.rm_so == m.rm_eo) { if (off + 1 > vis) break; off++; continue; }
                    if (ms >= vis) break;
                    if (me > vis) me = vis;
                    ab_add(&a, sp + printed, ms - printed);
                    ab_str(&a, "\033[7m"); ab_add(&a, sp + ms, me - ms); ab_str(&a, "\033[27m");
                    printed = me; off = me; remain = vis - off;
                }
                ab_add(&a, sp + printed, vis - printed);
            } else if (vis > 0) {
                ab_add(&a, line + start, vis);
            }
        } else {
            ab_str(&a, "\033[90m~\033[39m");
        }
        ab_str(&a, "\r\n");
    }
    /* status line */
    ab_str(&a, "\033[K\033[7m");
    char st[512]; int atend = (P.top >= maxtop());
    int botline = P.top + P.rows_scr; if (botline > P.nrows) botline = P.nrows;
    int pct = P.nrows ? (int)((long)botline * 100 / P.nrows) : 100;
    char pos[128];
    if (P.nrows == 0) snprintf(pos, sizeof pos, "(empty)");
    else if (atend) snprintf(pos, sizeof pos, "lines %d-%d/%d (END)", P.top + 1, botline, P.nrows);
    else snprintf(pos, sizeof pos, "lines %d-%d/%d %d%%", P.top + 1, botline, P.nrows, pct);
    const char *nm = P.name[0] ? P.name : "(stdin)";
    char fileinfo[300];
    if (P.nfiles > 1) snprintf(fileinfo, sizeof fileinfo, "%s (file %d/%d)", nm, P.cur + 1, P.nfiles);
    else snprintf(fileinfo, sizeof fileinfo, "%s", nm);
    int n = snprintf(st, sizeof st, " %s  %s", fileinfo, pos);
    if (P.msg[0]) n = snprintf(st, sizeof st, " %s", P.msg);
    if (n > P.cols_scr) n = P.cols_scr;
    ab_add(&a, st, n);
    for (int i = n; i < P.cols_scr; i++) ab_str(&a, " ");
    ab_str(&a, "\033[m");
    ssize_t w = write(STDOUT_FILENO, a.b, a.len); (void)w;
    free(a.b);
}

/* ---- bottom-line prompt (reads a string in raw mode) -------------------- */
static int prompt(const char *ps, char *out, int outsz) {
    int len = 0; out[0] = 0;
    for (;;) {
        char line[600]; int n = snprintf(line, sizeof line, "\033[%d;1H\033[K\033[7m%s%s\033[m", P.rows_scr + 1, ps, out);
        ssize_t w = write(STDOUT_FILENO, line, n); (void)w;
        int c = getkey();
        if (c == '\r' || c == '\n') return len;
        if (c == 27) { out[0] = 0; return -1; }
        if (c == 127 || c == 8 || c == K_DEL) { if (len > 0) out[--len] = 0; else return -1; continue; }
        if (c >= 32 && c < 127 && len < outsz - 1) { out[len++] = c; out[len] = 0; }
    }
}

/* ---- search ------------------------------------------------------------- */
static void set_search(const char *pat, int dir) {
    if (P.have_re) { regfree(&P.re); P.have_re = 0; }
    snprintf(P.pat, sizeof P.pat, "%s", pat); P.re_dir = dir;
    int flags = REG_EXTENDED;
    int hasupper = 0; for (const char *q = pat; *q; q++) if (isupper((unsigned char)*q)) hasupper = 1;
    if (!hasupper) flags |= REG_ICASE;                 /* smart-case, like less */
    if (regcomp(&P.re, pat, flags) != 0) {
        snprintf(P.msg, sizeof P.msg, "bad pattern: %.230s", pat); return;
    }
    P.have_re = 1;
}
static void do_search(int dir, int fromcur) {
    if (!P.have_re) { snprintf(P.msg, sizeof P.msg, "no previous search"); return; }
    int start = fromcur ? P.top + dir : P.top;
    for (int i = start; i >= 0 && i < P.nrows; i += dir) {
        if (regexec(&P.re, P.row[i], 0, NULL, 0) == 0) { P.top = i; clamp(); return; }
    }
    snprintf(P.msg, sizeof P.msg, "pattern not found: %.230s", P.pat);
}

/* ---- file switching ----------------------------------------------------- */
static void open_index(int i) {
    if (i < 0 || i >= P.nfiles) return;
    P.cur = i;
    if (load_file(P.files[i]) == 0) { P.top = P.hoff = 0; }
}

/* ---- non-interactive fallback: just cat --------------------------------- */
static void cat_out(void) {
    for (int i = 0; i < P.nrows; i++) { printf("%s\n", P.row[i]); }
}

static void usage(void) {
    printf("Usage: vpager [-N] [FILE...]\n"
           "A pager (installed as less/more/pg). With no FILE, reads stdin.\n"
           "  -N            show line numbers\n"
           "Keys: SPACE/f/b page, j/k line, d/u half, g/G top/end, /?nN search,\n"
           "      :n/:p next/prev file, Left/Right pan, h help, q quit\n");
}

int main(int argc, char **argv) {
    int ai = 1;
    for (; ai < argc; ai++) {
        if (!strcmp(argv[ai], "--")) { ai++; break; }
        if (!strcmp(argv[ai], "--help")) { usage(); return 0; }
        if (!strcmp(argv[ai], "-N") || !strcmp(argv[ai], "--LINE-NUMBERS")) { P.numbers = 1; continue; }
        if (argv[ai][0] == '-' && argv[ai][1]) continue;   /* ignore other less-ish flags */
        break;
    }
    P.files = &argv[ai]; P.nfiles = argc - ai; P.cur = 0;

    if (P.nfiles == 0) {
        if (isatty(STDIN_FILENO)) { usage(); return 1; }
        load_stream(stdin, "(stdin)");
        P.tin = open("/dev/tty", O_RDONLY);
    } else {
        if (load_file(P.files[0]) != 0) { fprintf(stderr, "%s\n", P.msg); return 1; }
        P.tin = isatty(STDIN_FILENO) ? STDIN_FILENO : open("/dev/tty", O_RDONLY);
    }

    /* not a real terminal session → behave like cat */
    if (!isatty(STDOUT_FILENO) || P.tin < 0) { cat_out(); return 0; }

    get_size();
    raw_on(); atexit(restore);
    if (!P.raw) { cat_out(); return 0; }

    for (;;) {
        clamp();
        draw();
        int c = getkey();
        if (c == -1) return 0;              /* tty closed / input exhausted → quit */
        P.msg[0] = 0;
        int page = P.rows_scr, half = P.rows_scr / 2 > 0 ? P.rows_scr / 2 : 1;
        switch (c) {
        case 'q': case 'Q': return 0;
        case ' ': case 'f': case 6: case K_PGDN: P.top += page; break;   /* ^F = 6 */
        case 'b': case 2: case K_PGUP: P.top -= page; break;            /* ^B = 2 */
        case 'j': case '\r': case '\n': case K_DOWN: P.top += 1; break;
        case 'k': case K_UP: P.top -= 1; break;
        case 'd': case 4: P.top += half; break;                         /* ^D */
        case 'u': case 21: P.top -= half; break;                        /* ^U */
        case 'g': case '<': case K_HOME: P.top = 0; break;
        case 'G': case '>': case K_END: P.top = maxtop(); break;
        case K_LEFT: P.hoff -= 8; break;
        case K_RIGHT: P.hoff += 8; break;
        case 'h': case 'H':
            snprintf(P.msg, sizeof P.msg,
                "SPACE/f fwd  b back  j/k line  d/u half  g/G top/end  /?  search  n/N repeat  :n/:p file  q quit");
            break;
        case '/': case '?': {
            char pat[256];
            char ps[2] = { (char)c, 0 };
            if (prompt(ps, pat, sizeof pat) > 0) { set_search(pat, c == '/' ? 1 : -1); do_search(P.re_dir, 1); }
            break; }
        case 'n': do_search(P.re_dir, 1); break;
        case 'N': do_search(-P.re_dir, 1); break;
        case ':': {
            char cmd[64];
            if (prompt(":", cmd, sizeof cmd) > 0) {
                if (!strcmp(cmd, "n")) open_index(P.cur + 1);
                else if (!strcmp(cmd, "p")) open_index(P.cur - 1);
                else if (!strcmp(cmd, "q")) return 0;
                else snprintf(P.msg, sizeof P.msg, "unknown command: :%s", cmd);
            }
            break; }
        default: break;
        }
    }
}
