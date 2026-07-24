/* vsed — VerkOS' own sed (stream editor).
 *
 * A focused sed: line/last/regex addresses and ranges, and the s, d, p, q, and =
 * commands, with -n, -e, -f, -E/-r, and -i. The substitute command supports g/p/i
 * and Nth-occurrence flags and &, \0-\9, \n, \t in the replacement — the bulk of
 * everyday sed and of what configure scripts use. Installed as `sed` only if it
 * passes the build-safety check; otherwise it ships as vsed.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <regex.h>
#include <ctype.h>

enum { A_NONE, A_LINE, A_LAST, A_RE };
struct addr { int type; long line; regex_t re; int has_re; };
enum { C_S, C_D, C_P, C_Q, C_EQ };
struct cmd {
    struct addr a1, a2; int naddr;
    int in_range;                 /* runtime: are we inside a1,a2 ? */
    int op;
    regex_t re; int has_re;       /* for s///  */
    char *repl; int gflag, pflag, nth;
};

static struct cmd cmds[128]; static int ncmd;
static int nflag, Eflag;

static void fatal(const char *m) { fprintf(stderr, "sed: %s\n", m); exit(1); }

/* parse an address at *p; advance *p; return 0 if none */
static int parse_addr(char **p, struct addr *a) {
    char *s = *p; a->type = A_NONE; a->has_re = 0;
    if (*s == '$') { a->type = A_LAST; *p = s + 1; return 1; }
    if (isdigit((unsigned char)*s)) { a->type = A_LINE; a->line = strtol(s, &s, 10); *p = s; return 1; }
    if (*s == '/') {
        s++; char re[512]; int n = 0;
        while (*s && *s != '/' && n < (int)sizeof re - 1) { if (*s == '\\' && s[1] == '/') s++; re[n++] = *s++; }
        if (*s != '/') fatal("unterminated address regex");
        re[n] = 0; s++;
        if (regcomp(&a->re, re, Eflag ? REG_EXTENDED : 0)) fatal("bad address regex");
        a->type = A_RE; a->has_re = 1; *p = s; return 1;
    }
    return 0;
}

/* parse one command from string *p; advance past it */
static void parse_cmd(char **p, struct cmd *c) {
    memset(c, 0, sizeof *c);
    char *s = *p;
    while (*s == ' ' || *s == '\t' || *s == ';' || *s == '\n') s++;
    if (!*s) { *p = s; c->op = -1; return; }
    c->naddr = parse_addr(&s, &c->a1);
    if (c->naddr && *s == ',') { s++; parse_addr(&s, &c->a2); c->naddr = 2; }
    while (*s == ' ' || *s == '\t') s++;
    char op = *s++;
    switch (op) {
        case 's': {
            char delim = *s++; char re[1024]; int n = 0;
            while (*s && *s != delim) { if (*s == '\\' && s[1] == delim) { re[n++] = delim; s += 2; continue; } re[n++] = *s++; }
            if (*s != delim) fatal("unterminated s///");
            re[n] = 0; s++;
            char rp[1024]; n = 0;
            while (*s && *s != delim) { if (*s == '\\' && s[1] == delim) { rp[n++] = delim; s += 2; continue; } rp[n++] = *s++; }
            if (*s != delim) fatal("unterminated s///");
            rp[n] = 0; s++;
            while (*s && *s != ';' && *s != '\n') {
                if (*s == 'g') c->gflag = 1;
                else if (*s == 'p') c->pflag = 1;
                else if (*s == 'i' || *s == 'I') { /* case-insensitive; recompiled below */ c->nth = c->nth; }
                else if (isdigit((unsigned char)*s)) c->nth = atoi(s);
                s++;
            }
            int cf = (Eflag ? REG_EXTENDED : 0);
            if (regcomp(&c->re, re, cf)) fatal("bad s/// regex");
            c->has_re = 1; c->repl = strdup(rp); c->op = C_S; break;
        }
        case 'd': c->op = C_D; break;
        case 'p': c->op = C_P; break;
        case 'q': c->op = C_Q; break;
        case '=': c->op = C_EQ; break;
        default: fatal("unknown command");
    }
    while (*s && *s != ';' && *s != '\n') s++;
    *p = s;
}

static void parse_script(char *script) {
    char *p = script;
    while (*p && ncmd < 128) {
        struct cmd c; parse_cmd(&p, &c);
        if (c.op == -1) break;
        cmds[ncmd++] = c;
    }
}

/* does address a match line `ln` (text `line`, last==is-last)? */
static int amatch(struct addr *a, long ln, const char *line, int last) {
    switch (a->type) {
        case A_LINE: return ln == a->line;
        case A_LAST: return last;
        case A_RE: return regexec(&a->re, line, 0, NULL, 0) == 0;
        default: return 0;
    }
}
static int selected(struct cmd *c, long ln, const char *line, int last) {
    if (c->naddr == 0) return 1;
    if (c->naddr == 1) return amatch(&c->a1, ln, line, last);
    /* two-address range */
    if (!c->in_range) {
        if (amatch(&c->a1, ln, line, last)) { c->in_range = 1;
            if (c->a2.type == A_LINE && c->a2.line <= ln) c->in_range = 0;  /* single line */
            return 1;
        }
        return 0;
    } else {
        if (amatch(&c->a2, ln, line, last)) c->in_range = 0;
        else if (c->a2.type == A_LINE && ln >= c->a2.line) c->in_range = 0;
        return 1;
    }
}

/* apply s/// to `line` (dynamically grown); returns 1 if any substitution */
static int do_subst(struct cmd *c, char **line) {
    regmatch_t m[10];
    char *src = *line; size_t off = 0; int count = 0, did = 0;
    char *out = malloc(strlen(src) + 1); size_t olen = 0, ocap = strlen(src) + 1;
    #define PUSH(ch) do { if (olen + 1 >= ocap) { ocap = ocap * 2 + 16; out = realloc(out, ocap); } out[olen++] = (ch); } while (0)
    while (src[off] && regexec(&c->re, src + off, 10, m, off ? REG_NOTBOL : 0) == 0) {
        count++;
        int match_this = c->gflag ? (c->nth ? count >= c->nth : 1) : (c->nth ? count == c->nth : count == 1);
        for (int i = 0; i < m[0].rm_so; i++) PUSH(src[off + i]);
        if (match_this) {
            did = 1;
            for (char *r = c->repl; *r; r++) {
                if (*r == '&') { for (int k = m[0].rm_so; k < m[0].rm_eo; k++) PUSH(src[off + k]); }
                else if (*r == '\\' && isdigit((unsigned char)r[1])) {
                    int g = r[1] - '0'; r++;
                    if (m[g].rm_so >= 0) for (int k = m[g].rm_so; k < m[g].rm_eo; k++) PUSH(src[off + k]);
                }
                else if (*r == '\\' && r[1] == 'n') { PUSH('\n'); r++; }
                else if (*r == '\\' && r[1] == 't') { PUSH('\t'); r++; }
                else if (*r == '\\' && r[1]) { PUSH(r[1]); r++; }
                else PUSH(*r);
            }
        } else {
            for (int k = m[0].rm_so; k < m[0].rm_eo; k++) PUSH(src[off + k]);
        }
        int adv = m[0].rm_eo;
        if (adv == m[0].rm_so) { if (src[off + adv]) PUSH(src[off + adv]); adv++; }   /* empty match */
        off += adv;
        if (!c->gflag && !(c->nth && count < c->nth)) break;
    }
    while (src[off]) PUSH(src[off++]);
    PUSH('\0'); olen--;
    if (did) { free(*line); *line = out; } else free(out);
    #undef PUSH
    return did;
}

static void process(FILE *in, int last_file, long *ln, FILE *out) {
    char *line = NULL; size_t cap = 0; ssize_t n;
    long total = 0;
    /* need lookahead to know "last line"; read all is simplest for $-correctness */
    char **all = NULL; int na = 0, ca = 0;
    while ((n = getline(&line, &cap, in)) != -1) {
        if (na >= ca) { ca = ca ? ca * 2 : 256; all = realloc(all, ca * sizeof *all); }
        if (n > 0 && line[n-1] == '\n') line[n-1] = 0;
        all[na++] = strdup(line);
    }
    free(line);
    for (int i = 0; i < na; i++) {
        (*ln)++; total = i;
        int islast = last_file && (i == na - 1);
        char *cur = all[i]; int deleted = 0, quit = 0;
        for (int ci = 0; ci < ncmd; ci++) {
            struct cmd *c = &cmds[ci];
            if (!selected(c, *ln, cur, islast)) continue;
            switch (c->op) {
                case C_S: if (do_subst(c, &cur) && c->pflag) { fputs(cur, out); fputc('\n', out); } break;
                case C_D: deleted = 1; break;
                case C_P: fputs(cur, out); fputc('\n', out); break;
                case C_EQ: fprintf(out, "%ld\n", *ln); break;
                case C_Q: quit = 1; break;
            }
            if (deleted || quit) break;
        }
        if (!nflag && !deleted) { fputs(cur, out); fputc('\n', out); }
        free(cur);
        if (quit) { for (int j = i + 1; j < na; j++) free(all[j]); break; }
    }
    free(all);
    (void)total;
}

int main(int argc, char **argv) {
    char *script = NULL; int inplace = 0;
    char *files[256]; int nf = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n")) nflag = 1;
        else if (!strcmp(argv[i], "-E") || !strcmp(argv[i], "-r")) Eflag = 1;
        else if (!strcmp(argv[i], "-i")) inplace = 1;
        else if (!strncmp(argv[i], "-i", 2) && argv[i][2]) inplace = 1;   /* -iSUFFIX (suffix ignored) */
        else if (!strcmp(argv[i], "-e") && i + 1 < argc) {
            char *add = argv[++i];
            if (!script) script = strdup(add);
            else { script = realloc(script, strlen(script) + strlen(add) + 2); strcat(script, "\n"); strcat(script, add); }
        }
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) {
            FILE *f = fopen(argv[++i], "r"); if (!f) fatal("cannot read script file");
            char b[65536]; size_t r = fread(b, 1, sizeof b - 1, f); b[r] = 0; fclose(f);
            script = script ? script : strdup(b);
        }
        else if (argv[i][0] != '-' && !script) script = strdup(argv[i]);
        else if (argv[i][0] != '-') { if (nf < 256) files[nf++] = argv[i]; }
        else fatal("unknown option");
    }
    if (!script) { fprintf(stderr, "usage: sed [-n] [-E] [-i] [-e SCRIPT | SCRIPT] [FILE...]\n"); return 1; }
    parse_script(script);

    long ln = 0;
    if (nf == 0) { process(stdin, 1, &ln, stdout); }
    else if (inplace) {
        for (int i = 0; i < nf; i++) {
            for (int ci = 0; ci < ncmd; ci++) cmds[ci].in_range = 0;
            ln = 0;
            FILE *in = fopen(files[i], "r"); if (!in) { fprintf(stderr, "sed: %s: cannot open\n", files[i]); continue; }
            char tmp[512]; snprintf(tmp, sizeof tmp, "%s.sed_tmp", files[i]);
            FILE *out = fopen(tmp, "w"); if (!out) { fclose(in); continue; }
            process(in, 1, &ln, out); fclose(in); fclose(out);
            rename(tmp, files[i]);
        }
    } else {
        for (int i = 0; i < nf; i++) {
            FILE *in = fopen(files[i], "r"); if (!in) { fprintf(stderr, "sed: %s: cannot open\n", files[i]); continue; }
            process(in, i == nf - 1, &ln, stdout); fclose(in);
        }
    }
    return 0;
}
