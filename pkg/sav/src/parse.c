/* The sav parser: source to a tree.
 *
 * Recursive descent, one token of lookahead, and no preprocessor — a sav file
 * means what it says without a second language deciding which half of it is
 * real. Structs must be declared before they are used; that is a rule and not
 * an accident, because a compiler that accepts a name it has not seen must
 * either guess its size or make two passes over everything.
 */
#include "sav.h"

typedef struct Loaded { char *path; struct Loaded *next; } Loaded;

typedef struct {
    Lexer lx;
    Unit *unit;
    const char *file;      /* the file being parsed right now */
    const char *text;
    Loaded *loaded;        /* every file already parsed, by path */
} P;

static void parse_path(P *p, const char *path, int line, int col);

static Type *parse_type(P *p);
static Expr *parse_expr(P *p);
static Stmt *parse_stmt(P *p);

/* The text of a clause, lifted straight out of the source between two token
 * positions. A contract that fails names itself in the message; quoting the
 * source is the only way for that name to stay true when the clause changes. */
static char *src_span(int l1, int c1, int l2, int c2) {
    const char *p = src_text;
    int line = 1, col = 1;
    const char *start = NULL, *end = NULL;
    for (;;) {
        if (line == l1 && col == c1) start = p;
        if (line == l2 && col == c2) { end = p; break; }
        if (!*p) { end = p; break; }
        if (*p == '\n') { line++; col = 1; } else col++;
        p++;
    }
    if (!start) return xstrndup("", 0);
    while (end > start && (end[-1] == ' ' || end[-1] == '\n' || end[-1] == '\t' || end[-1] == '\r')) end--;
    return xstrndup(start, (size_t)(end - start));
}

static void expect(P *p, TokKind k) {
    if (p->lx.tok.kind != k)
        die_at(src_name, p->lx.tok.line, p->lx.tok.col,
               "expected %s, found %s", tok_name(k), tok_name(p->lx.tok.kind));
    lex_next(&p->lx);
}

static int accept(P *p, TokKind k) {
    if (p->lx.tok.kind == k) { lex_next(&p->lx); return 1; }
    return 0;
}

static char *expect_ident(P *p) {
    if (p->lx.tok.kind != T_IDENT)
        die_at(src_name, p->lx.tok.line, p->lx.tok.col,
               "expected a name, found %s", tok_name(p->lx.tok.kind));
    char *s = p->lx.tok.text;
    lex_next(&p->lx);
    return s;
}

static Expr *new_expr(P *p, ExprKind k) {
    Expr *e = xmalloc(sizeof *e);
    e->kind = k; e->line = p->lx.tok.line; e->col = p->lx.tok.col;
    return e;
}

static Stmt *new_stmt(P *p, StmtKind k) {
    Stmt *s = xmalloc(sizeof *s);
    s->kind = k; s->line = p->lx.tok.line; s->col = p->lx.tok.col;
    return s;
}

/* ---- types ------------------------------------------------------------- */
static Type *find_struct(P *p, const char *name) {
    for (int i = 0; i < p->unit->nstructs; i++)
        if (strcmp(p->unit->structs[i]->name, name) == 0) return p->unit->structs[i];
    return NULL;
}

static Type *parse_type(P *p) {
    int line = p->lx.tok.line, col = p->lx.tok.col;
    int is_dev = accept(p, T_DEV);
    if (accept(p, T_STAR)) return ty_ptr(parse_type(p), is_dev);
    if (is_dev)
        die_at(src_name, line, col,
               "`dev` describes a pointer: write `dev *u32`, the address of a register");
    if (accept(p, T_LBRACK)) {
        if (accept(p, T_RBRACK)) return ty_slice(parse_type(p));
        if (p->lx.tok.kind != T_INT)
            die_at(src_name, p->lx.tok.line, p->lx.tok.col, "an array length must be a number");
        i64 n = p->lx.tok.ival;
        lex_next(&p->lx);
        expect(p, T_RBRACK);
        return ty_array(parse_type(p), n);
    }
    if (p->lx.tok.kind != T_IDENT)
        die_at(src_name, line, col, "expected a type, found %s", tok_name(p->lx.tok.kind));
    char *name = p->lx.tok.text;
    lex_next(&p->lx);
    static const struct { const char *n; int bits, sign; } ints[] = {
        {"i8",8,1},{"i16",16,1},{"i32",32,1},{"i64",64,1},
        {"u8",8,0},{"u16",16,0},{"u32",32,0},{"u64",64,0},
    };
    for (size_t i = 0; i < sizeof ints / sizeof ints[0]; i++)
        if (strcmp(name, ints[i].n) == 0) return ty_int(ints[i].bits, ints[i].sign);
    if (strcmp(name, "bool") == 0) return ty_bool();
    if (strcmp(name, "void") == 0) return ty_void();
    CapDef *c = cap_by_name(name);
    if (c) return ty_cap(c);
    Type *s = find_struct(p, name);
    if (s) return s;
    die_at(src_name, line, col, "there is no type called `%s`", name);
    return NULL;
}

/* ---- expressions ------------------------------------------------------- */
static Expr *parse_primary(P *p) {
    Token t = p->lx.tok;
    switch (t.kind) {
    case T_INT: {
        Expr *e = new_expr(p, E_INT); e->ival = t.ival;
        lex_next(&p->lx); return e;
    }
    case T_TRUE: case T_FALSE: {
        Expr *e = new_expr(p, E_BOOL); e->ival = (t.kind == T_TRUE);
        lex_next(&p->lx); return e;
    }
    case T_STR: {
        Expr *e = new_expr(p, E_STR); e->str = t.text; e->slen = t.len;
        lex_next(&p->lx); return e;
    }
    case T_SIZE: {
        Expr *e = new_expr(p, E_SIZE);
        lex_next(&p->lx);
        expect(p, T_LPAREN);
        e->type = parse_type(p);
        expect(p, T_RPAREN);
        return e;
    }
    case T_LPAREN: {
        lex_next(&p->lx);
        Expr *e = parse_expr(p);
        expect(p, T_RPAREN);
        return e;
    }
    case T_IDENT: {
        char *name = t.text;
        Expr *e = new_expr(p, E_VAR);
        lex_next(&p->lx);
        if (p->lx.tok.kind == T_LPAREN) {
            lex_next(&p->lx);
            e->kind = E_CALL; e->name = name;
            Expr **args = NULL; int n = 0;
            if (p->lx.tok.kind != T_RPAREN) {
                for (;;) {
                    args = realloc(args, sizeof(Expr *) * (size_t)(n + 1));
                    args[n++] = parse_expr(p);
                    if (!accept(p, T_COMMA)) break;
                }
            }
            expect(p, T_RPAREN);
            e->args = args; e->nargs = n;
            return e;
        }
        e->name = name;
        return e;
    }
    default:
        die_at(src_name, t.line, t.col, "expected a value, found %s", tok_name(t.kind));
        return NULL;
    }
}

static Expr *parse_postfix(P *p) {
    Expr *e = parse_primary(p);
    for (;;) {
        /* `out!write(...)`. A call that reaches the world is marked where it
         * happens, not only in the signature that permitted it: `grep -n '!'`
         * over a sav program lists every line that touches anything outside
         * the process. A capability's methods can be called no other way. */
        if (p->lx.tok.kind == T_BANG) {
            lex_next(&p->lx);
            char *name = expect_ident(p);
            expect(p, T_LPAREN);
            Expr *m = xmalloc(sizeof *m);
            m->kind = E_METHOD; m->line = e->line; m->col = e->col;
            m->a = e; m->name = name;
            Expr **args = NULL; int n = 0;
            if (p->lx.tok.kind != T_RPAREN) {
                for (;;) {
                    args = realloc(args, sizeof(Expr *) * (size_t)(n + 1));
                    args[n++] = parse_expr(p);
                    if (!accept(p, T_COMMA)) break;
                }
            }
            expect(p, T_RPAREN);
            m->args = args; m->nargs = n;
            e = m;
            continue;
        }
        /* `uart@`. A device access does not look like memory, because it is
         * not: it happens exactly once, at exactly the declared width, in the
         * order written. */
        if (p->lx.tok.kind == T_AT) {
            Expr *d = new_expr(p, E_DEREF);
            lex_next(&p->lx);
            d->a = e; d->op = T_AT;
            d->line = e->line; d->col = e->col;
            e = d;
            continue;
        }
        if (p->lx.tok.kind == T_DOT) {
            lex_next(&p->lx);
            int nline = p->lx.tok.line, ncol = p->lx.tok.col;
            char *name = expect_ident(p);
            /* `Reply.Ok(5)` builds a case of an enum. It is the one thing
             * that looks like a method call and is not one — and it is told
             * apart by the receiver being the name of a type. */
            if (e->kind == E_VAR && e->name) {
                Type *en = find_struct(p, e->name);
                if (en && en->kind == TY_ENUM) {
                    Expr *v = xmalloc(sizeof *v);
                    v->kind = E_VARIANT; v->line = e->line; v->col = e->col;
                    v->type = en; v->name = name;
                    if (accept(p, T_LPAREN)) {
                        Expr **args = NULL; int na = 0;
                        if (p->lx.tok.kind != T_RPAREN) {
                            for (;;) {
                                args = realloc(args, sizeof(Expr *) * (size_t)(na + 1));
                                args[na++] = parse_expr(p);
                                if (!accept(p, T_COMMA)) break;
                            }
                        }
                        expect(p, T_RPAREN);
                        v->args = args; v->nargs = na;
                    }
                    e = v;
                    continue;
                }
            }
            if (p->lx.tok.kind == T_LPAREN)
                die_at(src_name, nline, ncol,
                       "the only methods in sav are a capability's, and they are called with "
                       "`!`: write `%s!%s(...)`",
                       e->kind == E_VAR ? e->name : "x", name);
            Expr *f = xmalloc(sizeof *f);
            f->kind = E_FIELD; f->line = e->line; f->col = e->col;
            f->a = e; f->name = name;
            e = f;
            continue;
        }
        if (p->lx.tok.kind == T_LBRACK) {
            lex_next(&p->lx);
            Expr *i = parse_expr(p);
            if (accept(p, T_DOTDOT)) {          /* a slice of it */
                Expr *hi = (p->lx.tok.kind == T_RBRACK) ? NULL : parse_expr(p);
                expect(p, T_RBRACK);
                Expr *s = xmalloc(sizeof *s);
                s->kind = E_SLICE; s->line = e->line; s->col = e->col;
                s->a = e; s->b = i; s->c = hi;
                e = s;
                continue;
            }
            expect(p, T_RBRACK);
            Expr *ix = xmalloc(sizeof *ix);
            ix->kind = E_INDEX; ix->line = e->line; ix->col = e->col;
            ix->a = e; ix->b = i;
            e = ix;
            continue;
        }
        return e;
    }
}

static Expr *parse_unary(P *p) {
    Token t = p->lx.tok;
    if (t.kind == T_MINUS || t.kind == T_BANG || t.kind == T_TILDE) {
        Expr *e = new_expr(p, E_UNARY);
        e->op = t.kind;
        lex_next(&p->lx);
        e->a = parse_unary(p);
        return e;
    }
    if (t.kind == T_AMP) {
        Expr *e = new_expr(p, E_ADDR);
        lex_next(&p->lx);
        e->a = parse_unary(p);
        return e;
    }
    if (t.kind == T_STAR) {
        Expr *e = new_expr(p, E_DEREF);
        lex_next(&p->lx);
        e->a = parse_unary(p);
        return e;
    }
    Expr *e = parse_postfix(p);
    while (p->lx.tok.kind == T_AS) {
        Expr *c = xmalloc(sizeof *c);
        c->kind = E_CAST; c->line = e->line; c->col = e->col;
        lex_next(&p->lx);
        c->a = e; c->type = parse_type(p);
        e = c;
    }
    return e;
}

static int prec_of(TokKind k) {
    switch (k) {
    case T_STAR: case T_SLASH: case T_PERCENT: return 10;
    case T_PLUS: case T_MINUS: return 9;
    case T_SHL: case T_SHR: return 8;
    case T_LT: case T_LE: case T_GT: case T_GE: return 7;
    case T_EQEQ: case T_NE: return 6;
    case T_AMP: return 5;
    case T_CARET: return 4;
    case T_PIPE: return 3;
    case T_ANDAND: return 2;
    case T_OROR: return 1;
    default: return 0;
    }
}

static Expr *parse_binary(P *p, int min_prec) {
    Expr *lhs = parse_unary(p);
    for (;;) {
        int prec = prec_of(p->lx.tok.kind);
        if (prec == 0 || prec < min_prec) return lhs;
        int op = p->lx.tok.kind;
        int line = p->lx.tok.line, col = p->lx.tok.col;
        lex_next(&p->lx);
        Expr *rhs = parse_binary(p, prec + 1);
        Expr *e = xmalloc(sizeof *e);
        e->kind = E_BINARY; e->op = op; e->a = lhs; e->b = rhs;
        e->line = line; e->col = col;
        lhs = e;
    }
}

static Expr *parse_expr(P *p) {
    Expr *lhs = parse_binary(p, 1);
    if (p->lx.tok.kind == T_ASSIGN || p->lx.tok.kind == T_PLUSEQ || p->lx.tok.kind == T_MINUSEQ) {
        int op = p->lx.tok.kind;
        int line = p->lx.tok.line, col = p->lx.tok.col;
        lex_next(&p->lx);
        Expr *rhs = parse_expr(p);
        if (op != T_ASSIGN) {
            /* a += b is a = a + b, expanded here so the checker and the code
             * generator only ever see one shape. */
            Expr *sum = xmalloc(sizeof *sum);
            sum->kind = E_BINARY;
            sum->op = (op == T_PLUSEQ) ? T_PLUS : T_MINUS;
            sum->a = lhs; sum->b = rhs; sum->line = line; sum->col = col;
            rhs = sum;
        }
        Expr *e = xmalloc(sizeof *e);
        e->kind = E_ASSIGN; e->a = lhs; e->b = rhs; e->line = line; e->col = col;
        return e;
    }
    return lhs;
}

/* ---- statements -------------------------------------------------------- */
static Stmt *parse_block(P *p) {
    Stmt *s = new_stmt(p, S_BLOCK);
    expect(p, T_LBRACE);
    Stmt **list = NULL; int n = 0;
    while (p->lx.tok.kind != T_RBRACE) {
        if (p->lx.tok.kind == T_EOF)
            die_at(src_name, s->line, s->col, "this block is never closed");
        list = realloc(list, sizeof(Stmt *) * (size_t)(n + 1));
        list[n++] = parse_stmt(p);
    }
    expect(p, T_RBRACE);
    s->stmts = list; s->nstmts = n;
    return s;
}

static Stmt *parse_stmt(P *p) {
    switch (p->lx.tok.kind) {
    case T_LBRACE: return parse_block(p);
    case T_LET: case T_VAR: {
        Stmt *s = new_stmt(p, S_LET);
        s->is_var = (p->lx.tok.kind == T_VAR);
        lex_next(&p->lx);
        s->name = expect_ident(p);
        if (accept(p, T_COLON)) s->type = parse_type(p);
        if (accept(p, T_ASSIGN)) s->init = parse_expr(p);
        else if (!s->type)
            die_at(src_name, s->line, s->col,
                   "`%s %s` says neither a type nor a value, so nothing here knows what it is",
                   s->is_var ? "var" : "let", s->name);
        expect(p, T_SEMI);
        return s;
    }
    case T_IF: {
        Stmt *s = new_stmt(p, S_IF);
        lex_next(&p->lx);
        s->cond = parse_expr(p);
        s->then_s = parse_block(p);
        if (accept(p, T_ELSE))
            s->else_s = (p->lx.tok.kind == T_IF) ? parse_stmt(p) : parse_block(p);
        return s;
    }
    case T_WHILE: {
        Stmt *s = new_stmt(p, S_WHILE);
        lex_next(&p->lx);
        s->cond = parse_expr(p);
        s->body = parse_block(p);
        return s;
    }
    case T_FOR: {
        Stmt *s = new_stmt(p, S_FOR);
        lex_next(&p->lx);
        s->name = expect_ident(p);
        expect(p, T_IN);
        s->lo = parse_binary(p, 1);
        expect(p, T_DOTDOT);
        s->hi = parse_binary(p, 1);
        s->body = parse_block(p);
        return s;
    }
    case T_RETURN: {
        Stmt *s = new_stmt(p, S_RETURN);
        lex_next(&p->lx);
        if (p->lx.tok.kind != T_SEMI) s->expr = parse_expr(p);
        expect(p, T_SEMI);
        return s;
    }
    case T_BREAK: { Stmt *s = new_stmt(p, S_BREAK); lex_next(&p->lx); expect(p, T_SEMI); return s; }
    case T_CONTINUE: { Stmt *s = new_stmt(p, S_CONTINUE); lex_next(&p->lx); expect(p, T_SEMI); return s; }
    case T_MATCH: {
        Stmt *s = new_stmt(p, S_MATCH);
        lex_next(&p->lx);
        s->expr = parse_expr(p);
        expect(p, T_LBRACE);
        MatchArm *arms = NULL; int n = 0;
        while (p->lx.tok.kind != T_RBRACE) {
            MatchArm a;
            memset(&a, 0, sizeof a);
            a.line = p->lx.tok.line; a.col = p->lx.tok.col;
            if (accept(p, T_ELSE)) a.is_else = 1;
            else {
                a.name = expect_ident(p);
                if (accept(p, T_LPAREN)) {
                    while (p->lx.tok.kind != T_RPAREN) {
                        a.binds = realloc(a.binds, sizeof(char *) * (size_t)(a.nbinds + 1));
                        a.binds[a.nbinds++] = expect_ident(p);
                        if (!accept(p, T_COMMA)) break;
                    }
                    expect(p, T_RPAREN);
                }
            }
            expect(p, T_ARROW);
            a.body = parse_stmt(p);
            arms = realloc(arms, sizeof(MatchArm) * (size_t)(n + 1));
            arms[n++] = a;
        }
        expect(p, T_RBRACE);
        if (!n) die_at(src_name, s->line, s->col, "a match with no arms decides nothing");
        s->arms = arms; s->narms = n;
        return s;
    }
    case T_ASSERT: {
        Stmt *s = new_stmt(p, S_ASSERT);
        int l1 = p->lx.tok.line, c1;
        lex_next(&p->lx);
        c1 = p->lx.tok.col; l1 = p->lx.tok.line;
        s->cond = parse_expr(p);
        s->name = src_span(l1, c1, p->lx.tok.line, p->lx.tok.col);
        expect(p, T_SEMI);
        return s;
    }
    default: {
        Stmt *s = new_stmt(p, S_EXPR);
        s->expr = parse_expr(p);
        expect(p, T_SEMI);
        return s;
    }
    }
}

/* ---- declarations ------------------------------------------------------ */
static void parse_struct(P *p) {
    lex_next(&p->lx);
    Type *t = xmalloc(sizeof *t);
    t->kind = TY_STRUCT;
    t->name = expect_ident(p);
    expect(p, T_LBRACE);
    Field *fields = NULL; int n = 0, off = 0, align = 1;
    while (p->lx.tok.kind != T_RBRACE) {
        char *fname = expect_ident(p);
        expect(p, T_COLON);
        Type *ft = parse_type(p);
        if (ft->align > align) align = ft->align;
        off = (off + ft->align - 1) & ~(ft->align - 1);
        fields = realloc(fields, sizeof(Field) * (size_t)(n + 1));
        fields[n].name = fname; fields[n].type = ft; fields[n].offset = off;
        n++; off += ft->size;
        if (!accept(p, T_COMMA)) break;
    }
    expect(p, T_RBRACE);
    t->fields = fields; t->nfields = n;
    t->align = align;
    t->size = (off + align - 1) & ~(align - 1);
    p->unit->structs = realloc(p->unit->structs, sizeof(Type *) * (size_t)(p->unit->nstructs + 1));
    p->unit->structs[p->unit->nstructs++] = t;
}

/* An enum is a tag and, after it, whatever the chosen case carries. The tag
 * is eight bytes so that every payload after it is aligned without thought,
 * and the value is as large as its largest case — which is why a `match` can
 * read the payload straight out of the value rather than through a pointer. */
static void parse_enum(P *p) {
    lex_next(&p->lx);
    Type *t = xmalloc(sizeof *t);
    t->kind = TY_ENUM;
    t->name = expect_ident(p);
    expect(p, T_LBRACE);
    Variant *vs = NULL; int n = 0, biggest = 8;
    while (p->lx.tok.kind != T_RBRACE) {
        Variant v;
        memset(&v, 0, sizeof v);
        v.name = expect_ident(p);
        v.index = n;
        int off = 8;
        if (accept(p, T_LPAREN)) {
            while (p->lx.tok.kind != T_RPAREN) {
                char *fname = expect_ident(p);
                expect(p, T_COLON);
                Type *ft = parse_type(p);
                if (ft->kind == TY_ENUM || ft->kind == TY_STRUCT)
                    die_at(src_name, p->lx.tok.line, p->lx.tok.col,
                           "a case carries numbers, pointers and slices; for %s carry a pointer "
                           "to it, so that the size of one case cannot decide the size of every "
                           "value of this type", ty_str(ft));
                off = (off + ft->align - 1) & ~(ft->align - 1);
                v.fields = realloc(v.fields, sizeof(Field) * (size_t)(v.nfields + 1));
                v.fields[v.nfields].name = fname;
                v.fields[v.nfields].type = ft;
                v.fields[v.nfields].offset = off;
                v.nfields++;
                off += ft->size;
                if (!accept(p, T_COMMA)) break;
            }
            expect(p, T_RPAREN);
        }
        if (off > biggest) biggest = off;
        vs = realloc(vs, sizeof(Variant) * (size_t)(n + 1));
        vs[n++] = v;
        if (!accept(p, T_COMMA)) break;
    }
    expect(p, T_RBRACE);
    if (!n) die_at(src_name, p->lx.tok.line, p->lx.tok.col,
                   "an enum with no cases is a type with no values");
    t->variants = vs; t->nvariants = n;
    t->align = 8;
    t->size = (biggest + 7) & ~7;
    p->unit->structs = realloc(p->unit->structs, sizeof(Type *) * (size_t)(p->unit->nstructs + 1));
    p->unit->structs[p->unit->nstructs++] = t;
}

static void add_global(P *p, Sym *s) {
    s->next = p->unit->globals;
    p->unit->globals = s;
}

static void parse_fn(P *p, int is_entry) {
    Fn *f = xmalloc(sizeof *f);
    f->line = p->lx.tok.line; f->col = p->lx.tok.col;
    f->file = p->file; f->text = p->text;
    lex_next(&p->lx);                      /* fn */
    f->name = expect_ident(p);
    f->is_entry = is_entry;
    expect(p, T_LPAREN);
    Sym *tail = NULL;
    while (p->lx.tok.kind != T_RPAREN) {
        Sym *s = xmalloc(sizeof *s);
        s->kind = SYM_PARAM;
        s->name = expect_ident(p);
        expect(p, T_COLON);
        s->type = parse_type(p);
        if (tail) tail->next = s; else f->params = s;
        tail = s;
        f->nparams++;
        if (!accept(p, T_COMMA)) break;
    }
    expect(p, T_RPAREN);
    f->ret = ty_void();
    if (accept(p, T_ARROW)) {
        /* `-> i64` or `-> n: i64`. Naming the result is what makes a promise
         * about it possible to write: `gives n >= 1` talks about a name, the
         * way every other expression in the language does. */
        if (p->lx.tok.kind == T_IDENT && lex_peek(&p->lx)->kind == T_COLON) {
            f->ret_name = p->lx.tok.text;
            lex_next(&p->lx);
            lex_next(&p->lx);
        }
        f->ret = parse_type(p);
    }

    for (;;) {
        if (p->lx.tok.kind == T_NEEDS || p->lx.tok.kind == T_GIVES) {
            int is_req = (p->lx.tok.kind == T_NEEDS);
            lex_next(&p->lx);
            for (;;) {
                int l1 = p->lx.tok.line, c1 = p->lx.tok.col;
                Expr *e = parse_expr(p);
                char *text = src_span(l1, c1, p->lx.tok.line, p->lx.tok.col);
                Contract c = { e, l1, c1, text };
                if (is_req) {
                    f->req = realloc(f->req, sizeof(Contract) * (size_t)(f->nreq + 1));
                    f->req[f->nreq++] = c;
                } else {
                    f->ens = realloc(f->ens, sizeof(Contract) * (size_t)(f->nens + 1));
                    f->ens[f->nens++] = c;
                }
                if (!accept(p, T_COMMA)) break;
            }
            continue;
        }
        break;
    }

    f->body = parse_block(p);

    Fn **tp = &p->unit->fns;
    while (*tp) tp = &(*tp)->next;
    *tp = f;
    if (is_entry) {
        if (p->unit->entry)
            die_at(src_name, f->line, f->col,
                   "there is already an `entry fn` (%s); a program starts in one place",
                   p->unit->entry->name);
        p->unit->entry = f;
    }
}

/* The declarations in one file. Called once per file; `use` recurses. */
static void parse_decls(P *p) {
    while (p->lx.tok.kind != T_EOF) {
        switch (p->lx.tok.kind) {
        case T_STRUCT: parse_struct(p); break;
        case T_ENUM: parse_enum(p); break;
        case T_FN: parse_fn(p, 0); break;
        case T_ENTRY:
            lex_next(&p->lx);
            if (p->lx.tok.kind != T_FN)
                die_at(src_name, p->lx.tok.line, p->lx.tok.col, "`entry` belongs in front of `fn`");
            parse_fn(p, 1);
            break;
        case T_CONST: {
            Sym *s = xmalloc(sizeof *s);
            s->kind = SYM_CONST;
            s->line = p->lx.tok.line; s->col = p->lx.tok.col;
            lex_next(&p->lx);
            s->name = expect_ident(p);
            expect(p, T_COLON);
            s->type = parse_type(p);
            expect(p, T_ASSIGN);
            if (p->lx.tok.kind != T_INT && p->lx.tok.kind != T_TRUE && p->lx.tok.kind != T_FALSE)
                die_at(src_name, p->lx.tok.line, p->lx.tok.col,
                       "a const is a number known while compiling");
            s->cval = (p->lx.tok.kind == T_INT) ? p->lx.tok.ival : (p->lx.tok.kind == T_TRUE);
            lex_next(&p->lx);
            expect(p, T_SEMI);
            add_global(p, s);
            break;
        }
        case T_VAR: {
            Sym *s = xmalloc(sizeof *s);
            s->kind = SYM_GLOBAL;
            int line = p->lx.tok.line, col = p->lx.tok.col;
            s->line = line; s->col = col;
            lex_next(&p->lx);
            s->name = expect_ident(p);
            expect(p, T_COLON);
            s->type = parse_type(p);
            /* Rule 4: a global's initial value may not contain an address.
             * A table of absolute pointers in .data is what stopped resolving
             * when this project's kernel moved to the high half; sav has no
             * way to write one. Globals start as zero and are filled in by
             * code, where an address is formed PC-relative. */
            if (p->lx.tok.kind == T_ASSIGN)
                die_at(src_name, line, col,
                       "a global starts as zero in sav; assign to `%s` in code instead", s->name);
            expect(p, T_SEMI);
            add_global(p, s);
            break;
        }
        case T_USE: {
            int line = p->lx.tok.line, col = p->lx.tok.col;
            lex_next(&p->lx);
            if (p->lx.tok.kind != T_STR)
                die_at(src_name, line, col, "`use` names a file: `use \"num.sav\";`");
            char *what = p->lx.tok.text;
            lex_next(&p->lx);
            expect(p, T_SEMI);
            parse_path(p, what, line, col);
            break;
        }
        default:
            die_at(src_name, p->lx.tok.line, p->lx.tok.col,
                   "expected a declaration (fn, entry fn, struct, const, var, use), found %s",
                   tok_name(p->lx.tok.kind));
        }
    }
}

static char *read_whole_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = xmalloc((size_t)n + 1);
    if (n && fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); return NULL; }
    buf[n] = 0;
    fclose(f);
    return buf;
}

/* `use "num.sav"` names a file beside the one that says it, not beside the
 * directory savc happens to have been run from. A program that compiled from
 * one directory and not from another would be a build that depends on where
 * you stood. */
static char *resolve_beside(const char *including, const char *what) {
    if (what[0] == '/') return xstrndup(what, strlen(what));
    const char *slash = strrchr(including, '/');
    if (!slash) return xstrndup(what, strlen(what));
    size_t dir = (size_t)(slash - including) + 1;
    char *out = xmalloc(dir + strlen(what) + 1);
    memcpy(out, including, dir);
    strcpy(out + dir, what);
    return out;
}

static void parse_path(P *p, const char *what, int line, int col) {
    char *path = resolve_beside(p->file ? p->file : "", what);

    /* Once per file, however many times it is named: a diamond of `use` is an
     * ordinary shape and not an error, and parsing a file twice would make
     * every function in it a duplicate. */
    for (Loaded *l = p->loaded; l; l = l->next)
        if (strcmp(l->path, path) == 0) return;

    char *text = read_whole_file(path);
    if (!text) die_at(src_name, line, col, "there is no file `%s`", path);

    Loaded *l = xmalloc(sizeof *l);
    l->path = path; l->next = p->loaded;
    p->loaded = l;

    /* Everything that says which file we are in, saved and restored around the
     * one we are about to read. */
    Lexer saved_lx = p->lx;
    const char *saved_file = p->file, *saved_text = p->text;
    const char *saved_name = src_name, *saved_src = src_text;

    p->file = path; p->text = text;
    src_name = path; src_text = text;
    lex_init(&p->lx, text);
    parse_decls(p);

    p->lx = saved_lx;
    p->file = saved_file; p->text = saved_text;
    src_name = saved_name; src_text = saved_src;
}

void parse_into(Unit *u, const char *path) {
    P p;
    memset(&p, 0, sizeof p);
    p.unit = u;
    char *text = read_whole_file(path);
    if (!text) { fprintf(stderr, "savc: cannot read %s\n", path); exit(2); }
    Loaded *l = xmalloc(sizeof *l);
    l->path = xstrndup(path, strlen(path)); l->next = NULL;
    p.loaded = l;
    p.file = l->path; p.text = text;
    src_name = l->path; src_text = text;
    lex_init(&p.lx, text);
    parse_decls(&p);
}

Unit *parse_unit(const char *path) {
    Unit *u = xmalloc(sizeof(Unit));
    parse_into(u, path);
    return u;
}
