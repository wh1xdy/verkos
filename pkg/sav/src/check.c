/* Type checking, and the three rules that make sav sav.
 *
 * Rule 1 lives in check_contract(): a `requires` clause must depend on a
 * parameter, an `ensures` clause on `result` or a parameter, and neither may
 * be an expression the compiler can decide by itself. The whole of this
 * project's defect log is one shape — a report that does not depend on what
 * it reports — and this is where that shape stops being possible to write.
 *
 * Rule 2 lives in check_expr's E_METHOD case: a system call can only be
 * reached through a capability value, capability values are made only by the
 * entry code, and a capability cannot be stored in a global. Authority is
 * therefore always visible as an argument on the way down.
 *
 * Rule 4 lives in the cast and deref cases: `dev` cannot be cast away, so a
 * device pointer cannot be laundered into an ordinary one and then merged
 * with its neighbour by some later pass.
 */
#include "sav.h"

/* what the program will actually ask of the kernel: filled in as methods are
 * checked, and written into the executable as the manifest */
static u32 sys_used[64]; static int n_sys_used;
static const char *caps_used[16]; static int n_caps_used;

void record_syscall(u32 n) {
    if (!n) return;
    for (int i = 0; i < n_sys_used; i++) if (sys_used[i] == n) return;
    if (n_sys_used < 64) sys_used[n_sys_used++] = n;
}
static void record_cap(const char *name) {
    for (int i = 0; i < n_caps_used; i++) if (strcmp(caps_used[i], name) == 0) return;
    if (n_caps_used < 16) caps_used[n_caps_used++] = name;
}
u32 *syscalls_used(int *n) { *n = n_sys_used; return sys_used; }
const char **caps_used_list(int *n) { *n = n_caps_used; return caps_used; }

/* ---- scopes ------------------------------------------------------------
 * A scope holds pointers to symbols rather than the symbols themselves: a
 * parameter belongs to its function's list and is also visible in its body,
 * and one object cannot be threaded onto two lists by the same field. */
typedef struct SNode { Sym *sym; struct SNode *next; } SNode;
typedef struct Scope { SNode *syms; struct Scope *parent; } Scope;

typedef struct {
    Unit *unit;
    Fn *fn;
    Scope *scope;
    int frame;          /* bytes of frame used so far */
    int in_ensures;     /* `result` is only a name inside an ensures clause */
    int loop_depth;
} C;

static Sym *lookup(C *c, const char *name) {
    for (Scope *s = c->scope; s; s = s->parent)
        for (SNode *y = s->syms; y; y = y->next)
            if (strcmp(y->sym->name, name) == 0) return y->sym;
    for (Sym *y = c->unit->globals; y; y = y->next)
        if (strcmp(y->name, name) == 0) return y;
    return NULL;
}

static void push_scope(C *c) {
    Scope *s = xmalloc(sizeof *s);
    s->parent = c->scope;
    c->scope = s;
}
static void pop_scope(C *c) { c->scope = c->scope->parent; }

static void scope_add(C *c, Sym *s) {
    SNode *n = xmalloc(sizeof *n);
    n->sym = s; n->next = c->scope->syms;
    c->scope->syms = n;
}

static Sym *declare(C *c, const char *name, Type *t, SymKind kind, int line, int col) {
    for (SNode *y = c->scope->syms; y; y = y->next)
        if (strcmp(y->sym->name, name) == 0)
            die_at(src_name, line, col, "`%s` is already declared in this block", name);
    /* The returned value's name is spoken for. A local of the same name would
     * make `gives %s ...` read as a promise about the local, which is a claim
     * about something the caller never sees. */
    if (c->fn && c->fn->ret_name && strcmp(name, c->fn->ret_name) == 0)
        die_at(src_name, line, col,
               "`%s` is the name `%s` gives to the value it returns; call this one something "
               "else, so that a `gives` clause can only be read one way", name, c->fn->name);
    Sym *s = xmalloc(sizeof *s);
    s->kind = kind; s->name = xstrndup(name, strlen(name)); s->type = t;
    if (kind == SYM_LOCAL || kind == SYM_PARAM) {
        int sz = t->size < 8 ? 8 : t->size;
        c->frame += (sz + 7) & ~7;
        s->offset = c->frame;            /* [x29, -offset] */
    }
    scope_add(c, s);
    return s;
}

static Fn *find_fn(Unit *u, const char *name) {
    for (Fn *f = u->fns; f; f = f->next) if (strcmp(f->name, name) == 0) return f;
    return NULL;
}

static Expr *check_expr(C *c, Expr *e);
static void check_stmt(C *c, Stmt *s);
static int const_value(Expr *e, i64 *out);

/* An integer literal takes the type of what it is used with; everything else
 * needs a cast. There are no silent widenings in sav, because the ones in C
 * are how a length becomes negative. */
static int is_literal(Expr *e) { return e->kind == E_INT; }

static void want(Expr *e, Type *t, const char *what) {
    if (is_literal(e) && ty_is_int(t)) { e->type = t; return; }
    if (is_literal(e) && t->kind == TY_PTR) { e->type = t; return; }
    if (!ty_same(e->type, t))
        die_at(src_name, e->line, e->col, "%s wants %s, this is %s", what, ty_str(t), ty_str(e->type));
}

static void need_lvalue(Expr *e, const char *what) {
    if (!e->is_lvalue)
        die_at(src_name, e->line, e->col, "%s needs somewhere to put it, and this is a value", what);
}

static Expr *check_expr(C *c, Expr *e) {
    switch (e->kind) {
    case E_INT: e->type = ty_int(64, 1); break;
    case E_BOOL: e->type = ty_bool(); break;
    case E_STR: e->type = ty_slice(ty_int(8, 0)); break;
    case E_SIZE: {
        Type *t = e->type;
        e->kind = E_INT; e->ival = t->size; e->type = ty_int(64, 1);
        break;
    }
    case E_VAR: {
        Sym *s = lookup(c, e->name);
        if (!s) {
            if (find_fn(c->unit, e->name))
                die_at(src_name, e->line, e->col, "`%s` is a function; call it with ()", e->name);
            die_at(src_name, e->line, e->col, "nothing here is called `%s`", e->name);
        }
        e->sym = s; e->type = s->type;
        if (s->kind == SYM_CONST) { e->kind = E_INT; e->ival = s->cval; }
        else e->is_lvalue = 1;
        break;
    }
    case E_CALL: {
        Fn *f = find_fn(c->unit, e->name);
        if (!f) die_at(src_name, e->line, e->col, "there is no function called `%s`", e->name);
        if (f->is_entry)
            die_at(src_name, e->line, e->col,
                   "`%s` is where the program starts; the kernel calls it, not you", e->name);
        if (e->nargs != f->nparams)
            die_at(src_name, e->line, e->col, "`%s` takes %d argument%s, given %d",
                   f->name, f->nparams, f->nparams == 1 ? "" : "s", e->nargs);
        Sym *p = f->params;
        for (int i = 0; i < e->nargs; i++, p = p->next) {
            e->args[i] = check_expr(c, e->args[i]);
            want(e->args[i], p->type, "this argument");
        }
        e->fn = f; e->type = f->ret;
        break;
    }
    case E_METHOD: {
        e->a = check_expr(c, e->a);
        if (e->a->type->kind != TY_CAP)
            die_at(src_name, e->line, e->col,
                   "`!` calls a capability's method, and this is %s. A system call is reached "
                   "through a capability or not at all", ty_str(e->a->type));
        CapDef *d = &cap_defs[e->a->type->cap_id];
        CapMethod *m = NULL;
        for (int i = 0; i < d->nmethods; i++)
            if (strcmp(d->methods[i].name, e->name) == 0) { m = &d->methods[i]; break; }
        if (!m) {
            fprintf(stderr, "%s:%d:%d: %s has no method `%s`. It has:",
                    src_name, e->line, e->col, d->name, e->name);
            for (int i = 0; i < d->nmethods; i++) fprintf(stderr, " %s", d->methods[i].name);
            fprintf(stderr, "\n");
            exit(1);
        }
        if (e->nargs != m->nargs)
            die_at(src_name, e->line, e->col, "%s.%s takes %d argument%s, given %d",
                   d->name, m->name, m->nargs, m->nargs == 1 ? "" : "s", e->nargs);
        for (int i = 0; i < e->nargs; i++) {
            e->args[i] = check_expr(c, e->args[i]);
            Type *wt = (m->sig[i] == 's') ? ty_slice(ty_int(8, 0))
                     : (m->sig[i] == 'p') ? ty_ptr(ty_int(8, 0), 0)
                     : ty_int(64, 1);
            want(e->args[i], wt, "this argument");
        }
        e->method = m;
        e->type = (m->ret == 'i') ? ty_int(64, 1)
                : (m->ret == 's') ? ty_slice(ty_int(8, 0)) : ty_void();
        record_syscall((u32)m->sysno);
        record_cap(d->name);
        break;
    }
    case E_UNARY: {
        e->a = check_expr(c, e->a);
        if (e->op == T_BANG) { want(e->a, ty_bool(), "`!`"); e->type = ty_bool(); }
        else {
            if (!ty_is_int(e->a->type))
                die_at(src_name, e->line, e->col, "this operator wants a number, given %s", ty_str(e->a->type));
            e->type = e->a->type;
        }
        break;
    }
    case E_BINARY: {
        e->a = check_expr(c, e->a);
        e->b = check_expr(c, e->b);
        int op = e->op;
        if (op == T_ANDAND || op == T_OROR) {
            want(e->a, ty_bool(), "`&&`/`||`"); want(e->b, ty_bool(), "`&&`/`||`");
            e->type = ty_bool();
            break;
        }
        /* let a literal take the other side's type */
        if (is_literal(e->a) && !is_literal(e->b) && ty_is_int(e->b->type)) e->a->type = e->b->type;
        if (is_literal(e->b) && !is_literal(e->a) && ty_is_int(e->a->type)) e->b->type = e->a->type;
        if (op == T_EQEQ || op == T_NE || op == T_LT || op == T_LE || op == T_GT || op == T_GE) {
            if (!ty_same(e->a->type, e->b->type))
                die_at(src_name, e->line, e->col, "comparing %s with %s; say which one you meant with `as`",
                       ty_str(e->a->type), ty_str(e->b->type));
            e->type = ty_bool();
            break;
        }
        if (e->a->type->kind == TY_PTR && ty_is_int(e->b->type) && (op == T_PLUS || op == T_MINUS)) {
            e->type = e->a->type;      /* pointer arithmetic, in elements */
            break;
        }
        if (!ty_is_int(e->a->type) || !ty_is_int(e->b->type))
            die_at(src_name, e->line, e->col, "this operator wants numbers, given %s and %s",
                   ty_str(e->a->type), ty_str(e->b->type));
        if (!ty_same(e->a->type, e->b->type))
            die_at(src_name, e->line, e->col, "%s and %s do not mix; cast one with `as`",
                   ty_str(e->a->type), ty_str(e->b->type));
        e->type = e->a->type;
        break;
    }
    case E_INDEX: {
        e->a = check_expr(c, e->a);
        e->b = check_expr(c, e->b);
        if (!ty_is_int(e->b->type))
            die_at(src_name, e->line, e->col, "an index is a number, this is %s", ty_str(e->b->type));
        if (e->a->type->kind == TY_PTR && e->a->type->is_dev)
            die_at(src_name, e->line, e->col,
                   "a device pointer is not indexed: write `(p + i)@`, so that the access is "
                   "visible where it happens");
        /* A bare pointer has no length, so an index into one cannot be
         * checked. sav has slices for exactly this: `p[0..n]` is where the
         * length is said, once and visibly, and everything after it is
         * checked. */
        if (e->a->type->kind == TY_PTR)
            die_at(src_name, e->line, e->col,
                   "a pointer does not know how many there are, so this index cannot be checked. "
                   "Make a slice first: `p[0..n]`");
        if (e->a->type->kind == TY_ARRAY || e->a->type->kind == TY_SLICE) e->type = e->a->type->elem;
        else die_at(src_name, e->line, e->col, "%s cannot be indexed", ty_str(e->a->type));
        /* An index the compiler can work out, into a length it already knows,
         * is decided here rather than left to fail when it runs. */
        i64 k;
        if (e->a->type->kind == TY_ARRAY && const_value(e->b, &k) &&
            (k < 0 || k >= e->a->type->nelem))
            die_at(src_name, e->line, e->col,
                   "%lld is outside a %s, and that can be seen from here rather than found "
                   "when it runs", (long long)k, ty_str(e->a->type));
        e->is_lvalue = 1;
        break;
    }
    case E_FIELD: {
        e->a = check_expr(c, e->a);
        Type *t = e->a->type;
        if (t->kind == TY_CAP)
            die_at(src_name, e->line, e->col,
                   "a capability's methods are called with `!`: write `%s!%s(...)`. The mark is "
                   "there so that every line which reaches the world can be seen at a glance",
                   e->a->kind == E_VAR ? e->a->name : "cap", e->name);
        if (t->kind == TY_PTR && t->is_dev)
            die_at(src_name, e->line, e->col,
                   "a device register is reached with `@` and nothing else; `%s` would be an "
                   "access that does not look like one", e->name);
        if ((t->kind == TY_SLICE || t->kind == TY_ARRAY) && strcmp(e->name, "len") == 0) {
            e->kind = E_LEN; e->type = ty_int(64, 1);
            break;
        }
        if (t->kind == TY_SLICE && strcmp(e->name, "ptr") == 0) {
            e->type = ty_ptr(t->elem, 0);
            break;
        }
        if (t->kind == TY_PTR) t = t->elem;      /* one level of auto-deref */
        if (t->kind != TY_STRUCT)
            die_at(src_name, e->line, e->col, "%s has no fields", ty_str(e->a->type));
        for (int i = 0; i < t->nfields; i++)
            if (strcmp(t->fields[i].name, e->name) == 0) {
                e->field = &t->fields[i]; e->type = t->fields[i].type;
                e->is_lvalue = 1;
                break;
            }
        if (!e->field)
            die_at(src_name, e->line, e->col, "%s has no field `%s`", t->name, e->name);
        break;
    }
    case E_SLICE: {
        e->a = check_expr(c, e->a);
        e->b = check_expr(c, e->b);
        if (e->c) e->c = check_expr(c, e->c);
        Type *t = e->a->type;
        if (t->kind == TY_PTR && t->is_dev)
            die_at(src_name, e->line, e->col,
                   "a device pointer cannot become a slice: every access to a register is `@`, "
                   "one at a time, where it is written");
        if (t->kind != TY_ARRAY && t->kind != TY_SLICE && t->kind != TY_PTR)
            die_at(src_name, e->line, e->col,
                   "only an array, a slice or a pointer can be sliced, this is %s", ty_str(t));
        /* `a[2..]` runs to a length the value carries. A pointer carries none,
         * so the end has to be written down — this is the one place in sav
         * where a length is asserted rather than checked, and it should be
         * impossible to do by accident. */
        if (t->kind == TY_PTR && !e->c)
            die_at(src_name, e->line, e->col,
                   "a pointer has no end to run to: say how many, as `p[0..n]`");
        e->type = ty_slice(t->elem);
        break;
    }
    case E_ADDR: {
        e->a = check_expr(c, e->a);
        need_lvalue(e->a, "`&`");
        e->type = ty_ptr(e->a->type, 0);
        break;
    }
    case E_DEREF: {
        e->a = check_expr(c, e->a);
        if (e->a->type->kind != TY_PTR)
            die_at(src_name, e->line, e->col, "only a pointer can be followed, this is %s", ty_str(e->a->type));
        int dev = e->a->type->is_dev;
        if (e->op == T_AT && !dev)
            die_at(src_name, e->line, e->col,
                   "`@` is how device memory is reached, and this is %s. Ordinary memory is `*p`",
                   ty_str(e->a->type));
        if (e->op != T_AT && dev)
            die_at(src_name, e->line, e->col,
                   "a device access is written `p@`, not `*p`. It is not a dereference: it is one "
                   "access, of the declared width, in the order written — and it should not look "
                   "like memory");
        e->type = e->a->type->elem;
        e->is_lvalue = 1;
        break;
    }
    case E_CAST: {
        Type *to = e->type;
        e->a = check_expr(c, e->a);
        Type *from = e->a->type;
        /* Rule 4: `dev` is not a coat that can be taken off. A device pointer
         * that became an ordinary one would be free to be merged, widened or
         * hoisted, which is exactly the bug this rule exists to prevent. */
        if (from->kind == TY_PTR && from->is_dev && !(to->kind == TY_PTR && to->is_dev))
            die_at(src_name, e->line, e->col,
                   "a `dev` pointer cannot become %s. Device memory is read and written "
                   "exactly as written down; an ordinary pointer promises nothing of the kind", ty_str(to));
        if (from->kind == TY_CAP || to->kind == TY_CAP)
            die_at(src_name, e->line, e->col,
                   "a capability cannot be cast. It is authority, not a number");
        int ok = (ty_is_int(from) && ty_is_int(to))
              || (ty_is_int(from) && to->kind == TY_PTR)
              || (from->kind == TY_PTR && ty_is_int(to))
              || (from->kind == TY_PTR && to->kind == TY_PTR)
              || (from->kind == TY_BOOL && ty_is_int(to))
              || (ty_is_int(from) && to->kind == TY_BOOL);
        if (!ok)
            die_at(src_name, e->line, e->col, "%s cannot be cast to %s", ty_str(from), ty_str(to));
        break;
    }
    case E_ASSIGN: {
        e->a = check_expr(c, e->a);
        e->b = check_expr(c, e->b);
        need_lvalue(e->a, "an assignment");
        if (e->a->sym && e->a->sym->kind == SYM_LOCAL && !e->a->sym->offset) {}
        want(e->b, e->a->type, "this assignment");
        e->type = e->a->type;
        break;
    }
    case E_LEN: e->type = ty_int(64, 1); break;
    }
    return e;
}

/* ---- rule 1: a claim must depend on what it claims ----------------------
 * `depends` answers whether an expression can differ between two runs of the
 * function. A parameter can, `result` can, a call can. A literal cannot, and
 * neither can a const, which the checker has already folded into one. */
static int depends(Expr *e, int allow_result) {
    if (!e) return 0;
    switch (e->kind) {
    case E_INT: case E_BOOL: case E_STR: case E_SIZE: return 0;
    case E_VAR:
        if (!e->sym) return 0;
        if (e->sym->kind == SYM_RESULT) return allow_result;
        return e->sym->kind == SYM_PARAM || e->sym->kind == SYM_LOCAL || e->sym->kind == SYM_GLOBAL;
    case E_CALL: case E_METHOD: return 1;
    case E_LEN: case E_DEREF: case E_ADDR: case E_FIELD: case E_CAST: case E_UNARY:
        return depends(e->a, allow_result);
    case E_BINARY: case E_INDEX: case E_ASSIGN: case E_SLICE:
        return depends(e->a, allow_result) || depends(e->b, allow_result) || depends(e->c, allow_result);
    default: return 1;
    }
}

/* Whether the compiler can settle the expression itself. `1 == 1` mentions no
 * parameter and no result, but so does `true`; both are refused, and the
 * message says which kind of nothing it is. */
static int const_value(Expr *e, i64 *out) {
    i64 a, b;
    switch (e->kind) {
    case E_INT: case E_BOOL: *out = e->ival; return 1;
    case E_UNARY:
        if (!const_value(e->a, &a)) return 0;
        *out = (e->op == T_MINUS) ? -a : (e->op == T_BANG) ? !a : ~a;
        return 1;
    case E_BINARY:
        if (!const_value(e->a, &a) || !const_value(e->b, &b)) return 0;
        switch (e->op) {
        case T_PLUS: *out = a + b; return 1;
        case T_MINUS: *out = a - b; return 1;
        case T_STAR: *out = a * b; return 1;
        case T_SLASH: if (!b) return 0; *out = a / b; return 1;
        case T_PERCENT: if (!b) return 0; *out = a % b; return 1;
        case T_LT: *out = a < b; return 1;
        case T_LE: *out = a <= b; return 1;
        case T_GT: *out = a > b; return 1;
        case T_GE: *out = a >= b; return 1;
        case T_EQEQ: *out = a == b; return 1;
        case T_NE: *out = a != b; return 1;
        case T_ANDAND: *out = a && b; return 1;
        case T_OROR: *out = a || b; return 1;
        default: return 0;
        }
    default: return 0;
    }
}

static void check_contract(C *c, Contract *k, int is_ensures) {
    c->in_ensures = is_ensures;
    k->expr = check_expr(c, k->expr);
    c->in_ensures = 0;
    if (k->expr->type->kind != TY_BOOL)
        die_at(src_name, k->line, k->col,
               "a %s clause is true or false about the run; this is %s",
               is_ensures ? "`gives`" : "`needs`", ty_str(k->expr->type));

    i64 v;
    if (const_value(k->expr, &v)) {
        die_at(src_name, k->line, k->col,
               "this claim is decided before the program runs: it is %s no matter what happens",
               v ? "true" : "false");
    }
    if (!depends(k->expr, is_ensures)) {
        die_at(src_name, k->line, k->col,
               "this claim does not depend on what it claims");
    }
    if (is_ensures) {
        /* An `ensures` that never names `result` describes the arguments, which
         * the caller already knows. It passes whether or not the function did
         * its work — the exact shape this project keeps finding in its own
         * reports. */
        int names_result = 0;
        Expr *stack[64]; int sp = 0; stack[sp++] = k->expr;
        while (sp) {
            Expr *e = stack[--sp];
            if (!e) continue;
            if (e->kind == E_VAR && e->sym && e->sym->kind == SYM_RESULT) { names_result = 1; break; }
            if (sp < 60) {
                stack[sp++] = e->a; stack[sp++] = e->b; stack[sp++] = e->c;
                for (int i = 0; i < e->nargs && sp < 60; i++) stack[sp++] = e->args[i];
            }
        }
        if (!names_result && c->fn->ret->kind != TY_VOID)
            die_at(src_name, k->line, k->col,
                   "this promise never mentions `%s`, the value `%s` returns, so it says the "
                   "same thing whether the function worked or not",
                   c->fn->ret_name ? c->fn->ret_name : "the result", c->fn->name);
    }
}

/* ---- statements -------------------------------------------------------- */
static int always_returns(Stmt *s) {
    if (!s) return 0;
    switch (s->kind) {
    case S_RETURN: return 1;
    case S_BLOCK: return s->nstmts && always_returns(s->stmts[s->nstmts - 1]);
    case S_IF: return s->else_s && always_returns(s->then_s) && always_returns(s->else_s);
    default: return 0;
    }
}

static void check_stmt(C *c, Stmt *s) {
    switch (s->kind) {
    case S_BLOCK:
        push_scope(c);
        for (int i = 0; i < s->nstmts; i++) check_stmt(c, s->stmts[i]);
        pop_scope(c);
        break;
    case S_LET: {
        if (s->init) {
            s->init = check_expr(c, s->init);
            if (!s->type) {
                if (s->init->type->kind == TY_VOID)
                    die_at(src_name, s->line, s->col, "`%s` is being given nothing at all", s->name);
                s->type = s->init->type;
                if (is_literal(s->init)) s->type = ty_int(64, 1);
            } else {
                want(s->init, s->type, "this initialiser");
            }
        }
        if (s->type->kind == TY_CAP && s->is_var)
            die_at(src_name, s->line, s->col,
                   "a capability is not a variable: it is authority handed in, not something to reassign");
        s->sym = declare(c, s->name, s->type, SYM_LOCAL, s->line, s->col);
        break;
    }
    case S_EXPR: s->expr = check_expr(c, s->expr); break;
    case S_IF:
        s->cond = check_expr(c, s->cond);
        want(s->cond, ty_bool(), "an `if`");
        check_stmt(c, s->then_s);
        if (s->else_s) check_stmt(c, s->else_s);
        break;
    case S_WHILE:
        s->cond = check_expr(c, s->cond);
        want(s->cond, ty_bool(), "a `while`");
        c->loop_depth++;
        check_stmt(c, s->body);
        c->loop_depth--;
        break;
    case S_FOR:
        s->lo = check_expr(c, s->lo);
        s->hi = check_expr(c, s->hi);
        if (!ty_is_int(s->lo->type) || !ty_is_int(s->hi->type))
            die_at(src_name, s->line, s->col, "a `for` counts over numbers");
        s->lo->type = ty_int(64, 1); s->hi->type = ty_int(64, 1);
        push_scope(c);
        s->sym = declare(c, s->name, ty_int(64, 1), SYM_LOCAL, s->line, s->col);
        c->loop_depth++;
        check_stmt(c, s->body);
        c->loop_depth--;
        pop_scope(c);
        break;
    case S_RETURN:
        if (s->expr) {
            s->expr = check_expr(c, s->expr);
            if (c->fn->ret->kind == TY_VOID)
                die_at(src_name, s->line, s->col, "`%s` returns nothing, so it cannot return a value", c->fn->name);
            want(s->expr, c->fn->ret, "this return");
        } else if (c->fn->ret->kind != TY_VOID) {
            die_at(src_name, s->line, s->col, "`%s` returns %s, so this `return` needs a value",
                   c->fn->name, ty_str(c->fn->ret));
        }
        break;
    case S_BREAK: case S_CONTINUE:
        if (!c->loop_depth)
            die_at(src_name, s->line, s->col, "`%s` is only meaningful inside a loop",
                   s->kind == S_BREAK ? "break" : "continue");
        break;
    case S_ASSERT: {
        s->cond = check_expr(c, s->cond);
        want(s->cond, ty_bool(), "an `assert`");
        i64 v;
        if (const_value(s->cond, &v))
            die_at(src_name, s->line, s->col,
                   "this check is decided before the program runs: it is %s every time, "
                   "so it checks nothing", v ? "true" : "false");
        if (!depends(s->cond, 0))
            die_at(src_name, s->line, s->col,
                   "this check does not depend on anything the program did");
        record_syscall(64); record_syscall(93);   /* a failed check says so, and stops */
        break;
    }
    }
}

/* ---- functions --------------------------------------------------------- */
static void check_fn(C *c, Fn *f) {
    /* Diagnostics from here on name the file this function is in, which is not
     * the file savc was invoked on as soon as one file `use`s another. */
    if (f->file) { src_name = f->file; src_text = f->text; }
    c->fn = f;
    c->frame = 0;
    push_scope(c);

    int regs = 0;
    for (Sym *p = f->params; p; p = p->next) {
        if (p->type->kind == TY_VOID)
            die_at(src_name, f->line, f->col, "a parameter cannot be void");
        if (p->type->kind == TY_STRUCT || p->type->kind == TY_ARRAY)
            die_at(src_name, f->line, f->col,
                   "`%s` is %s: pass `*%s` instead. sav copies a struct only where you write "
                   "the copy, so that a large argument is visible as one", p->name,
                   ty_str(p->type), ty_str(p->type));
        if (f->is_entry && p->type->kind != TY_CAP)
            die_at(src_name, f->line, f->col,
                   "`entry fn %s` may only take capabilities, and `%s` is %s. What a program "
                   "can do is what its entry point asks for", f->name, p->name, ty_str(p->type));
        p->reg_index = regs;
        regs += (p->type->kind == TY_SLICE) ? 2 : 1;
        if (regs > 8)
            die_at(src_name, f->line, f->col,
                   "`%s` has more arguments than the eight registers hold", f->name);
        int sz = p->type->size < 8 ? 8 : p->type->size;
        c->frame += (sz + 7) & ~7;
        p->offset = c->frame;            /* [x29, -offset], written by the prologue */
        scope_add(c, p);
    }

    /* The returned value gets its slot before the clauses are checked, because
     * a `gives` clause reads it by name and the name must already mean
     * somewhere. */
    if (f->nens) {
        if (f->ret->kind != TY_VOID && !f->ret_name)
            die_at(src_name, f->ens[0].line, f->ens[0].col,
                   "`gives` promises something about the value `%s` returns, and that value has "
                   "no name. Write `-> name: %s` in the signature: you may only promise about "
                   "something you have named", f->name, ty_str(f->ret));
        c->frame += 16;
        f->result_off = c->frame;
        if (f->ret_name) {
            Sym *r = xmalloc(sizeof *r);
            r->kind = SYM_RESULT; r->name = f->ret_name; r->type = f->ret;
            r->offset = f->result_off;
            f->ret_sym = r;
        }
    } else if (f->ret_name) {
        /* Named and never promised anything: harmless, and a name that means
         * nothing is worth saying so rather than quietly ignoring. */
    }

    for (int i = 0; i < f->nreq; i++) check_contract(c, &f->req[i], 0);
    if (f->nens) {
        push_scope(c);
        if (f->ret_sym) scope_add(c, f->ret_sym);
        for (int i = 0; i < f->nens; i++) check_contract(c, &f->ens[i], 1);
        pop_scope(c);
    }
    if (f->nreq || f->nens) { record_syscall(64); record_syscall(93); }

    check_stmt(c, f->body);
    pop_scope(c);

    if (f->ret->kind != TY_VOID && !always_returns(f->body))
        die_at(src_name, f->line, f->col,
               "`%s` returns %s, but its body can reach the end without returning one",
               f->name, ty_str(f->ret));

    f->frame_size = (c->frame + 15) & ~15;
}

void check_unit(Unit *u) {
    C c;
    memset(&c, 0, sizeof c);
    c.unit = u;
    for (Sym *g = u->globals; g; g = g->next)
        if (g->kind == SYM_GLOBAL && g->type->kind == TY_CAP)
            die_at(src_name, g->line, g->col,
                   "`%s` is a global capability. Authority in sav travels as an argument, "
                   "so that reading a signature is enough to know what a function can do", g->name);
    if (!u->entry)
        die_at(src_name, 0, 0, "this program has no `entry fn`, so nothing can start it");
    for (Fn *f = u->fns; f; f = f->next) check_fn(&c, f);
    if (u->entry->ret->kind != TY_VOID && !(ty_is_int(u->entry->ret) && u->entry->ret->bits >= 32))
        die_at(src_name, u->entry->line, u->entry->col,
               "`entry fn %s` returns %s; an exit status is i32 or i64, or nothing at all",
               u->entry->name, ty_str(u->entry->ret));
    record_syscall(93);   /* every program exits, and the manifest says so */
}
