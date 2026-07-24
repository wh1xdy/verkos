/* ved — VerkOS' own vi (a small modal text editor).
 *
 * VerkOS ships nano but no vi; ved fills that, the classic modal editor every
 * Unix wants its own of. Installed as vi/vim. A focused subset: normal/insert/
 * command modes, hjkl + word/line motion, x/dd/D/r, i/a/o/O/A/I insert, / search
 * with n, a one-level undo, and :w/:q/:wq/:q!/:N ex commands.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <termios.h>
#include <sys/ioctl.h>

enum { NORMAL, INSERT, COMMAND };

static struct {
    char **row; int nrows, cap;         /* text lines (no trailing \n) */
    int cx, cy, off;                    /* cursor col/row, scroll offset */
    int mode, rows_scr, cols_scr;
    char file[256]; int dirty;
    char msg[128], cmd[128]; int clen;
    struct termios orig; int raw;
    /* one-level undo snapshot */
    char **urow; int unrows, ucx, ucy;
} E;

/* ---- raw terminal ------------------------------------------------------- */
static void restore(void) { if (E.raw) tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig); }
static void raw_on(void) {
    if (tcgetattr(STDIN_FILENO, &E.orig) < 0) return;   /* not a tty (e.g. piped) */
    struct termios r = E.orig;
    r.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    r.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    r.c_oflag &= ~(OPOST);
    r.c_cc[VMIN] = 1; r.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &r) == 0) E.raw = 1;
}
static void get_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row) { E.rows_scr = ws.ws_row - 1; E.cols_scr = ws.ws_col; }
    else { E.rows_scr = 24; E.cols_scr = 80; }
}

/* ---- rows --------------------------------------------------------------- */
static void row_free_all(char **r, int n) { for (int i = 0; i < n; i++) free(r[i]); free(r); }
static void ins_row(int at, const char *s, int len) {
    if (E.nrows >= E.cap) { E.cap = E.cap ? E.cap * 2 : 64; E.row = realloc(E.row, E.cap * sizeof *E.row); }
    memmove(&E.row[at + 1], &E.row[at], (E.nrows - at) * sizeof *E.row);
    E.row[at] = malloc(len + 1); memcpy(E.row[at], s, len); E.row[at][len] = 0;
    E.nrows++;
}
static void del_row(int at) {
    if (at < 0 || at >= E.nrows) return;
    free(E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], (E.nrows - at - 1) * sizeof *E.row);
    E.nrows--;
    if (E.nrows == 0) ins_row(0, "", 0);
}
static void save_undo(void) {
    row_free_all(E.urow, E.unrows);
    E.urow = malloc(E.nrows * sizeof *E.urow); E.unrows = E.nrows;
    for (int i = 0; i < E.nrows; i++) E.urow[i] = strdup(E.row[i]);
    E.ucx = E.cx; E.ucy = E.cy;
}
static void do_undo(void) {
    if (!E.urow) return;
    char **t = E.row; int tn = E.nrows, tc = E.cap;
    E.row = malloc(E.unrows * sizeof *E.row); E.cap = E.unrows;
    for (int i = 0; i < E.unrows; i++) E.row[i] = strdup(E.urow[i]);
    int on = E.nrows; E.nrows = E.unrows;
    /* swap current into undo slot so u toggles */
    row_free_all(E.urow, E.unrows); E.urow = t; E.unrows = tn; (void)tc; (void)on;
    E.cx = E.ucx; E.cy = E.ucy; E.dirty = 1;
    snprintf(E.msg, sizeof E.msg, "undo");
}

/* ---- file I/O ----------------------------------------------------------- */
static void load(const char *fn) {
    snprintf(E.file, sizeof E.file, "%s", fn);
    FILE *f = fopen(fn, "r");
    if (!f) { ins_row(0, "", 0); snprintf(E.msg, sizeof E.msg, "\"%s\" [New File]", fn); return; }
    char *line = NULL; size_t cap = 0; ssize_t n; long bytes = 0;
    while ((n = getline(&line, &cap, f)) != -1) {
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) n--;
        ins_row(E.nrows, line, n); bytes += n + 1;
    }
    free(line); fclose(f);
    if (E.nrows == 0) ins_row(0, "", 0);
    snprintf(E.msg, sizeof E.msg, "\"%s\" %dL, %ldB", fn, E.nrows, bytes);
}
static int save_file(const char *fn) {
    FILE *f = fopen(fn, "w"); if (!f) return -1;
    long bytes = 0;
    for (int i = 0; i < E.nrows; i++) { fputs(E.row[i], f); fputc('\n', f); bytes += strlen(E.row[i]) + 1; }
    fclose(f);
    E.dirty = 0;
    snprintf(E.msg, sizeof E.msg, "\"%s\" %dL, %ldB written", fn, E.nrows, bytes);
    return 0;
}

/* ---- rendering ---------------------------------------------------------- */
static void draw(void) {
    if (E.cy < E.off) E.off = E.cy;
    if (E.cy >= E.off + E.rows_scr) E.off = E.cy - E.rows_scr + 1;
    char buf[65536]; int p = 0;
    p += sprintf(buf + p, "\033[H");                 /* cursor home */
    for (int y = 0; y < E.rows_scr; y++) {
        p += sprintf(buf + p, "\033[K");             /* clear line */
        int fr = E.off + y;
        if (fr < E.nrows) {
            int len = strlen(E.row[fr]); if (len > E.cols_scr) len = E.cols_scr;
            memcpy(buf + p, E.row[fr], len); p += len;
        } else buf[p++] = '~';
        p += sprintf(buf + p, "\r\n");
    }
    /* status bar (inverse) */
    p += sprintf(buf + p, "\033[7m\033[K");
    const char *m = E.mode == INSERT ? "-- INSERT --" : E.mode == COMMAND ? "" : "";
    char st[512];
    if (E.mode == COMMAND) snprintf(st, sizeof st, ":%s", E.cmd);
    else snprintf(st, sizeof st, "%s %s%s  %d,%d   %s", m,
                  E.file[0] ? E.file : "[No Name]", E.dirty ? " [+]" : "", E.cy + 1, E.cx + 1, E.msg);
    int sl = strlen(st); if (sl > E.cols_scr) sl = E.cols_scr;
    memcpy(buf + p, st, sl); p += sl;
    p += sprintf(buf + p, "\033[m");
    /* place cursor */
    int scx = E.cx + 1; if (scx > E.cols_scr) scx = E.cols_scr;
    p += sprintf(buf + p, "\033[%d;%dH", E.cy - E.off + 1, scx);
    write(STDOUT_FILENO, buf, p);
}

/* ---- editing primitives ------------------------------------------------- */
static char *cur(void) { return E.row[E.cy]; }
static int curlen(void) { return strlen(cur()); }
static void clamp(void) {
    if (E.cy < 0) E.cy = 0;
    if (E.cy >= E.nrows) E.cy = E.nrows - 1;
    int l = curlen(); int max = E.mode == INSERT ? l : (l ? l - 1 : 0);
    if (E.cx > max) E.cx = max;
    if (E.cx < 0) E.cx = 0;
}
static void ins_char(int c) {
    char *r = cur(); int l = strlen(r);
    r = realloc(r, l + 2); E.row[E.cy] = r;
    memmove(&r[E.cx + 1], &r[E.cx], l - E.cx + 1);
    r[E.cx] = (char)c; E.cx++; E.dirty = 1;
}
static void split_line(void) {
    char *r = cur(); int l = strlen(r);
    ins_row(E.cy + 1, r + E.cx, l - E.cx);
    r[E.cx] = 0; E.row[E.cy] = realloc(r, E.cx + 1);
    E.cy++; E.cx = 0; E.dirty = 1;
}
static void del_char_before(void) {
    if (E.cx == 0) {
        if (E.cy == 0) return;
        int pl = strlen(E.row[E.cy - 1]);
        char *p = realloc(E.row[E.cy - 1], pl + curlen() + 1);
        strcpy(p + pl, cur()); E.row[E.cy - 1] = p;
        del_row(E.cy); E.cy--; E.cx = pl; E.dirty = 1;
    } else {
        char *r = cur(); int l = strlen(r);
        memmove(&r[E.cx - 1], &r[E.cx], l - E.cx + 1); E.cx--; E.dirty = 1;
    }
}
static void del_char_at(void) {
    char *r = cur(); int l = strlen(r);
    if (E.cx < l) { memmove(&r[E.cx], &r[E.cx + 1], l - E.cx); E.dirty = 1; }
}

/* ---- input -------------------------------------------------------------- */
static int getch(void) { char c; int n = read(STDIN_FILENO, &c, 1); return n == 1 ? (unsigned char)c : -1; }

static void search(const char *pat) {
    if (!pat[0]) return;
    for (int i = 0; i < E.nrows; i++) {
        int y = (E.cy + 1 + i) % E.nrows;
        char *h = strstr(E.row[y], pat);
        if (h) { E.cy = y; E.cx = (int)(h - E.row[y]); snprintf(E.msg, sizeof E.msg, "/%s", pat); return; }
    }
    snprintf(E.msg, sizeof E.msg, "pattern not found: %s", pat);
}

static int run_ex(const char *c, int *quit) {
    /* :w [file] :q :q! :wq :x :N */
    if (isdigit((unsigned char)c[0])) { int n = atoi(c); if (n >= 1 && n <= E.nrows) { E.cy = n - 1; E.cx = 0; } return 0; }
    int forced = 0; char cmd[128]; snprintf(cmd, sizeof cmd, "%s", c);
    char *bang = strchr(cmd, '!'); if (bang) { forced = 1; *bang = 0; }
    char *sp = strchr(cmd, ' '); const char *arg = NULL;
    if (sp) { *sp = 0; arg = sp + 1; while (*arg == ' ') arg++; }
    int wrote = 0;
    if (strchr(cmd, 'w')) { if (save_file(arg && *arg ? arg : E.file) < 0) { snprintf(E.msg, sizeof E.msg, "write failed"); return 0; } wrote = 1; }
    if (strchr(cmd, 'x') && !wrote) save_file(E.file);
    if (strchr(cmd, 'q') || strchr(cmd, 'x')) {
        if (E.dirty && !forced && !wrote) { snprintf(E.msg, sizeof E.msg, "No write since last change (add ! to override)"); return 0; }
        *quit = 1;
    }
    return 0;
}

static void normal_key(int c, int *quit) {
    static int pending_g = 0, pending_d = 0, pending_r = 0;
    if (pending_r) { pending_r = 0; if (c >= 32) { save_undo(); if (E.cx < curlen()) { cur()[E.cx] = (char)c; E.dirty = 1; } } return; }
    if (pending_d) { pending_d = 0; if (c == 'd') { save_undo(); del_row(E.cy); clamp(); } return; }
    if (pending_g) { pending_g = 0; if (c == 'g') { E.cy = 0; E.cx = 0; } return; }
    switch (c) {
        case 'h': case 127: case 8: E.cx--; clamp(); break;
        case 'l': case ' ': E.cx++; clamp(); break;
        case 'j': E.cy++; clamp(); break;
        case 'k': E.cy--; clamp(); break;
        case '0': E.cx = 0; break;
        case '^': { int i = 0; while (cur()[i] && isspace((unsigned char)cur()[i])) i++; E.cx = i; break; }
        case '$': E.cx = curlen() ? curlen() - 1 : 0; break;
        case 'G': E.cy = E.nrows - 1; E.cx = 0; clamp(); break;
        case 'g': pending_g = 1; break;
        case 'w': { char *r = cur(); int i = E.cx; while (r[i] && !isspace((unsigned char)r[i])) i++; while (r[i] && isspace((unsigned char)r[i])) i++; if (!r[i] && E.cy < E.nrows-1) { E.cy++; E.cx=0; } else E.cx = i; clamp(); break; }
        case 'b': { int i = E.cx; if (i>0) i--; while (i>0 && isspace((unsigned char)cur()[i])) i--; while (i>0 && !isspace((unsigned char)cur()[i-1])) i--; E.cx = i; break; }
        case 'x': save_undo(); del_char_at(); clamp(); break;
        case 'D': save_undo(); if (E.cx < curlen()) { cur()[E.cx] = 0; E.row[E.cy] = realloc(cur(), E.cx + 1); } clamp(); break;
        case 'd': pending_d = 1; break;
        case 'r': pending_r = 1; break;
        case 'u': do_undo(); clamp(); break;
        case 'i': save_undo(); E.mode = INSERT; break;
        case 'a': save_undo(); E.cx++; E.mode = INSERT; clamp(); break;
        case 'A': save_undo(); E.cx = curlen(); E.mode = INSERT; break;
        case 'I': save_undo(); { int i = 0; while (cur()[i] && isspace((unsigned char)cur()[i])) i++; E.cx = i; } E.mode = INSERT; break;
        case 'o': save_undo(); ins_row(E.cy + 1, "", 0); E.cy++; E.cx = 0; E.mode = INSERT; E.dirty = 1; break;
        case 'O': save_undo(); ins_row(E.cy, "", 0); E.cx = 0; E.mode = INSERT; E.dirty = 1; break;
        case ':': E.mode = COMMAND; E.clen = 0; E.cmd[0] = 0; break;
        case '/': { E.mode = COMMAND; E.clen = 0; E.cmd[0] = 0; E.cmd[E.clen++] = '/'; E.cmd[E.clen] = 0; break; }
        case 'n': search(E.cmd[0] == '/' ? E.cmd + 1 : E.cmd); break;
        case '\r': case '\n': E.cy++; E.cx = 0; clamp(); break;
        case 27: break;
        default: (void)quit; break;
    }
}

static void insert_key(int c) {
    if (c == 27) { E.mode = NORMAL; E.cx--; clamp(); }
    else if (c == '\r' || c == '\n') split_line();
    else if (c == 127 || c == 8) del_char_before();
    else if (c == '\t') { ins_char(' '); ins_char(' '); }
    else if (c >= 32 || c == '\t') ins_char(c);
}

static void command_key(int c, int *quit) {
    if (c == 27) { E.mode = NORMAL; E.msg[0] = 0; }
    else if (c == '\r' || c == '\n') {
        E.mode = NORMAL;
        if (E.cmd[0] == '/') search(E.cmd + 1);
        else run_ex(E.cmd, quit);
    }
    else if (c == 127 || c == 8) { if (E.clen > 0) E.cmd[--E.clen] = 0; else E.mode = NORMAL; }
    else if (c >= 32 && E.clen < (int)sizeof E.cmd - 1) { E.cmd[E.clen++] = (char)c; E.cmd[E.clen] = 0; }
}

int main(int argc, char **argv) {
    memset(&E, 0, sizeof E);
    load(argc > 1 ? argv[1] : "");
    if (!E.file[0] && argc <= 1) { }   /* [No Name] */
    raw_on(); get_size(); atexit(restore);

    int quit = 0;
    while (!quit) {
        draw();
        int c = getch();
        if (c == -1) break;                          /* EOF (piped input done) */
        if (E.mode == NORMAL) normal_key(c, &quit);
        else if (E.mode == INSERT) insert_key(c);
        else command_key(c, &quit);
    }
    write(STDOUT_FILENO, "\033[2J\033[H", 7);
    return 0;
}
