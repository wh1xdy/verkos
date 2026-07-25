/* vawk — VerkOS' own awk (a real, if focused, awk interpreter).
 *
 * Lexer + recursive-descent parser + tree-walking evaluator. Supports:
 *   - rules: pattern { action }, BEGIN, END, /regex/, expr, and pat1,pat2 ranges
 *   - fields $0 $1..$NF and NF NR FNR FS OFS ORS RS with -F and -v
 *   - values with awk's number/string duck typing; assoc arrays a[k]; in
 *   - operators + - * / % ^, comparisons, && || !, ternary ?:, ~ !~, concat,
 *     ++ -- and the assignment ops (= += -= *= /= %=)
 *   - statements print printf if/else while for for-in do next exit {} and
 *     expressions; ; and newline separated
 *   - builtins length substr index split sub gsub match sprintf tolower toupper
 *     sin cos atan2 exp log sqrt int rand srand system
 *
 *   awk [-F sep] [-v var=val] 'PROGRAM' [FILE...]   |   awk -f PROG.awk ...
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <regex.h>

/* ===== values ============================================================ */
typedef struct { double num; char *str; int isnum; } Val;   /* isnum: valid num; str may coexist */
static Val vnum(double n) { Val v; v.num = n; v.str = NULL; v.isnum = 1; return v; }
static Val vstr(const char *s) { Val v; v.num = 0; v.str = strdup(s ? s : ""); v.isnum = 0; return v; }
static double to_num(Val v) { if (v.isnum) return v.num; return v.str ? atof(v.str) : 0; }
static const char *to_str(Val v, char *buf) {
    if (!v.isnum) return v.str ? v.str : "";
    if (v.num == (long)v.num) sprintf(buf, "%ld", (long)v.num); else sprintf(buf, "%.6g", v.num);
    return buf;
}
static int to_bool(Val v) { if (v.isnum) return v.num != 0; return v.str && v.str[0]; }

/* ===== lexer ============================================================= */
enum { T_EOF, T_NUM, T_STR, T_ERE, T_ID, T_FUNC, T_BUILTIN, T_KW,
       T_LB, T_RB, T_LP, T_RP, T_LK, T_RK, T_SEMI, T_NL, T_COMMA,
       T_ASSIGN, T_ADDA, T_SUBA, T_MULA, T_DIVA, T_MODA,
       T_OR, T_AND, T_NOT, T_LT, T_LE, T_GT, T_GE, T_EQ, T_NE, T_MATCH, T_NMATCH,
       T_PLUS, T_MINUS, T_MUL, T_DIV, T_MOD, T_POW, T_DOLLAR, T_INCR, T_DECR, T_Q, T_COLON };
static const char *KW[] = { "BEGIN","END","if","else","while","for","do","next","exit","print","printf","in","break","continue","delete","getline",0 };
static const char *BI[] = { "length","substr","index","split","sub","gsub","match","sprintf","tolower","toupper","sin","cos","atan2","exp","log","sqrt","int","rand","srand","system",0 };

typedef struct { int type; char *s; double num; } Tok;
static const char *P;                 /* program cursor */
static Tok cur, ahead; static int have_ahead;

static Tok lex(void) {
    Tok t; t.s = NULL; t.num = 0;
    while (*P == ' ' || *P == '\t' || (*P == '\\' && P[1] == '\n')) P += (*P == '\\') ? 2 : 1;
    if (*P == '#') { while (*P && *P != '\n') P++; }
    if (*P == 0) { t.type = T_EOF; return t; }
    if (*P == '\n') { P++; t.type = T_NL; return t; }
    if (isdigit((unsigned char)*P) || (*P == '.' && isdigit((unsigned char)P[1]))) { char *e; t.num = strtod(P, &e); P = e; t.type = T_NUM; return t; }
    if (*P == '"') { P++; char buf[4096]; int n = 0; while (*P && *P != '"') { if (*P == '\\') { P++; char c = *P++; t.type = 0; buf[n++] = c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c == '\\' ? '\\' : c == '"' ? '"' : c == '/' ? '/' : c; } else buf[n++] = *P++; } if (*P == '"') P++; buf[n] = 0; t.type = T_STR; t.s = strdup(buf); return t; }
    if (isalpha((unsigned char)*P) || *P == '_') {
        char buf[256]; int n = 0; while (isalnum((unsigned char)*P) || *P == '_') buf[n++] = *P++; buf[n] = 0;
        for (int i = 0; KW[i]; i++) if (!strcmp(buf, KW[i])) { t.type = T_KW; t.s = strdup(buf); return t; }
        for (int i = 0; BI[i]; i++) if (!strcmp(buf, BI[i])) { t.type = T_BUILTIN; t.s = strdup(buf); return t; }
        const char *q = P; while (*q == ' ') q++;
        t.type = (*q == '(') ? T_FUNC : T_ID; t.s = strdup(buf); return t;
    }
    char c = *P++;
    switch (c) {
        case '{': t.type = T_LB; return t; case '}': t.type = T_RB; return t;
        case '(': t.type = T_LP; return t; case ')': t.type = T_RP; return t;
        case '[': t.type = T_LK; return t; case ']': t.type = T_RK; return t;
        case ';': t.type = T_SEMI; return t; case ',': t.type = T_COMMA; return t;
        case '?': t.type = T_Q; return t; case ':': t.type = T_COLON; return t;
        case '$': t.type = T_DOLLAR; return t;
        case '+': if (*P=='+'){P++;t.type=T_INCR;} else if(*P=='='){P++;t.type=T_ADDA;} else t.type=T_PLUS; return t;
        case '-': if (*P=='-'){P++;t.type=T_DECR;} else if(*P=='='){P++;t.type=T_SUBA;} else t.type=T_MINUS; return t;
        case '*': if (*P=='='){P++;t.type=T_MULA;} else t.type=T_MUL; return t;
        case '/': if (*P=='='){P++;t.type=T_DIVA;} else t.type=T_DIV; return t;
        case '%': if (*P=='='){P++;t.type=T_MODA;} else t.type=T_MOD; return t;
        case '^': t.type = T_POW; return t;
        case '=': if (*P=='='){P++;t.type=T_EQ;} else t.type=T_ASSIGN; return t;
        case '!': if (*P=='='){P++;t.type=T_NE;} else if(*P=='~'){P++;t.type=T_NMATCH;} else t.type=T_NOT; return t;
        case '<': if (*P=='='){P++;t.type=T_LE;} else t.type=T_LT; return t;
        case '>': if (*P=='='){P++;t.type=T_GE;} else t.type=T_GT; return t;
        case '~': t.type = T_MATCH; return t;
        case '|': if (*P=='|'){P++;t.type=T_OR;} return t;
        case '&': if (*P=='&'){P++;t.type=T_AND;} return t;
    }
    t.type = T_EOF; return t;
}
/* ERE context: after certain tokens, '/' begins a regex */
static int regex_ok;
static Tok lex_maybe_regex(void) {
    while (*P == ' ' || *P == '\t') P++;
    if (regex_ok && *P == '/') { P++; char buf[1024]; int n = 0; while (*P && *P != '/') { if (*P == '\\') buf[n++] = *P++; buf[n++] = *P++; } if (*P == '/') P++; buf[n] = 0; Tok t; t.type = T_ERE; t.s = strdup(buf); t.num = 0; return t; }
    return lex();
}
static void set_regctx(int t) { regex_ok = (t==0||t==T_LP||t==T_COMMA||t==T_SEMI||t==T_NL||t==T_LB||t==T_AND||t==T_OR||t==T_NOT||t==T_MATCH||t==T_NMATCH||t==T_KW); }
static void advance(void) { if (have_ahead) { cur = ahead; have_ahead = 0; } else { set_regctx(cur.type); cur = lex_maybe_regex(); } }
static Tok peek(void) { if (!have_ahead) { int sc = regex_ok; set_regctx(cur.type); ahead = lex_maybe_regex(); regex_ok = sc; have_ahead = 1; } return ahead; }

/* ===== AST ============================================================== */
enum { N_NUM, N_STR, N_ERE, N_VAR, N_FIELD, N_IDX, N_ASSIGN, N_BIN, N_UN, N_POST, N_MATCH, N_TERN,
       N_CALL, N_BUILTIN, N_GROUP, N_IN, N_CONCAT,
       S_PRINT, S_PRINTF, S_IF, S_WHILE, S_FOR, S_FORIN, S_BLOCK, S_EXPR, S_NEXT, S_EXIT, S_DELETE, S_DO, S_BREAK, S_CONT };
typedef struct Node {
    int kind, op;
    double num; char *str;
    struct Node *a, *b, *c, *d;
    struct Node **list; int nlist;
} Node;
static Node *nnew(int k) { Node *n = calloc(1, sizeof *n); n->kind = k; return n; }
static void addlist(Node *n, Node *e) { n->list = realloc(n->list, (n->nlist + 1) * sizeof *n->list); n->list[n->nlist++] = e; }

/* forward */
static Node *parse_expr(void);
static Node *parse_stmt(void);

static void expect(int t) { if (cur.type != t) { fprintf(stderr, "awk: syntax error (got token %d, want %d)\n", cur.type, t); exit(2); } advance(); }
static void skipnl(void) { while (cur.type == T_NL || cur.type == T_SEMI) advance(); }

static Node *parse_primary(void) {
    Node *n;
    if (cur.type == T_NUM) { n = nnew(N_NUM); n->num = cur.num; advance(); return n; }
    if (cur.type == T_STR) { n = nnew(N_STR); n->str = cur.s; advance(); return n; }
    if (cur.type == T_ERE) { n = nnew(N_ERE); n->str = cur.s; advance(); return n; }
    if (cur.type == T_DOLLAR) { advance(); n = nnew(N_FIELD); n->a = parse_primary(); return n; }
    if (cur.type == T_NOT) { advance(); n = nnew(N_UN); n->op = T_NOT; n->a = parse_primary(); return n; }
    if (cur.type == T_MINUS) { advance(); n = nnew(N_UN); n->op = T_MINUS; n->a = parse_primary(); return n; }
    if (cur.type == T_PLUS) { advance(); return parse_primary(); }
    if (cur.type == T_INCR || cur.type == T_DECR) { int o = cur.type; advance(); n = nnew(N_UN); n->op = o; n->a = parse_primary(); return n; }
    if (cur.type == T_LP) { advance(); n = parse_expr(); expect(T_RP); Node *g = nnew(N_GROUP); g->a = n; return g; }
    if (cur.type == T_BUILTIN) { n = nnew(N_BUILTIN); n->str = cur.s; advance(); if (cur.type == T_LP) { advance(); while (cur.type != T_RP && cur.type != T_EOF) { addlist(n, parse_expr()); if (cur.type == T_COMMA) advance(); } expect(T_RP); } return n; }
    if (cur.type == T_FUNC) { n = nnew(N_CALL); n->str = cur.s; advance(); expect(T_LP); while (cur.type != T_RP && cur.type != T_EOF) { addlist(n, parse_expr()); if (cur.type == T_COMMA) advance(); } expect(T_RP); return n; }
    if (cur.type == T_ID) {
        char *name = cur.s; advance();
        if (cur.type == T_LK) { advance(); n = nnew(N_IDX); n->str = name; n->a = parse_expr(); expect(T_RK); return n; }
        n = nnew(N_VAR); n->str = name; return n;
    }
    n = nnew(N_NUM); n->num = 0; advance(); return n;
}
static Node *parse_post(void) {
    Node *n = parse_primary();
    while (cur.type == T_INCR || cur.type == T_DECR) { Node *p = nnew(N_POST); p->op = cur.type; p->a = n; advance(); n = p; }
    return n;
}
static Node *parse_pow(void) { Node *n = parse_post(); if (cur.type == T_POW) { advance(); Node *b = nnew(N_BIN); b->op = T_POW; b->a = n; b->b = parse_pow(); return b; } return n; }
static Node *parse_mul(void) { Node *n = parse_pow(); while (cur.type == T_MUL || cur.type == T_DIV || cur.type == T_MOD) { int o = cur.type; advance(); Node *b = nnew(N_BIN); b->op = o; b->a = n; b->b = parse_pow(); n = b; } return n; }
static Node *parse_add(void) { Node *n = parse_mul(); while (cur.type == T_PLUS || cur.type == T_MINUS) { int o = cur.type; advance(); Node *b = nnew(N_BIN); b->op = o; b->a = n; b->b = parse_mul(); n = b; } return n; }
/* string concatenation: adjacent expressions */
static int starts_value(int t) { return t==T_NUM||t==T_STR||t==T_ERE||t==T_ID||t==T_FUNC||t==T_BUILTIN||t==T_DOLLAR||t==T_LP||t==T_NOT||t==T_INCR||t==T_DECR||t==T_MINUS; }
static Node *parse_concat(void) { Node *n = parse_add(); while (starts_value(cur.type) && cur.type != T_MINUS) { Node *c = nnew(N_CONCAT); c->a = n; c->b = parse_add(); n = c; } return n; }
static Node *parse_rel(void) {
    Node *n = parse_concat();
    if (cur.type==T_LT||cur.type==T_LE||cur.type==T_GT||cur.type==T_GE||cur.type==T_EQ||cur.type==T_NE) { int o=cur.type; advance(); Node *b=nnew(N_BIN); b->op=o; b->a=n; b->b=parse_concat(); return b; }
    if (cur.type==T_MATCH||cur.type==T_NMATCH) { int o=cur.type; advance(); Node *m=nnew(N_MATCH); m->op=o; m->a=n; m->b=parse_concat(); return m; }
    if (cur.type==T_KW && !strcmp(cur.s,"in")) { advance(); Node *in=nnew(N_IN); in->a=n; in->str=cur.s; expect(T_ID); return in; }
    return n;
}
static Node *parse_and(void) { Node *n = parse_rel(); while (cur.type == T_AND) { advance(); skipnl(); Node *b = nnew(N_BIN); b->op = T_AND; b->a = n; b->b = parse_rel(); n = b; } return n; }
static Node *parse_or(void) { Node *n = parse_and(); while (cur.type == T_OR) { advance(); skipnl(); Node *b = nnew(N_BIN); b->op = T_OR; b->a = n; b->b = parse_and(); n = b; } return n; }
static Node *parse_tern(void) { Node *n = parse_or(); if (cur.type == T_Q) { advance(); Node *t = nnew(N_TERN); t->a = n; t->b = parse_expr(); expect(T_COLON); t->c = parse_expr(); return t; } return n; }
static Node *parse_expr(void) {
    Node *n = parse_tern();
    if (cur.type==T_ASSIGN||cur.type==T_ADDA||cur.type==T_SUBA||cur.type==T_MULA||cur.type==T_DIVA||cur.type==T_MODA) {
        int o = cur.type; advance(); Node *a = nnew(N_ASSIGN); a->op = o; a->a = n; a->b = parse_expr(); return a;
    }
    return n;
}

static Node *parse_simple_stmt(void) {
    if (cur.type == T_KW) {
        char *k = cur.s;
        if (!strcmp(k,"print")||!strcmp(k,"printf")) { int pf=!strcmp(k,"printf"); advance(); Node *n=nnew(pf?S_PRINTF:S_PRINT); while (cur.type!=T_SEMI&&cur.type!=T_NL&&cur.type!=T_RB&&cur.type!=T_EOF&&cur.type!=T_GT) { addlist(n,parse_expr()); if (cur.type==T_COMMA) advance(); else break; } if (cur.type==T_GT){advance(); n->a=parse_expr();} return n; }
        if (!strcmp(k,"next")) { advance(); return nnew(S_NEXT); }
        if (!strcmp(k,"break")) { advance(); return nnew(S_BREAK); }
        if (!strcmp(k,"continue")) { advance(); return nnew(S_CONT); }
        if (!strcmp(k,"exit")) { advance(); Node *n=nnew(S_EXIT); if (starts_value(cur.type)) n->a=parse_expr(); return n; }
        if (!strcmp(k,"delete")) { advance(); Node *n=nnew(S_DELETE); n->str=cur.s; advance(); if (cur.type==T_LK){advance(); n->a=parse_expr(); expect(T_RK);} return n; }
    }
    Node *n = nnew(S_EXPR); n->a = parse_expr(); return n;
}
static Node *parse_stmt(void) {
    skipnl();
    if (cur.type == T_LB) { advance(); Node *b = nnew(S_BLOCK); skipnl(); while (cur.type != T_RB && cur.type != T_EOF) { addlist(b, parse_stmt()); skipnl(); } expect(T_RB); return b; }
    if (cur.type == T_KW) {
        char *k = cur.s;
        if (!strcmp(k,"if")) { advance(); expect(T_LP); Node *n=nnew(S_IF); n->a=parse_expr(); expect(T_RP); skipnl(); n->b=parse_stmt(); Tok sv; int p=(int)(P-P); (void)sv;(void)p; skipnl(); if (cur.type==T_KW && !strcmp(cur.s,"else")) { advance(); skipnl(); n->c=parse_stmt(); } return n; }
        if (!strcmp(k,"while")) { advance(); expect(T_LP); Node *n=nnew(S_WHILE); n->a=parse_expr(); expect(T_RP); skipnl(); n->b=parse_stmt(); return n; }
        if (!strcmp(k,"do")) { advance(); skipnl(); Node *n=nnew(S_DO); n->b=parse_stmt(); skipnl(); if (cur.type==T_KW&&!strcmp(cur.s,"while")) advance(); expect(T_LP); n->a=parse_expr(); expect(T_RP); return n; }
        if (!strcmp(k,"for")) {
            advance(); expect(T_LP);
            if (cur.type==T_LP) { /* rare */ }
            if (cur.type==T_ID && peek().type==T_KW) {  /* for (x in arr) */
                Node *n=nnew(S_FORIN); n->str=cur.s; advance(); advance(); n->b=nnew(N_VAR); n->b->str=cur.s; advance(); expect(T_RP); skipnl(); n->c=parse_stmt(); return n;
            }
            Node *n=nnew(S_FOR); if (cur.type!=T_SEMI) n->a=parse_simple_stmt(); expect(T_SEMI); if (cur.type!=T_SEMI) n->b=parse_expr(); expect(T_SEMI); if (cur.type!=T_RP) n->c=parse_simple_stmt(); expect(T_RP); skipnl(); n->d=parse_stmt(); return n;
        }
    }
    Node *n = parse_simple_stmt();
    return n;
}

/* rules */
typedef struct { Node *pat, *pat2, *act; int begin, end, inrange; } Rule;
static Rule rules[256]; static int nrules;

static void parse_program(void) {
    advance();
    skipnl();
    while (cur.type != T_EOF) {
        Rule *r = &rules[nrules]; memset(r, 0, sizeof *r);
        if (cur.type == T_KW && !strcmp(cur.s,"BEGIN")) { r->begin = 1; advance(); }
        else if (cur.type == T_KW && !strcmp(cur.s,"END")) { r->end = 1; advance(); }
        else if (cur.type != T_LB) { r->pat = parse_expr(); if (cur.type == T_COMMA) { advance(); r->pat2 = parse_expr(); } }
        skipnl();
        if (cur.type == T_LB) r->act = parse_stmt();
        nrules++;
        skipnl();
    }
}

/* ===== runtime ========================================================== */
typedef struct Ent { char *key; Val v; struct Ent *next; } Ent;
#define HN 1021
typedef struct { Ent *b[HN]; } Map;
static Map globals;
static Map *arrays[512]; static char *arrnames[512]; static int narr;
static unsigned hash(const char *s) { unsigned h = 5381; while (*s) h = h * 33 + (unsigned char)*s++; return h % HN; }
static Val *map_get(Map *m, const char *k) { unsigned h = hash(k); for (Ent *e = m->b[h]; e; e = e->next) if (!strcmp(e->key, k)) return &e->v; Ent *e = calloc(1, sizeof *e); e->key = strdup(k); e->v = vstr(""); e->next = m->b[h]; m->b[h] = e; return &e->v; }
static int map_has(Map *m, const char *k) { unsigned h = hash(k); for (Ent *e = m->b[h]; e; e = e->next) if (!strcmp(e->key, k)) return 1; return 0; }
static void map_del(Map *m, const char *k) { unsigned h = hash(k); Ent **pp = &m->b[h]; while (*pp) { if (!strcmp((*pp)->key, k)) { Ent *d = *pp; *pp = d->next; free(d->key); free(d); return; } pp = &(*pp)->next; } }
static Map *get_array(const char *name) { for (int i = 0; i < narr; i++) if (!strcmp(arrnames[i], name)) return arrays[i]; arrays[narr] = calloc(1, sizeof(Map)); arrnames[narr] = strdup(name); return arrays[narr++]; }

/* fields */
static char *g_line; static char *fields[256]; static int NF;
static char FS_[64] = " ", OFS[64] = " ", ORS[64] = "\n";
static long NR, FNR;
static void split_fields(void) {
    for (int i = 1; i <= NF; i++) { free(fields[i]); fields[i] = NULL; }
    NF = 0; char *s = g_line;
    if (!strcmp(FS_, " ")) {
        while (*s) { while (*s == ' ' || *s == '\t' || *s == '\n') s++; if (!*s) break; char *st = s; while (*s && *s != ' ' && *s != '\t' && *s != '\n') s++; int l = s - st; fields[++NF] = malloc(l + 1); memcpy(fields[NF], st, l); fields[NF][l] = 0; }
    } else if (FS_[1] == 0) {
        char fc = FS_[0]; char *st = s;
        for (;; s++) { if (*s == fc || *s == 0) { int l = s - st; fields[++NF] = malloc(l + 1); memcpy(fields[NF], st, l); fields[NF][l] = 0; if (!*s) break; st = s + 1; } }
    } else {
        /* POSIX: an FS of more than one character is an extended regular
         * expression, not a literal. Using only FS_[0] meant -F'[(, ]+' split
         * on '[', so every field after the first came out empty — a wrong
         * answer with a zero exit status. GCC's own build generates its
         * aarch64 tune table this way and stopped because the table came out
         * empty; that is how this was found. */
        regex_t fre;
        if (regcomp(&fre, FS_, REG_EXTENDED)) {
            char fc = FS_[0]; char *st = s;
            for (;; s++) { if (*s == fc || *s == 0) { int l = s - st; fields[++NF] = malloc(l + 1); memcpy(fields[NF], st, l); fields[NF][l] = 0; if (!*s) break; st = s + 1; } }
            return;
        }
        char *p = s;
        regmatch_t m;
        while (*p && regexec(&fre, p, 1, &m, p == s ? 0 : REG_NOTBOL) == 0) {
            if (m.rm_eo == m.rm_so) break;          /* empty match: no progress */
            int l = (int)m.rm_so;
            fields[++NF] = malloc((size_t)l + 1);
            memcpy(fields[NF], p, (size_t)l); fields[NF][l] = 0;
            p += m.rm_eo;
            if (NF >= 250) break;
        }
        int l = (int)strlen(p);
        fields[++NF] = malloc((size_t)l + 1);
        memcpy(fields[NF], p, (size_t)l); fields[NF][l] = 0;
        regfree(&fre);
    }
}
static void set_line(const char *l) { free(g_line); g_line = strdup(l); split_fields(); }
static void rebuild_line(void) { char buf[8192]; int n = 0; for (int i = 1; i <= NF; i++) { if (i > 1) { strcpy(buf + n, OFS); n += strlen(OFS); } const char *f = fields[i] ? fields[i] : ""; strcpy(buf + n, f); n += strlen(f); } buf[n] = 0; free(g_line); g_line = strdup(buf); }
static const char *get_field(int i) { if (i == 0) return g_line ? g_line : ""; if (i >= 1 && i <= NF) return fields[i] ? fields[i] : ""; return ""; }
static void set_field(int i, const char *v) { if (i == 0) { set_line(v); return; } while (NF < i) { fields[++NF] = strdup(""); } free(fields[i]); fields[i] = strdup(v); rebuild_line(); }

static Val eval(Node *n);
static int exit_flag, next_flag, break_flag, cont_flag; static int exit_code;

static double special_num(const char *name, int *is) {
    *is = 1;
    if (!strcmp(name,"NR")) return NR;
    if (!strcmp(name,"NF")) return NF;
    if (!strcmp(name,"FNR")) return FNR;
    *is = 0; return 0;
}
static Val get_var(const char *name) {
    int is; double d = special_num(name, &is); if (is) return vnum(d);
    if (!strcmp(name,"FS")) return vstr(FS_);
    if (!strcmp(name,"OFS")) return vstr(OFS);
    if (!strcmp(name,"ORS")) return vstr(ORS);
    return *map_get(&globals, name);
}
static void set_var(const char *name, Val v) {
    char b[64];
    if (!strcmp(name,"NF")) { NF = (int)to_num(v); rebuild_line(); return; }
    if (!strcmp(name,"FS")) { snprintf(FS_, sizeof FS_, "%s", to_str(v, b)); return; }
    if (!strcmp(name,"OFS")) { snprintf(OFS, sizeof OFS, "%s", to_str(v, b)); return; }
    if (!strcmp(name,"ORS")) { snprintf(ORS, sizeof ORS, "%s", to_str(v, b)); return; }
    Val *slot = map_get(&globals, name); free(slot->str); *slot = v.isnum ? vnum(v.num) : vstr(v.str);
}

static int rx_match(const char *s, const char *pat) { regex_t re; if (regcomp(&re, pat, REG_EXTENDED)) return 0; int r = regexec(&re, s, 0, NULL, 0) == 0; regfree(&re); return r; }

static Val call_builtin(Node *n) {
    char b1[64], b2[64];
    const char *name = n->str;
    #define ARG(i) (n->nlist > (i) ? eval(n->list[i]) : vstr(""))
    if (!strcmp(name,"length")) { if (n->nlist == 0) return vnum(strlen(g_line ? g_line : "")); Val a = ARG(0); return vnum(strlen(to_str(a, b1))); }
    if (!strcmp(name,"substr")) { Val s = ARG(0); const char *str = to_str(s, b1); int len = strlen(str); int m = (int)to_num(ARG(1)); int l = n->nlist > 2 ? (int)to_num(ARG(2)) : len; if (m < 1) { l += m - 1; m = 1; } if (m > len) return vstr(""); if (l < 0) l = 0; if (m - 1 + l > len) l = len - (m - 1); char *o = malloc(l + 1); memcpy(o, str + m - 1, l); o[l] = 0; Val v = vstr(o); free(o); return v; }
    if (!strcmp(name,"index")) { Val a = ARG(0), b = ARG(1); const char *h = to_str(a, b1); char *p = strstr(h, to_str(b, b2)); return vnum(p ? p - h + 1 : 0); }
    if (!strcmp(name,"toupper")) { Val a = ARG(0); char *s = strdup(to_str(a, b1)); for (char *p = s; *p; p++) *p = toupper((unsigned char)*p); Val v = vstr(s); free(s); return v; }
    if (!strcmp(name,"tolower")) { Val a = ARG(0); char *s = strdup(to_str(a, b1)); for (char *p = s; *p; p++) *p = tolower((unsigned char)*p); Val v = vstr(s); free(s); return v; }
    if (!strcmp(name,"int")) return vnum((double)(long)to_num(ARG(0)));
    if (!strcmp(name,"sqrt")) return vnum(sqrt(to_num(ARG(0))));
    if (!strcmp(name,"sin")) return vnum(sin(to_num(ARG(0))));
    if (!strcmp(name,"cos")) return vnum(cos(to_num(ARG(0))));
    if (!strcmp(name,"exp")) return vnum(exp(to_num(ARG(0))));
    if (!strcmp(name,"log")) return vnum(log(to_num(ARG(0))));
    if (!strcmp(name,"atan2")) return vnum(atan2(to_num(ARG(0)), to_num(ARG(1))));
    if (!strcmp(name,"rand")) return vnum((double)rand() / ((double)RAND_MAX + 1));
    if (!strcmp(name,"srand")) { srand(n->nlist ? (unsigned)to_num(ARG(0)) : (unsigned)time(NULL)); return vnum(0); }
    if (!strcmp(name,"system")) { Val a = ARG(0); return vnum(system(to_str(a, b1))); }
    if (!strcmp(name,"match")) { Val a = ARG(0), b = ARG(1); regex_t re; regmatch_t m; if (regcomp(&re, to_str(b, b2), REG_EXTENDED)) return vnum(0); int r = regexec(&re, to_str(a, b1), 1, &m, 0); regfree(&re); if (r) { set_var("RSTART", vnum(0)); set_var("RLENGTH", vnum(-1)); return vnum(0); } set_var("RSTART", vnum(m.rm_so + 1)); set_var("RLENGTH", vnum(m.rm_eo - m.rm_so)); return vnum(m.rm_so + 1); }
    if (!strcmp(name,"sprintf")) { char out[8192]; char fmt[1024]; Val f = ARG(0); snprintf(fmt, sizeof fmt, "%s", to_str(f, b1)); int oi = 0, ai = 1; for (char *p = fmt; *p; ) { if (*p != '%') { out[oi++] = *p++; continue; } char spec[64]; int si = 0; spec[si++] = *p++; while (*p && !strchr("diouxXeEfgGcs%", *p)) spec[si++] = *p++; if (*p) spec[si++] = *p; spec[si] = 0; char conv = *p ? *p++ : 's'; if (conv == '%') { out[oi++] = '%'; continue; } Val a = ai < n->nlist ? eval(n->list[ai++]) : vstr(""); if (conv == 's') oi += sprintf(out + oi, spec, to_str(a, b2)); else if (conv == 'c') { char cc[2]; const char *sv = a.isnum ? NULL : a.str; cc[0] = a.isnum ? (char)(long)a.num : (sv ? sv[0] : 0); cc[1] = 0; strcpy(spec + si - 1, "s"); oi += sprintf(out + oi, spec, cc); } else if (strchr("diouxX", conv)) oi += sprintf(out + oi, spec, (long)to_num(a)); else oi += sprintf(out + oi, spec, to_num(a)); } out[oi] = 0; return vstr(out); }
    if (!strcmp(name,"split")) { Val s = ARG(0); const char *str = strdup(to_str(s, b1)); const char *an = n->list[1]->kind == N_VAR ? n->list[1]->str : "SPLIT"; Map *arr = get_array(an); for (int i = 0; i < HN; i++) { while (arr->b[i]) { Ent *e = arr->b[i]; arr->b[i] = e->next; free(e->key); free(e); } } char sep = n->nlist > 2 ? (n->list[2]->kind==N_ERE ? n->list[2]->str[0] : to_str(eval(n->list[2]), b2)[0]) : (FS_[0] == ' ' ? ' ' : FS_[0]); int cnt = 0; char *cp = strdup(str), *tok, key[32]; if (sep == ' ') { tok = strtok(cp, " \t\n"); while (tok) { sprintf(key, "%d", ++cnt); *map_get(arr, key) = vstr(tok); tok = strtok(NULL, " \t\n"); } } else { char *st = cp; for (char *q = cp;; q++) { if (*q == sep || *q == 0) { char sc = *q; *q = 0; sprintf(key, "%d", ++cnt); *map_get(arr, key) = vstr(st); if (!sc) break; st = q + 1; *q = sep; } } } free(cp); free((void*)str); return vnum(cnt); }
    if (!strcmp(name,"sub") || !strcmp(name,"gsub")) { int global = name[0]=='g'; const char *pat = n->list[0]->kind==N_ERE ? n->list[0]->str : to_str(eval(n->list[0]), b1); Val rv = eval(n->list[1]); char rep[1024]; snprintf(rep, sizeof rep, "%s", to_str(rv, b2)); Node *tgt = n->nlist > 2 ? n->list[2] : NULL; const char *cur_s = tgt ? (tgt->kind==N_FIELD ? get_field((int)to_num(eval(tgt->a))) : to_str(get_var(tgt->str), b1)) : g_line; char work[8192]; snprintf(work, sizeof work, "%s", cur_s ? cur_s : ""); regex_t re; if (regcomp(&re, pat, REG_EXTENDED)) return vnum(0); char out[8192]; int oi = 0, cnt = 0; char *p = work; regmatch_t m; while (regexec(&re, p, 1, &m, p==work?0:REG_NOTBOL) == 0) { memcpy(out + oi, p, m.rm_so); oi += m.rm_so; for (char *r = rep; *r; r++) { if (*r == '&') { memcpy(out+oi, p+m.rm_so, m.rm_eo-m.rm_so); oi += m.rm_eo-m.rm_so; } else if (*r=='\\'&&r[1]=='&'){out[oi++]='&';r++;} else out[oi++] = *r; } cnt++; if (m.rm_eo == m.rm_so) { if (p[m.rm_eo]) out[oi++] = p[m.rm_eo]; p += m.rm_eo + 1; } else p += m.rm_eo; if (!global) break; } strcpy(out + oi, p); regfree(&re); if (cnt) { if (tgt) { if (tgt->kind==N_FIELD) set_field((int)to_num(eval(tgt->a)), out); else set_var(tgt->str, vstr(out)); } else set_line(out); } return vnum(cnt); }
    return vstr("");
    #undef ARG
}

static Val eval(Node *n) {
    char b1[64], b2[64];
    if (!n) return vstr("");
    switch (n->kind) {
        case N_NUM: return vnum(n->num);
        case N_STR: return vstr(n->str);
        case N_ERE: return vnum(rx_match(g_line ? g_line : "", n->str));  /* bare /re/ matches $0 */
        case N_GROUP: return eval(n->a);
        case N_VAR: return get_var(n->str);
        case N_FIELD: return vstr(get_field((int)to_num(eval(n->a))));
        case N_IDX: { Map *a = get_array(n->str); Val k = eval(n->a); const char *ks = to_str(k, b1); return *map_get(a, ks); }
        case N_IN: { Map *a = get_array(n->str); Val k = eval(n->a); return vnum(map_has(a, to_str(k, b1))); }
        case N_CONCAT: { Val a = eval(n->a), b = eval(n->b); char *s = malloc(strlen(to_str(a,b1))+strlen(to_str(b,b2))+1); strcpy(s, to_str(a,b1)); strcat(s, to_str(b,b2)); Val v = vstr(s); free(s); return v; }
        case N_TERN: return to_bool(eval(n->a)) ? eval(n->b) : eval(n->c);
        case N_BUILTIN: return call_builtin(n);
        case N_CALL: return vstr("");   /* user functions not supported */
        case N_MATCH: { Val a = eval(n->a); const char *pat = n->b->kind == N_ERE ? n->b->str : to_str(eval(n->b), b2); int r = rx_match(to_str(a, b1), pat); return vnum(n->op == T_MATCH ? r : !r); }
        case N_UN: { if (n->op == T_NOT) return vnum(!to_bool(eval(n->a))); if (n->op == T_MINUS) return vnum(-to_num(eval(n->a))); if (n->op == T_INCR || n->op == T_DECR) { Val v = eval(n->a); double d = to_num(v) + (n->op==T_INCR?1:-1); Node *t = n->a; if (t->kind==N_VAR) set_var(t->str, vnum(d)); else if (t->kind==N_FIELD) { char bb[64]; set_field((int)to_num(eval(t->a)), (sprintf(bb,"%g",d),bb)); } else if (t->kind==N_IDX) { Map *a=get_array(t->str); *map_get(a,to_str(eval(t->a),b1))=vnum(d); } return vnum(d); } return vstr(""); }
        case N_POST: { Val v = eval(n->a); double old = to_num(v), d = old + (n->op==T_INCR?1:-1); Node *t = n->a; if (t->kind==N_VAR) set_var(t->str, vnum(d)); else if (t->kind==N_FIELD) { char bb[64]; set_field((int)to_num(eval(t->a)), (sprintf(bb,"%g",d),bb)); } else if (t->kind==N_IDX) { Map *a=get_array(t->str); *map_get(a,to_str(eval(t->a),b1))=vnum(d); } return vnum(old); }
        case N_ASSIGN: {
            double base = 0; if (n->op != T_ASSIGN) base = to_num(eval(n->a));
            Val rhs = eval(n->b); double rv = to_num(rhs);
            Val res; switch (n->op) { case T_ADDA: res=vnum(base+rv); break; case T_SUBA: res=vnum(base-rv); break; case T_MULA: res=vnum(base*rv); break; case T_DIVA: res=vnum(base/rv); break; case T_MODA: res=vnum(fmod(base,rv)); break; default: res = rhs.isnum ? vnum(rhs.num) : vstr(rhs.str); }
            Node *t = n->a;
            if (t->kind==N_VAR) set_var(t->str, res);
            else if (t->kind==N_FIELD) set_field((int)to_num(eval(t->a)), to_str(res, b1));
            else if (t->kind==N_IDX) { Map *a=get_array(t->str); Val *slot=map_get(a, to_str(eval(t->a),b2)); free(slot->str); *slot = res.isnum?vnum(res.num):vstr(res.str); }
            return res;
        }
        case N_BIN: {
            int o = n->op;
            if (o == T_AND) return vnum(to_bool(eval(n->a)) && to_bool(eval(n->b)));
            if (o == T_OR) return vnum(to_bool(eval(n->a)) || to_bool(eval(n->b)));
            Val a = eval(n->a), b = eval(n->b);
            if (o==T_PLUS) return vnum(to_num(a)+to_num(b));
            if (o==T_MINUS) return vnum(to_num(a)-to_num(b));
            if (o==T_MUL) return vnum(to_num(a)*to_num(b));
            if (o==T_DIV) return vnum(to_num(a)/to_num(b));
            if (o==T_MOD) return vnum(fmod(to_num(a),to_num(b)));
            if (o==T_POW) return vnum(pow(to_num(a),to_num(b)));
            int cmp; int nn = (a.isnum||!a.str||a.str[0]==0||isdigit((unsigned char)a.str[0])||a.str[0]=='-'||a.str[0]=='.') && (b.isnum);
            if ((a.isnum && b.isnum) || nn) { double x=to_num(a),y=to_num(b); cmp = x<y?-1:x>y?1:0; }
            else cmp = strcmp(to_str(a,b1), to_str(b,b2));
            switch (o) { case T_LT: return vnum(cmp<0); case T_LE: return vnum(cmp<=0); case T_GT: return vnum(cmp>0); case T_GE: return vnum(cmp>=0); case T_EQ: return vnum(cmp==0); case T_NE: return vnum(cmp!=0); }
            return vstr("");
        }
    }
    return vstr("");
}

static void exec_stmt(Node *n);
static void exec_print(Node *n, int pf) {
    char b[64]; FILE *o = stdout;
    char fname[1024];
    if (n->a) { Val f = eval(n->a); o = fopen(to_str(f, fname), "a"); if (!o) o = stdout; }
    if (pf) { if (n->nlist) { Node call; memset(&call,0,sizeof call); call.str=(char*)"sprintf"; call.list=n->list; call.nlist=n->nlist; Val s=call_builtin(&call); fputs(to_str(s,b), o); } }
    else { if (n->nlist == 0) fputs(g_line ? g_line : "", o); else for (int i = 0; i < n->nlist; i++) { if (i) fputs(OFS, o); Val v = eval(n->list[i]); fputs(to_str(v, b), o); } fputs(ORS, o); }
    if (o != stdout) fclose(o);
}
static void exec_stmt(Node *n) {
    if (!n || exit_flag || next_flag || break_flag || cont_flag) return;
    switch (n->kind) {
        case S_BLOCK: for (int i = 0; i < n->nlist; i++) { exec_stmt(n->list[i]); if (exit_flag||next_flag||break_flag||cont_flag) break; } break;
        case S_EXPR: eval(n->a); break;
        case S_PRINT: exec_print(n, 0); break;
        case S_PRINTF: exec_print(n, 1); break;
        case S_IF: if (to_bool(eval(n->a))) exec_stmt(n->b); else if (n->c) exec_stmt(n->c); break;
        case S_WHILE: while (to_bool(eval(n->a))) { exec_stmt(n->b); if (break_flag) { break_flag = 0; break; } cont_flag = 0; if (exit_flag||next_flag) break; } break;
        case S_DO: do { exec_stmt(n->b); if (break_flag) { break_flag = 0; break; } cont_flag = 0; if (exit_flag||next_flag) break; } while (to_bool(eval(n->a))); break;
        case S_FOR: if (n->a) exec_stmt(n->a); while (!n->b || to_bool(eval(n->b))) { exec_stmt(n->d); if (break_flag) { break_flag = 0; break; } cont_flag = 0; if (exit_flag||next_flag) break; if (n->c) exec_stmt(n->c); } break;
        case S_FORIN: { Map *a = get_array(n->b->str); for (int i = 0; i < HN; i++) for (Ent *e = a->b[i]; e; e = e->next) { set_var(n->str, vstr(e->key)); exec_stmt(n->c); if (break_flag) { break_flag = 0; goto forin_done; } cont_flag = 0; if (exit_flag||next_flag) goto forin_done; } forin_done: break; }
        case S_NEXT: next_flag = 1; break;
        case S_BREAK: break_flag = 1; break;
        case S_CONT: cont_flag = 1; break;
        case S_EXIT: if (n->a) exit_code = (int)to_num(eval(n->a)); exit_flag = 1; break;
        case S_DELETE: { Map *a = get_array(n->str); if (n->a) map_del(a, to_str(eval(n->a), (char[64]){0})); else for (int i=0;i<HN;i++){ while(a->b[i]){Ent*e=a->b[i];a->b[i]=e->next;free(e->key);free(e);} } break; }
        default: break;
    }
}

static int eval_pattern(Rule *r) {
    if (r->pat && !r->pat2) return to_bool(eval(r->pat));
    if (r->pat && r->pat2) {
        if (!r->inrange) { if (to_bool(eval(r->pat))) { r->inrange = 1; if (to_bool(eval(r->pat2))) r->inrange = 0; return 1; } return 0; }
        else { if (to_bool(eval(r->pat2))) r->inrange = 0; return 1; }
    }
    return 1;
}
static void run_record(void) {
    NR++; FNR++;
    for (int i = 0; i < nrules && !exit_flag; i++) {
        Rule *r = &rules[i]; if (r->begin || r->end) continue;
        if (eval_pattern(r)) { if (r->act) exec_stmt(r->act); else { char b[64]; fputs(g_line?g_line:"", stdout); (void)b; fputs(ORS, stdout); } }
        if (next_flag) { next_flag = 0; break; }
    }
}

int main(int argc, char **argv) {
    const char *prog = NULL; char *files[512]; int nf = 0;
    char preassign[64][256]; int npre = 0;
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-F") && i+1 < argc) { snprintf(FS_, sizeof FS_, "%s", argv[++i]); }
        else if (!strncmp(argv[i], "-F", 2) && argv[i][2]) snprintf(FS_, sizeof FS_, "%s", argv[i]+2);
        else if (!strcmp(argv[i], "-v") && i+1 < argc) snprintf(preassign[npre++], 256, "%s", argv[++i]);
        else if (!strcmp(argv[i], "-f") && i+1 < argc) { FILE *f = fopen(argv[++i], "r"); if (!f) { fprintf(stderr,"awk: cannot open program\n"); return 2; } static char pbuf[65536]; size_t r = fread(pbuf,1,sizeof pbuf-1,f); pbuf[r]=0; fclose(f); prog = pbuf; }
        /* "--" ends the options; anything else starting with '-' is a mistyped
         * option, not a program. Taking it as the program hid the typo. */
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "awk: unknown option %s\n", argv[i]); return 2;
        }
        else break;
    }
    if (!prog) { if (i >= argc) { fprintf(stderr, "usage: awk [-F sep] [-v v=x] 'PROGRAM' [FILE...]\n"); return 2; } prog = argv[i++]; }
    for (; i < argc; i++) files[nf++] = argv[i];

    P = prog; parse_program();
    srand(1);
    for (int k = 0; k < npre; k++) { char *eq = strchr(preassign[k], '='); if (eq) { *eq = 0; set_var(preassign[k], vstr(eq+1)); } }

    for (int r = 0; r < nrules && !exit_flag; r++) if (rules[r].begin) exec_stmt(rules[r].act);

    /* An input file we could not read is a failure, not an empty file. */
    int file_error = 0;
    int has_main = 0, has_end = 0;
    for (int r = 0; r < nrules; r++) { if (!rules[r].begin && !rules[r].end) has_main = 1; if (rules[r].end) has_end = 1; }
    if ((has_main || has_end) && !exit_flag) {
        char *line = NULL; size_t cap = 0; ssize_t rd;
        int fi = 0;
        FILE *in = nf ? NULL : stdin;
        do {
            if (nf) { in = fopen(files[fi], "r"); if (!in) { fprintf(stderr, "awk: %s: cannot open\n", files[fi]); file_error = 2; continue; } FNR = 0; }
            if (in) while ((rd = getline(&line, &cap, in)) != -1) { if (rd && line[rd-1]=='\n') line[rd-1]=0; set_line(line); run_record(); if (exit_flag) break; }
            if (nf && in) fclose(in);
        } while (nf && ++fi < nf && !exit_flag);
        free(line);
    }
    exit_flag = 0;
    for (int r = 0; r < nrules; r++) if (rules[r].end) exec_stmt(rules[r].act);
    return exit_code ? exit_code : file_error;
}
