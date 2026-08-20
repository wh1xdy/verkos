/* Turning sav source into tokens, and the diagnostics everything else uses.
 *
 * The diagnostic printer shows the offending line and points at the column.
 * That is not decoration: half of what this compiler refuses, it refuses on
 * purpose (a vacuous claim, an address in a global initialiser), and a
 * refusal a reader cannot locate reads as a broken compiler rather than as
 * the language working.
 */
#include "sav.h"
#include <stdarg.h>
#include <ctype.h>

const char *src_name = "<stdin>";
const char *src_text = "";

void *xmalloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) { fprintf(stderr, "savc: out of memory\n"); exit(2); }
    return p;
}

char *xstrndup(const char *s, size_t n) {
    char *d = xmalloc(n + 1);
    memcpy(d, s, n);
    return d;
}

/* Print the source line and a caret under the column. */
static void show_line(int line, int col) {
    const char *p = src_text;
    int l = 1;
    while (*p && l < line) { if (*p++ == '\n') l++; }
    const char *end = p;
    while (*end && *end != '\n') end++;
    fprintf(stderr, "  %5d | %.*s\n", line, (int)(end - p), p);
    fprintf(stderr, "        | ");
    for (int i = 1; i < col; i++) fputc(' ', stderr);
    fprintf(stderr, "^\n");
}

void die_at(const char *file, int line, int col, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%d:%d: ", file ? file : src_name, line, col);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
    if (line > 0) show_line(line, col);
    exit(1);
}

void note(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "        note: ");
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
}

static struct { const char *word; TokKind kind; } keywords[] = {
    {"fn", T_FN}, {"entry", T_ENTRY}, {"struct", T_STRUCT}, {"const", T_CONST},
    {"var", T_VAR}, {"let", T_LET}, {"if", T_IF}, {"else", T_ELSE},
    {"while", T_WHILE}, {"for", T_FOR}, {"in", T_IN}, {"return", T_RETURN},
    {"break", T_BREAK}, {"continue", T_CONTINUE}, {"needs", T_NEEDS},
    {"gives", T_GIVES}, {"assert", T_ASSERT}, {"as", T_AS}, {"dev", T_DEV},
    {"true", T_TRUE}, {"false", T_FALSE}, {"size", T_SIZE},
    {"use", T_USE},
};

const char *tok_name(TokKind k) {
    switch (k) {
    case T_EOF: return "end of file";
    case T_IDENT: return "a name";
    case T_INT: return "a number";
    case T_STR: return "a string";
    case T_CHAR: return "a character";
    case T_LPAREN: return "'('"; case T_RPAREN: return "')'";
    case T_LBRACE: return "'{'"; case T_RBRACE: return "'}'";
    case T_LBRACK: return "'['"; case T_RBRACK: return "']'";
    case T_COMMA: return "','"; case T_SEMI: return "';'";
    case T_COLON: return "':'"; case T_DOT: return "'.'";
    case T_DOTDOT: return "'..'"; case T_ARROW: return "'->'";
    case T_ASSIGN: return "'='";
    default: break;
    }
    for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++)
        if (keywords[i].kind == k) return keywords[i].word;
    return "an operator";
}

static int at(Lexer *lx, const char *s) { return strncmp(lx->p, s, strlen(s)) == 0; }

static void adv(Lexer *lx, int n) {
    for (int i = 0; i < n; i++) {
        if (*lx->p == '\n') { lx->line++; lx->col = 1; }
        else lx->col++;
        lx->p++;
    }
}

static void skip_space(Lexer *lx) {
    for (;;) {
        while (*lx->p == ' ' || *lx->p == '\t' || *lx->p == '\r' || *lx->p == '\n') adv(lx, 1);
        if (at(lx, "//")) { while (*lx->p && *lx->p != '\n') adv(lx, 1); continue; }
        if (at(lx, "/*")) {
            int line = lx->line, col = lx->col;
            adv(lx, 2);
            while (*lx->p && !at(lx, "*/")) adv(lx, 1);
            if (!*lx->p) die_at(src_name, line, col, "this comment is never closed");
            adv(lx, 2);
            continue;
        }
        return;
    }
}

static int esc_char(Lexer *lx) {
    /* on entry lx->p is at the backslash */
    adv(lx, 1);
    int c = *lx->p;
    adv(lx, 1);
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case '0': return '\0';
    case '\\': return '\\';
    case '"': return '"';
    case '\'': return '\'';
    default:
        die_at(src_name, lx->line, lx->col, "\\%c is not an escape sav knows", c);
        return 0;
    }
}

static Token scan(Lexer *lx) {
    skip_space(lx);
    Token t;
    memset(&t, 0, sizeof t);
    t.line = lx->line; t.col = lx->col;

    if (!*lx->p) { t.kind = T_EOF; return t; }

    if (isalpha((unsigned char)*lx->p) || *lx->p == '_') {
        const char *s = lx->p;
        while (isalnum((unsigned char)*lx->p) || *lx->p == '_') adv(lx, 1);
        t.text = xstrndup(s, (size_t)(lx->p - s));
        t.kind = T_IDENT;
        for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++)
            if (strcmp(t.text, keywords[i].word) == 0) { t.kind = keywords[i].kind; break; }
        return t;
    }

    if (isdigit((unsigned char)*lx->p)) {
        i64 v = 0;
        if (at(lx, "0x") || at(lx, "0X")) {
            adv(lx, 2);
            if (!isxdigit((unsigned char)*lx->p)) die_at(src_name, t.line, t.col, "0x with no digits after it");
            while (isxdigit((unsigned char)*lx->p) || *lx->p == '_') {
                if (*lx->p != '_') {
                    int c = tolower((unsigned char)*lx->p);
                    v = v * 16 + (c <= '9' ? c - '0' : c - 'a' + 10);
                }
                adv(lx, 1);
            }
        } else {
            while (isdigit((unsigned char)*lx->p) || *lx->p == '_') {
                if (*lx->p != '_') v = v * 10 + (*lx->p - '0');
                adv(lx, 1);
            }
        }
        t.kind = T_INT; t.ival = v;
        return t;
    }

    if (*lx->p == '"') {
        adv(lx, 1);
        size_t cap = 32, n = 0;
        char *buf = xmalloc(cap);
        while (*lx->p && *lx->p != '"') {
            int c = (*lx->p == '\\') ? esc_char(lx) : (adv(lx, 1), lx->p[-1]);
            if (n + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
            buf[n++] = (char)c;
        }
        if (*lx->p != '"') die_at(src_name, t.line, t.col, "this string is never closed");
        adv(lx, 1);
        buf[n] = 0;
        t.kind = T_STR; t.text = buf; t.len = n;
        return t;
    }

    if (*lx->p == '\'') {
        adv(lx, 1);
        int c = (*lx->p == '\\') ? esc_char(lx) : (adv(lx, 1), (unsigned char)lx->p[-1]);
        if (*lx->p != '\'') die_at(src_name, t.line, t.col, "a character literal holds exactly one character");
        adv(lx, 1);
        t.kind = T_INT; t.ival = c;
        return t;
    }

    struct { const char *s; TokKind k; } ops[] = {
        {"...", T_EOF}, /* never matched; keeps the three-char row honest */
        {"<<", T_SHL}, {">>", T_SHR}, {"<=", T_LE}, {">=", T_GE},
        {"==", T_EQEQ}, {"!=", T_NE}, {"&&", T_ANDAND}, {"||", T_OROR},
        {"->", T_ARROW}, {"..", T_DOTDOT}, {"+=", T_PLUSEQ}, {"-=", T_MINUSEQ},
        {"(", T_LPAREN}, {")", T_RPAREN}, {"{", T_LBRACE}, {"}", T_RBRACE},
        {"[", T_LBRACK}, {"]", T_RBRACK}, {",", T_COMMA}, {";", T_SEMI},
        {":", T_COLON}, {".", T_DOT}, {"+", T_PLUS}, {"-", T_MINUS},
        {"*", T_STAR}, {"/", T_SLASH}, {"%", T_PERCENT}, {"&", T_AMP},
        {"|", T_PIPE}, {"^", T_CARET}, {"~", T_TILDE}, {"!", T_BANG},
        {"@", T_AT},
        {"<", T_LT}, {">", T_GT}, {"=", T_ASSIGN},
    };
    for (size_t i = 1; i < sizeof ops / sizeof ops[0]; i++) {
        if (at(lx, ops[i].s)) {
            adv(lx, (int)strlen(ops[i].s));
            t.kind = ops[i].k;
            return t;
        }
    }
    die_at(src_name, t.line, t.col, "'%c' is not a character sav uses", *lx->p);
    return t;
}

void lex_init(Lexer *lx, const char *text) {
    memset(lx, 0, sizeof *lx);
    lx->p = text; lx->line = 1; lx->col = 1;
    lx->tok = scan(lx);
}

void lex_next(Lexer *lx) {
    if (lx->have_ahead) { lx->tok = lx->ahead; lx->have_ahead = 0; return; }
    lx->tok = scan(lx);
}

Token *lex_peek(Lexer *lx) {
    if (!lx->have_ahead) { lx->ahead = scan(lx); lx->have_ahead = 1; }
    return &lx->ahead;
}
