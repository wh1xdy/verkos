/* vsed — VerkOS' own sed (stream editor), full engine.
 *
 * A proper sed interpreter: a pattern space and a hold space, the full command
 * set (s y p P d D n N h H g G x b t T : { } a i c = l q Q r w #), line/$/regex/
 * step addresses with ranges and ! negation, and -n/-e/-f/-E/-i/-s/-z. The s
 * command supports g/p/i/Nth and &, \0-\9, \n, \t, \L\U\l\u\E in the replacement.
 *
 * Complete enough to take over `sed` if it survives the build-safety check;
 * otherwise it ships as `vsed` and GNU sed stays.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <regex.h>
#include <ctype.h>

/* ---- dynamic string (pattern/hold space; may contain embedded newlines) -- */
struct str { char *s; size_t len, cap; };
static void s_reserve(struct str *b, size_t n) { if (b->len + n + 1 > b->cap) { b->cap = (b->len + n + 1) * 2; b->s = realloc(b->s, b->cap); } }
static void s_set(struct str *b, const char *p, size_t n) { b->len = 0; s_reserve(b, n); memcpy(b->s, p, n); b->len = n; b->s[n] = 0; }
static void s_cat(struct str *b, const char *p, size_t n) { s_reserve(b, n); memcpy(b->s + b->len, p, n); b->len += n; b->s[b->len] = 0; }
static void s_catc(struct str *b, char c) { s_reserve(b, 1); b->s[b->len++] = c; b->s[b->len] = 0; }

/* ---- addresses & commands ----------------------------------------------- */
enum { A_NONE, A_LINE, A_LAST, A_RE, A_STEP, A_ZERO };
struct addr { int type; long n, step; regex_t re; int has_re; };
struct cmd {
    struct addr a1, a2; int naddr, negate, in_range;
    char c;
    regex_t re; int has_re; char *repl; int gflag, pflag, nth, icase;
    char *y1, *y2; int ylen;
    char *text;                 /* a/i/c text, :label/b/t/T label, r/w filename */
    int jump;                   /* resolved target for { } b t T */
};

static struct cmd cmds[512]; static int ncmd;
static int nflag, Eflag, sepflag;
static char labels[256][64]; static int label_idx[256]; static int nlabels;

static void fatal(const char *m) { fprintf(stderr, "sed: %s\n", m); exit(1); }
static void skipws(char **p) { while (**p == ' ' || **p == '\t') (*p)++; }

static int parse_addr(char **p, struct addr *a) {
    char *s = *p; a->type = A_NONE; a->has_re = 0;
    if (*s == '$') { a->type = A_LAST; *p = s + 1; return 1; }
    if (*s >= '0' && *s <= '9') {
        a->n = strtol(s, &s, 10);
        if (*s == '~') { s++; a->step = strtol(s, &s, 10); a->type = A_STEP; }
        else a->type = (a->n == 0) ? A_ZERO : A_LINE;
        *p = s; return 1;
    }
    if (*s == '/' || *s == '\\') {
        char delim = '/'; if (*s == '\\') { s++; delim = *s; } s++;
        char re[1024]; int n = 0, inbr = 0;
        while (*s && (*s != delim || inbr) && n < (int)sizeof re - 2) {
            if (*s == '\\' && s[1]) {
                if (s[1] == delim && !inbr) { re[n++] = delim; s += 2; }
                else if (s[1] == 'n') { re[n++] = '\n'; s += 2; }
                else if (s[1] == 't') { re[n++] = '\t'; s += 2; }
                else { re[n++] = *s++; re[n++] = *s++; }
                continue;
            }
            if (*s == '[' && !inbr) { inbr = 1; re[n++] = *s++; if (*s == '^') re[n++] = *s++; if (*s == ']') re[n++] = *s++; continue; }
            if (*s == ']' && inbr) inbr = 0;
            re[n++] = *s++;
        }
        if (*s != delim) fatal("unterminated address regex");
        re[n] = 0; s++;
        int cf = Eflag ? REG_EXTENDED : 0;
        if (*s == 'I') { cf |= REG_ICASE; s++; }
        if (regcomp(&a->re, re, cf)) fatal("bad address regex");
        a->type = A_RE; a->has_re = 1; *p = s; return 1;
    }
    return 0;
}

/* take text for a/i/c: after optional '\', the rest up to an unescaped newline */
static char *take_text(char **p) {
    char *s = *p;
    if (*s == '\\') { s++; if (*s == '\n') s++; }
    skipws(&s);
    struct str t = {0};
    while (*s && *s != '\n') {
        if (*s == '\\' && s[1]) { s++; if (*s == 'n') { s_catc(&t, '\n'); s++; continue; } if (*s == 't') { s_catc(&t, '\t'); s++; continue; } s_catc(&t, *s++); continue; }
        s_catc(&t, *s++);
    }
    *p = s;
    return t.s ? t.s : strdup("");
}
static char *take_label(char **p) {
    char *s = *p; skipws(&s); struct str t = {0};
    while (*s && *s != '\n' && *s != ';' && *s != ' ' && *s != '\t' && *s != '}') s_catc(&t, *s++);
    *p = s; return t.s ? t.s : strdup("");
}

static void parse_script(char *script) {
    char *s = script;
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == ';' || *s == '\n') s++;
        if (!*s) break;
        if (*s == '#') { while (*s && *s != '\n') s++; continue; }
        struct cmd *c = &cmds[ncmd]; memset(c, 0, sizeof *c);
        c->naddr = parse_addr(&s, &c->a1);
        if (c->naddr && *s == ',') { s++; parse_addr(&s, &c->a2); c->naddr = 2; }
        skipws(&s);
        while (*s == '!') { c->negate ^= 1; s++; skipws(&s); }
        char op = *s++;
        c->c = op;
        switch (op) {
            case 's': {
                char d = *s++; char re[2048]; int n = 0, inbr = 0;
                while (*s && (*s != d || inbr)) {
                    if (*s == '\\' && s[1]) {
                        if (s[1] == d && !inbr) { re[n++] = d; s += 2; }
                        else if (s[1] == 'n') { re[n++] = '\n'; s += 2; }
                        else if (s[1] == 't') { re[n++] = '\t'; s += 2; }
                        else { re[n++] = *s++; re[n++] = *s++; }   /* keep \\, \., \1 ... intact */
                        continue;
                    }
                    if (*s == '[' && !inbr) { inbr = 1; re[n++] = *s++; if (*s == '^') re[n++] = *s++; if (*s == ']') re[n++] = *s++; continue; }
                    if (*s == ']' && inbr) inbr = 0;
                    re[n++] = *s++;
                }
                if (*s != d) fatal("unterminated s");
                re[n] = 0; s++;
                struct str rp = {0};
                while (*s && *s != d) {
                    if (*s == '\\' && s[1]) { if (s[1] == d) { s_catc(&rp, d); s += 2; } else { s_catc(&rp, *s++); s_catc(&rp, *s++); } continue; }
                    s_catc(&rp, *s++);
                }
                if (*s != d) fatal("unterminated s");
                s++;
                int cf = Eflag ? REG_EXTENDED : 0;
                while (*s && *s != ';' && *s != '\n' && *s != '}') {
                    if (*s == 'g') c->gflag = 1; else if (*s == 'p') c->pflag = 1;
                    else if (*s == 'i' || *s == 'I') { cf |= REG_ICASE; c->icase = 1; }
                    else if (*s == 'm' || *s == 'M') cf |= REG_NEWLINE;
                    else if (*s >= '0' && *s <= '9') c->nth = atoi(s);
                    s++;
                }
                if (regcomp(&c->re, re, cf)) fatal("bad s regex");
                c->has_re = 1; c->repl = rp.s ? rp.s : strdup(""); break;
            }
            case 'y': {
                char d = *s++; struct str a = {0}, b = {0};
                while (*s && *s != d) { if (*s == '\\' && s[1]) { s++; if (*s=='n') s_catc(&a,'\n'); else if(*s=='t') s_catc(&a,'\t'); else s_catc(&a,*s); s++; } else s_catc(&a, *s++); }
                if (*s != d) fatal("unterminated y");
                s++;
                while (*s && *s != d) { if (*s == '\\' && s[1]) { s++; if (*s=='n') s_catc(&b,'\n'); else if(*s=='t') s_catc(&b,'\t'); else s_catc(&b,*s); s++; } else s_catc(&b, *s++); }
                if (*s != d) fatal("unterminated y");
                s++;
                if (a.len != b.len) fatal("y strings differ in length");
                c->y1 = a.s; c->y2 = b.s; c->ylen = a.len; break;
            }
            case 'a': case 'i': case 'c': c->text = take_text(&s); break;
            case 'b': case 't': case 'T': c->text = take_label(&s); break;
            case ':': c->text = take_label(&s);
                      if (nlabels < 256) { snprintf(labels[nlabels], 64, "%s", c->text); label_idx[nlabels] = ncmd; nlabels++; } break;
            case 'r': case 'w': skipws(&s); c->text = take_label(&s); break;
            case 'l': while (*s >= '0' && *s <= '9') s++; break;   /* optional width, ignored */
            case 'q': case 'Q': while (*s >= '0' && *s <= '9') s++; break;   /* optional exit code */
            case '{': case '}':
            case 'd': case 'D': case 'p': case 'P': case 'n': case 'N':
            case 'h': case 'H': case 'g': case 'G': case 'x': case '=': case 'z': case 'F':
                break;
            default: fprintf(stderr, "sed: unknown command: %c\n", op); exit(1);
        }
        ncmd++;
        /* Skip whatever follows a command up to the next separator — but NOT
         * after a brace. A '{' is followed immediately by the first command of
         * its block ("/./ {s/a/b/}"), and skipping to the next separator
         * swallowed that command whole, silently: the block was entered and
         * did nothing. Commands on later lines of the same block still parsed,
         * so a multi-line script would apply some of its substitutions and not
         * others, which is how cmake came out half-configured. */
        if (op != '{' && op != '}')
            while (*s && *s != ';' && *s != '\n' && *s != '}') s++;
    }
    /* resolve { } nesting and b/t/T labels */
    int stack[256], sp = 0;
    for (int i = 0; i < ncmd; i++) {
        if (cmds[i].c == '{') stack[sp++] = i;
        else if (cmds[i].c == '}') { if (sp) cmds[stack[--sp]].jump = i; }
    }
    for (int i = 0; i < ncmd; i++) {
        char cc = cmds[i].c;
        if (cc == 'b' || cc == 't' || cc == 'T') {
            cmds[i].jump = ncmd;   /* empty label -> end of script */
            if (cmds[i].text && cmds[i].text[0])
                for (int j = 0; j < nlabels; j++) if (!strcmp(labels[j], cmds[i].text)) { cmds[i].jump = label_idx[j]; break; }
        }
    }
}

/* ---- s/// substitution -------------------------------------------------- */
static int do_subst(struct cmd *c, struct str *ps) {
    regmatch_t m[10];
    char *src = malloc(ps->len + 1); memcpy(src, ps->s, ps->len + 1);
    struct str out = {0}; size_t off = 0; int count = 0, did = 0;
    int ucase = 0, lcase = 0, uone = 0, lone = 0;
    while (off <= ps->len && regexec(&c->re, src + off, 10, m, off ? REG_NOTBOL : 0) == 0) {
        count++;
        int hit = c->gflag ? (c->nth ? count >= c->nth : 1) : (c->nth ? count == c->nth : count == 1);
        s_cat(&out, src + off, m[0].rm_so);
        if (hit) {
            did = 1;
            for (char *r = c->repl; *r; r++) {
                char ch = 0; int emit = 1;
                if (*r == '&') { for (int k = m[0].rm_so; k < m[0].rm_eo; k++) { char x=src[off+k]; if(uone){x=toupper((unsigned char)x);uone=0;}else if(lone){x=tolower((unsigned char)x);lone=0;}else if(ucase)x=toupper((unsigned char)x);else if(lcase)x=tolower((unsigned char)x); s_catc(&out,x);} emit=0; }
                else if (*r == '\\' && r[1]) {
                    r++;
                    if (*r >= '0' && *r <= '9') { int g = *r - '0'; if (m[g].rm_so >= 0) for (int k = m[g].rm_so; k < m[g].rm_eo; k++){char x=src[off+k]; if(uone){x=toupper((unsigned char)x);uone=0;}else if(lone){x=tolower((unsigned char)x);lone=0;}else if(ucase)x=toupper((unsigned char)x);else if(lcase)x=tolower((unsigned char)x); s_catc(&out,x);} emit = 0; }
                    else if (*r == 'n') ch = '\n';
                    else if (*r == 't') ch = '\t';
                    else if (*r == 'U') { ucase = 1; lcase = 0; emit = 0; }
                    else if (*r == 'L') { lcase = 1; ucase = 0; emit = 0; }
                    else if (*r == 'E') { ucase = lcase = 0; emit = 0; }
                    else if (*r == 'u') { uone = 1; emit = 0; }
                    else if (*r == 'l') { lone = 1; emit = 0; }
                    else ch = *r;
                } else ch = *r;
                if (emit) { if (uone) { ch = toupper((unsigned char)ch); uone = 0; } else if (lone) { ch = tolower((unsigned char)ch); lone = 0; } else if (ucase) ch = toupper((unsigned char)ch); else if (lcase) ch = tolower((unsigned char)ch); s_catc(&out, ch); }
            }
        } else s_cat(&out, src + off + m[0].rm_so, m[0].rm_eo - m[0].rm_so);
        int adv = m[0].rm_eo;
        if (adv == m[0].rm_so) { if (off + adv < ps->len) s_catc(&out, src[off + adv]); adv++; }
        off += adv;
        if (!c->gflag && count >= (c->nth ? c->nth : 1)) break;
    }
    if (off < ps->len) s_cat(&out, src + off, ps->len - off);
    free(src);
    if (did) { free(ps->s); ps->s = out.s; ps->len = out.len; ps->cap = out.cap; }
    else free(out.s);
    return did;
}

/* ---- address matching --------------------------------------------------- */
static int amatch(struct addr *a, long ln, struct str *ps, int last) {
    switch (a->type) {
        case A_LINE: return ln == a->n;
        case A_ZERO: return 0;
        case A_LAST: return last;
        case A_STEP: return a->step > 0 ? (ln >= a->n && (ln - a->n) % a->step == 0) : ln == a->n;
        case A_RE:   return regexec(&a->re, ps->s, 0, NULL, 0) == 0;
        default:     return 1;
    }
}
static int selected(struct cmd *c, long ln, struct str *ps, int last) {
    int r;
    if (c->naddr == 0) r = 1;
    else if (c->naddr == 1) r = amatch(&c->a1, ln, ps, last);
    else {
        if (!c->in_range) {
            if (c->a1.type == A_ZERO) { c->in_range = 1; r = 1;   /* 0,/re/ starts active */
                if (amatch(&c->a2, ln, ps, last)) c->in_range = 0; }
            else if (amatch(&c->a1, ln, ps, last)) { c->in_range = 1; r = 1;
                if (c->a2.type == A_LINE && c->a2.n <= ln) c->in_range = 0; }
            else r = 0;
        } else {
            r = 1;
            if (amatch(&c->a2, ln, ps, last) || (c->a2.type == A_LINE && ln >= c->a2.n)) c->in_range = 0;
        }
    }
    return c->negate ? !r : r;
}

/* ---- main loop ---------------------------------------------------------- */
static struct str PS, HS; static int subst_since_t;
static struct str append_q;         /* text queued by a/r, emitted after the cycle */
static FILE **g_ins; static int g_nin, g_fi; static long g_ln; static int g_last;
static char *g_lb; static size_t g_lc;

static void out_line(FILE *o, struct str *s) { fwrite(s->s, 1, s->len, o); fputc(sepflag ? 0 : '\n', o); }
static void flush_appends(FILE *o) { if (append_q.len) { fwrite(append_q.s, 1, append_q.len, o); append_q.len = 0; if (append_q.s) append_q.s[0] = 0; } }
static void queue_file(const char *fn) { FILE *f = fopen(fn, "r"); if (!f) return; char b[4096]; size_t r; while ((r = fread(b, 1, sizeof b, f)) > 0) s_cat(&append_q, b, r); fclose(f); }

/* read the next input line into `into` (resets the t-flag); 0 at EOF */
static int next_line(struct str *into) {
    for (;;) {
        if (g_fi >= g_nin) return 0;
        ssize_t nn = getdelim(&g_lb, &g_lc, sepflag ? 0 : '\n', g_ins[g_fi]);
        if (nn < 0) { g_fi++; continue; }
        g_ln++; subst_since_t = 0;
        if (nn > 0 && g_lb[nn-1] == (sepflag ? 0 : '\n')) nn--;
        s_set(into, g_lb, nn);
        int c = getc(g_ins[g_fi]);
        if (c == EOF) g_last = (g_fi >= g_nin - 1);
        else { ungetc(c, g_ins[g_fi]); g_last = 0; }
        return 1;
    }
}

/* append a newline + the next input line to `into`; 0 at EOF */
static int next_line_append(struct str *into) {
    struct str tmp = {0};
    if (!next_line(&tmp)) { free(tmp.s); return 0; }
    s_catc(into, '\n'); s_cat(into, tmp.s, tmp.len); free(tmp.s);
    return 1;
}

/* returns: 0 normal cycle end, 1 quit(print done), 2 quit(no print) */
/* w FILE — write the pattern space to a file. The file is truncated the first
 * time it is used and stays open for the rest of the run, so later writes
 * accumulate, which is what sed specifies. This was parsed and then silently
 * discarded: sed exited 0 and the file never appeared. gnulib generates its
 * replacement headers with "sed -n -e 'w $@-t'", so the build died on
 * "mv: cannot stat 'lib/selinux/context.h-t'" with nothing to say why. */
static struct { char *name; FILE *fp; } wfiles[32];
static int nwfiles;
static FILE *wfile(const char *name) {
    for (int i = 0; i < nwfiles; i++)
        if (!strcmp(wfiles[i].name, name)) return wfiles[i].fp;
    if (nwfiles >= (int)(sizeof wfiles / sizeof *wfiles)) return NULL;
    FILE *f;
    if (!strcmp(name, "/dev/stdout")) f = stdout;
    else if (!strcmp(name, "/dev/stderr")) f = stderr;
    else f = fopen(name, "w");
    if (!f) { fprintf(stderr, "sed: couldn't open file %s: %s\n", name, strerror(errno)); exit(4); }
    wfiles[nwfiles].name = strdup(name);
    wfiles[nwfiles].fp = f;
    return wfiles[nwfiles++].fp;
}

static int run_line(FILE *o) {
    int pc = 0, autoprint = !nflag, deleted = 0;
restart:
    for (; pc < ncmd; pc++) {
        struct cmd *c = &cmds[pc];
        if (c->c == '}' || c->c == ':') continue;
        int sel = selected(c, g_ln, &PS, g_last);
        if (!sel) { if (c->c == '{') pc = c->jump; continue; }
        switch (c->c) {
            case '{': break;
            case 's': if (do_subst(c, &PS)) { subst_since_t = 1; if (c->pflag) out_line(o, &PS); } break;
            case 'y': for (size_t i = 0; i < PS.len; i++) for (int k = 0; k < c->ylen; k++) if (PS.s[i] == c->y1[k]) { PS.s[i] = c->y2[k]; break; } break;
            case 'p': out_line(o, &PS); break;
            case 'P': { char *nl = memchr(PS.s, '\n', PS.len); size_t l = nl ? (size_t)(nl - PS.s) : PS.len; fwrite(PS.s, 1, l, o); fputc('\n', o); break; }
            case 'd': deleted = 1; goto endcycle;
            case 'D': { char *nl = memchr(PS.s, '\n', PS.len); if (!nl) { deleted = 1; goto endcycle; } size_t rem = PS.len - (nl - PS.s) - 1; memmove(PS.s, nl + 1, rem); PS.len = rem; PS.s[rem] = 0; pc = 0; goto restart; }
            case 'h': s_set(&HS, PS.s, PS.len); break;
            case 'H': s_catc(&HS, '\n'); s_cat(&HS, PS.s, PS.len); break;
            case 'g': s_set(&PS, HS.s ? HS.s : "", HS.len); break;
            case 'G': s_catc(&PS, '\n'); s_cat(&PS, HS.s ? HS.s : "", HS.len); break;
            case 'x': { struct str t = PS; PS = HS; HS = t; break; }
            case '=': fprintf(o, "%ld\n", g_ln); break;
            case 'z': PS.len = 0; if (PS.s) PS.s[0] = 0; break;
            case 'l': out_line(o, &PS); break;
            case 'F': fprintf(o, "-\n"); break;
            case 'n': if (autoprint) out_line(o, &PS);
                      flush_appends(o);
                      if (!next_line(&PS)) return 2;
                      break;
            case 'N': if (!next_line_append(&PS)) { if (autoprint) out_line(o, &PS); flush_appends(o); return 2; } break;
            case 'a': s_cat(&append_q, c->text, strlen(c->text)); s_catc(&append_q, '\n'); break;
            case 'i': fwrite(c->text, 1, strlen(c->text), o); fputc('\n', o); break;
            case 'c': deleted = 1;
                      if (c->naddr < 2 || !c->in_range) { fwrite(c->text, 1, strlen(c->text), o); fputc('\n', o); }
                      goto endcycle;
            case 'r': queue_file(c->text); break;
            case 'w': { FILE *wf = wfile(c->text);
                        if (wf) { fwrite(PS.s, 1, PS.len, wf); fputc('\n', wf); fflush(wf); }
                        break; }
            case 'b': pc = c->jump; goto restart;
            case 't': if (subst_since_t) { subst_since_t = 0; pc = c->jump; goto restart; } break;
            case 'T': if (!subst_since_t) { pc = c->jump; goto restart; } break;
            case 'q': if (autoprint) out_line(o, &PS); flush_appends(o); return 1;
            case 'Q': return 2;
        }
    }
endcycle:
    if (!deleted && autoprint) out_line(o, &PS);
    flush_appends(o);
    return 0;
}

static void process(FILE **ins, int nin, FILE *out) {
    g_ins = ins; g_nin = nin; g_fi = 0; g_ln = 0;
    while (next_line(&PS)) { int r = run_line(out); if (r == 1 || r == 2) return; }
}

int main(int argc, char **argv) {
    char *script = NULL; int inplace = 0;
    char *files[512]; int nf = 0;
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!strcmp(a, "-n") || !strcmp(a, "--quiet") || !strcmp(a, "--silent")) nflag = 1;
        else if (!strcmp(a, "-E") || !strcmp(a, "-r") || !strcmp(a, "--regexp-extended")) Eflag = 1;
        else if (!strcmp(a, "-s") || !strcmp(a, "--separate")) sepflag = 0;   /* accepted */
        else if (!strcmp(a, "-z") || !strcmp(a, "--null-data")) sepflag = 1;
        else if (!strcmp(a, "-i") || !strncmp(a, "-i", 2) || !strcmp(a, "--in-place")) inplace = 1;
        else if (!strcmp(a, "-e") || !strcmp(a, "--expression")) { char *add = argv[++i]; if (!script) script = strdup(add); else { script = realloc(script, strlen(script) + strlen(add) + 2); strcat(script, "\n"); strcat(script, add); } }
        else if (!strncmp(a, "-e", 2)) { char *add = a + 2; if (!script) script = strdup(add); else { script = realloc(script, strlen(script) + strlen(add) + 2); strcat(script, "\n"); strcat(script, add); } }
        else if (!strcmp(a, "-f") || !strcmp(a, "--file")) { FILE *f = fopen(argv[++i], "r"); if (!f) fatal("cannot read script"); char b[65536]; size_t r = fread(b, 1, sizeof b - 1, f); b[r] = 0; fclose(f); if (!script) script = strdup(b); else { script = realloc(script, strlen(script) + r + 2); strcat(script, "\n"); strcat(script, b); } }
        else if (!strcmp(a, "--posix") || !strcmp(a, "-u") || !strcmp(a, "--unbuffered")) ;
        else if (a[0] == '-' && a[1]) fatal("unknown option");
        else if (!script) script = strdup(a);
        else files[nf++] = a;
    }
    if (!script) { fprintf(stderr, "usage: sed [OPTION]... SCRIPT [FILE]...\n"); return 1; }
    parse_script(script);

    /* GNU sed exits 2 when it cannot read an input file. Reporting the problem
     * and then exiting 0 lets a script carry on as though the file were empty. */
    int rc = 0;
    if (inplace && nf > 0) {
        for (int i = 0; i < nf; i++) {
            for (int k = 0; k < ncmd; k++) cmds[k].in_range = 0;
            FILE *in = fopen(files[i], "r"); if (!in) { fprintf(stderr, "sed: %s: cannot open\n", files[i]); rc = 2; continue; }
            char tmp[600]; snprintf(tmp, sizeof tmp, "%s.sedtmp", files[i]);
            FILE *out = fopen(tmp, "w"); if (!out) { fprintf(stderr, "sed: %s: cannot write\n", tmp); fclose(in); rc = 4; continue; }
            FILE *ins[1] = { in }; process(ins, 1, out);
            fclose(in); fclose(out); rename(tmp, files[i]);
        }
        return 0;
    }
    if (nf == 0) { FILE *ins[1] = { stdin }; process(ins, 1, stdout); }
    else {
        FILE *ins[512]; int n = 0;
        for (int i = 0; i < nf; i++) { FILE *f = fopen(files[i], "r"); if (!f) { fprintf(stderr, "sed: %s: cannot open\n", files[i]); rc = 2; continue; } ins[n++] = f; }
        if (n) process(ins, n, stdout);
    }
    return rc;
}
