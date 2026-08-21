/* Code generation: a sav tree into aarch64 machine code.
 *
 * Simple on purpose. Values live in x0 (and x1 for the second word of a
 * slice), everything else goes on the stack, and no pass moves an access
 * anywhere. That is not only simplicity: rule 4 says a `dev` access happens
 * exactly as written, and a compiler with no reordering pass keeps that
 * promise by construction rather than by exception.
 *
 * Every address is formed with adrp/add, PC-relative, so the executables
 * savc writes need no relocation and hold no absolute address. The kernel in
 * this repository was broken twice by absolute addresses; sav cannot produce
 * one.
 */
#include "arm.h"

extern u32 *syscalls_used(int *n);
extern const char **caps_used_list(int *n);
extern void record_syscall(u32 n);

#define TEXT_VA 0x400100ull      /* the ELF header and program headers sit below */
#define DATA_VA 0x500000ull

enum { SEC_CODE, SEC_RODATA, SEC_BSS };
enum { FX_B26, FX_B19, FX_ADRP, FX_ADD12 };

typedef struct { size_t at; int kind; int label; int sect; i64 off; } Fix;

typedef struct {
    Unit *u;
    Image *img;
    Buf code;          /* instructions */
    Buf rodata;        /* string literals and contract messages */
    Fix *fix; int nfix;
    i64 *lab; int nlab;
    Fn *fn;
    int brk_lab, cont_lab;
    /* runtime pieces the compiler writes itself */
    int lbl_fail, lbl_argget, lbl_cstr, lbl_num, lbl_write2, lbl_range;
    int need_argget, need_cstr, need_range;
    i64 bss_sp, bss_cstr, bss_numbuf, bss_save, bss_size;
    /* one label per user function */
    int *fn_lab; int nfn;
} G;

/* ---- buffers ----------------------------------------------------------- */
void buf_put(Buf *b, const void *data, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 64;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, data, n);
    b->n += n;
}
void buf_u32(Buf *b, u32 v) { buf_put(b, &v, 4); }
void buf_align(Buf *b, size_t a) {
    static const u8 zero[16] = {0};
    while (b->n % a) buf_put(b, zero, 1);
}

static void emit(G *g, u32 insn) { buf_u32(&g->code, insn); }

static int newlab(G *g) {
    g->lab = realloc(g->lab, sizeof(i64) * (size_t)(g->nlab + 1));
    g->lab[g->nlab] = -1;
    return g->nlab++;
}
static void place(G *g, int l) { g->lab[l] = (i64)g->code.n; }

static void fix_here(G *g, int kind, int label, int sect, i64 off) {
    g->fix = realloc(g->fix, sizeof(Fix) * (size_t)(g->nfix + 1));
    g->fix[g->nfix].at = g->code.n;
    g->fix[g->nfix].kind = kind;
    g->fix[g->nfix].label = label;
    g->fix[g->nfix].sect = sect;
    g->fix[g->nfix].off = off;
    g->nfix++;
}

static void emit_branch(G *g, int label) { fix_here(g, FX_B26, label, 0, 0); emit(g, a_b(0)); }
static void emit_call(G *g, int label) { fix_here(g, FX_B26 + 100, label, 0, 0); emit(g, a_bl(0)); }
static void emit_bcond(G *g, int cond, int label) { fix_here(g, FX_B19, label, 0, 0); emit(g, a_bcond(cond, 0)); }
static void emit_cbz(G *g, int rt, int label) { fix_here(g, FX_B19 + 200, label, 0, 0); emit(g, a_cbz(rt, 0)); }

/* An address, always PC-relative. This is the only way savc names anything,
 * which is why the executables it writes contain no relocations at all. */
static void emit_addr(G *g, int rd, int sect, i64 off) {
    fix_here(g, FX_ADRP, -1, sect, off); emit(g, a_adrp(rd, 0));
    fix_here(g, FX_ADD12, -1, sect, off); emit(g, a_add_imm(rd, rd, 0, 0));
}

static void emit_imm(G *g, int rd, u64 v) {
    if (v == 0) { emit(g, a_movz(rd, 0, 0)); return; }
    i64 sv = (i64)v;
    if (sv < 0 && sv >= -65536) { emit(g, a_movn(rd, (u32)(~v & 0xFFFF), 0)); return; }
    int first = 1;
    for (int hw = 0; hw < 4; hw++) {
        u32 part = (u32)((v >> (16 * hw)) & 0xFFFF);
        if (!part) continue;
        emit(g, first ? a_movz(rd, part, hw) : a_movk(rd, part, hw));
        first = 0;
    }
}

/* sp adjustments and frame offsets outgrow a 12-bit immediate; say so in one
 * place rather than at every call site. */
static void emit_sub_sp(G *g, int amount) {
    if (amount >= 4096) emit(g, a_sub_imm(SP, SP, (u32)(amount >> 12), 1));
    if (amount & 0xFFF) emit(g, a_sub_imm(SP, SP, (u32)(amount & 0xFFF), 0));
}

static void local_addr(G *g, int rd, int off) {
    if (off < 4096) { emit(g, a_sub_imm(rd, X29, (u32)off, 0)); return; }
    emit_imm(g, rd, (u64)off);
    emit(g, a_sub(rd, X29, rd));
}

/* ---- loading and storing values ---------------------------------------- */
static int type_load_size(Type *t) {
    switch (t->kind) {
    case TY_BOOL: return 1;
    case TY_INT: return t->size;
    case TY_PTR: return 8;
    default: return 8;
    }
}

/* Load the value at [rbase, off] into x0 (and x1 for a slice). A struct or an
 * array is not loaded: its address is the value, and a copy only happens
 * where the source says copy. */
static int is_aggregate(Type *t) {
    return t->kind == TY_STRUCT || t->kind == TY_ARRAY || t->kind == TY_ENUM;
}

static void load_value(G *g, int rbase, i64 off, Type *t) {
    if (is_aggregate(t)) {
        if (off == 0) { emit(g, a_mov(X0, rbase)); return; }
        emit_imm(g, X17, (u64)off);
        emit(g, a_add(X0, rbase, X17));
        return;
    }
    if (t->kind == TY_SLICE) {
        emit(g, a_ldr_off(X1, rbase, off + 8, 8, 0));
        emit(g, a_ldr_off(X0, rbase, off, 8, 0));
        return;
    }
    int sz = type_load_size(t);
    int sign = (t->kind == TY_INT) ? t->is_signed : 0;
    emit(g, a_ldr_off(X0, rbase, off, sz, sign));
}

/* Store x0 (and x1) into [rbase, off]. */
static void store_value(G *g, int rbase, i64 off, Type *t) {
    if (t->kind == TY_SLICE) {
        emit(g, a_str_off(X0, rbase, off, 8));
        emit(g, a_str_off(X1, rbase, off + 8, 8));
        return;
    }
    if (is_aggregate(t)) {
        /* x0 holds the address of the source; copy size bytes, eight at a
         * time and then one at a time. */
        int n = t->size, i = 0;
        emit(g, a_mov(X16, rbase));
        for (; i + 8 <= n; i += 8) {
            emit(g, a_ldr_off(X17, X0, i, 8, 0));
            emit(g, a_str_off(X17, X16, off + i, 8));
        }
        for (; i < n; i++) {
            emit(g, a_ldr_off(X17, X0, i, 1, 0));
            emit(g, a_str_off(X17, X16, off + i, 1));
        }
        return;
    }
    emit(g, a_str_off(X0, rbase, off, type_load_size(t)));
}

static void push_pair_fwd(G *g);
static void pop_pair_fwd(G *g, int r1, int r2);

/* A frame slot. The unscaled load and store reach 255 bytes either side of
 * x29, and a frame can be larger than that; rather than let the encoder
 * silently truncate the offset, compute the address when it does not fit. */
static void frame_load(G *g, int off, Type *t) {
    if (off <= 240) { load_value(g, X29, -(i64)off, t); return; }
    local_addr(g, X16, off);
    load_value(g, X16, 0, t);
}
static void frame_store(G *g, int off, Type *t) {
    if (off <= 240) { store_value(g, X29, -(i64)off, t); return; }
    if (is_aggregate(t)) {
        push_pair_fwd(g);
        local_addr(g, X9, off);
        pop_pair_fwd(g, X0, X1);
        store_value(g, X9, 0, t);
        return;
    }
    push_pair_fwd(g);
    local_addr(g, X9, off);
    pop_pair_fwd(g, X0, X1);
    store_value(g, X9, 0, t);
}

/* x0 (and x1) into one field of a value that lives in a frame slot. The
 * address is recomputed rather than held in a register, because whatever
 * produced the value was free to use every register there is. */
static void frame_store_field(G *g, int slot_off, int field_off, Type *t) {
    push_pair_fwd(g);
    local_addr(g, X9, slot_off);
    if (field_off) { emit_imm(g, X17, (u64)field_off); emit(g, a_add(X9, X9, X17)); }
    pop_pair_fwd(g, X0, X1);
    store_value(g, X9, 0, t);
}

/* ---- rodata ------------------------------------------------------------ */
static i64 add_rodata(G *g, const void *p, size_t n) {
    buf_align(&g->rodata, 8);
    i64 off = (i64)g->rodata.n;
    buf_put(&g->rodata, p, n);
    static const u8 nul = 0;
    buf_put(&g->rodata, &nul, 1);      /* literals are NUL-terminated too, which
                                          costs a byte and lets a path be used
                                          where the kernel wants a C string */
    return off;
}

static void gen_expr(G *g, Expr *e);
static void gen_addr(G *g, Expr *e);
static void gen_stmt(G *g, Stmt *s);

/* ---- pushing and popping the expression stack -------------------------- */
static void push_pair(G *g) { emit(g, a_stp_pre(X0, X1, SP, -16)); }
static void pop_pair(G *g, int r1, int r2) { emit(g, a_ldp_post(r1, r2, SP, 16)); }
static void push_pair_fwd(G *g) { push_pair(g); }
static void pop_pair_fwd(G *g, int r1, int r2) { pop_pair(g, r1, r2); }

/* The failure half of a range check: x2 is the value that was wrong, x3 the
 * bound it was checked against. Split out because the passing path must not
 * disturb a register, and the failing one may disturb every one of them. */
static void gen_range_fail(G *g, int line, const char *what,
                           const char *label1, const char *label2) {
    g->need_range = 1;
    char msg[256];
    int n = snprintf(msg, sizeof msg, "%s:%d: %s", src_name, line, what);
    i64 off = add_rodata(g, msg, (size_t)n);
    i64 l1 = add_rodata(g, label1, strlen(label1));
    i64 l2 = add_rodata(g, label2, strlen(label2));
    emit_addr(g, X4, SEC_RODATA, l1); emit_imm(g, X5, strlen(label1));
    emit_addr(g, X6, SEC_RODATA, l2); emit_imm(g, X7, strlen(label2));
    emit_addr(g, X0, SEC_RODATA, off);
    emit_imm(g, X1, (u64)n);
    emit_call(g, g->lbl_range);
}

/* x2 against x1, unsigned — which is one comparison for both ends, because a
 * negative index is a very large unsigned one and fails the same test. */
static void gen_bounds_check(G *g, int line, const char *what) {
    int ok = newlab(g);
    emit(g, a_subs(XZR, X2, X1));
    emit_bcond(g, CC_LO, ok);
    emit(g, a_mov(X3, X1));
    gen_range_fail(g, line, what, "index ", "length ");
    place(g, ok);
}

/* ---- addresses of lvalues ---------------------------------------------- */
static void gen_addr(G *g, Expr *e) {
    switch (e->kind) {
    case E_VAR: {
        Sym *s = e->sym;
        if (s->kind == SYM_GLOBAL) emit_addr(g, X0, SEC_BSS, s->offset);
        else local_addr(g, X0, s->offset);
        return;
    }
    case E_DEREF:
        gen_expr(g, e->a);
        return;
    case E_FIELD: {
        if (e->a->type->kind == TY_PTR) gen_expr(g, e->a);
        else gen_addr(g, e->a);
        if (e->field && e->field->offset) {
            emit_imm(g, X17, (u64)e->field->offset);
            emit(g, a_add(X0, X0, X17));
        }
        return;
    }
    case E_INDEX: {
        Type *at = e->a->type;
        if (at->kind == TY_SLICE) gen_expr(g, e->a);          /* x0 = ptr, x1 = len */
        else if (at->kind == TY_PTR) gen_expr(g, e->a);
        else gen_addr(g, e->a);                                /* array */
        push_pair(g);
        gen_expr(g, e->b);                                     /* index */
        emit(g, a_mov(X2, X0));
        pop_pair(g, X0, X1);
        /* An array's length is known here and nowhere else at run time; a
         * slice carries its own. Either way the check happens before the
         * multiply, on the index the source wrote. */
        if (at->kind == TY_ARRAY) emit_imm(g, X1, (u64)at->nelem);
        gen_bounds_check(g, e->line, "index out of range");
        int esz = at->elem->size;
        if (esz != 1) { emit_imm(g, X17, (u64)esz); emit(g, a_mul(X2, X2, X17)); }
        emit(g, a_add(X0, X0, X2));
        return;
    }
    default:
        die_at(src_name, e->line, e->col, "this is a value, and a value has no address");
    }
}

/* ---- capability methods -------------------------------------------------
 * The only place in the compiler that emits `svc`. Everything a sav program
 * can ask of the kernel passes through this switch, which is why the manifest
 * can be believed: there is no second path to a system call. */
static void gen_method(G *g, Expr *e) {
    CapDef *d = &cap_defs[e->a->type->cap_id];
    CapMethod *m = e->method;

    if (strcmp(d->name, "Console") == 0) {
        if (strcmp(m->name, "write") == 0 || strcmp(m->name, "error") == 0 || strcmp(m->name, "read") == 0) {
            gen_expr(g, e->args[0]);                 /* x0 = ptr, x1 = len */
            emit(g, a_mov(X2, X1));
            emit(g, a_mov(X1, X0));
            emit_imm(g, X0, strcmp(m->name, "write") == 0 ? 1 : strcmp(m->name, "error") == 0 ? 2 : 0);
            emit_imm(g, X8, (u64)m->sysno);
            emit(g, a_svc0());
            return;
        }
    }
    if (strcmp(d->name, "Proc") == 0) {
        emit_imm(g, X8, (u64)m->sysno);
        emit(g, a_svc0());
        return;
    }
    if (strcmp(d->name, "Args") == 0) {
        if (strcmp(m->name, "count") == 0) {
            emit_addr(g, X16, SEC_BSS, g->bss_sp);
            emit(g, a_ldr_off(X16, X16, 0, 8, 0));
            emit(g, a_ldr_off(X0, X16, 0, 8, 0));     /* argc, where the kernel left it */
            return;
        }
        gen_expr(g, e->args[0]);
        g->need_argget = 1;
        emit_call(g, g->lbl_argget);
        return;
    }
    if (strcmp(d->name, "Files") == 0) {
        if (strcmp(m->name, "open") == 0) {
            gen_expr(g, e->args[0]); push_pair(g);          /* path */
            gen_expr(g, e->args[1]); push_pair(g);          /* flags */
            pop_pair(g, X9, X17);                            /* flags */
            pop_pair(g, X0, X1);                             /* path ptr, len */
            g->need_cstr = 1;
            emit_call(g, g->lbl_cstr);                       /* x0 = NUL-terminated copy */
            emit(g, a_mov(X1, X0));
            emit_imm(g, X0, (u64)(i64)-100);                 /* AT_FDCWD */
            emit(g, a_mov(X2, X9));
            emit_imm(g, X3, 0644);
            emit_imm(g, X8, (u64)m->sysno);
            emit(g, a_svc0());
            return;
        }
        if (strcmp(m->name, "read") == 0 || strcmp(m->name, "write") == 0) {
            gen_expr(g, e->args[0]); push_pair(g);          /* fd */
            gen_expr(g, e->args[1]); push_pair(g);          /* buffer */
            pop_pair(g, X1, X2);                             /* ptr, len */
            pop_pair(g, X0, X17);                            /* fd */
            emit_imm(g, X8, (u64)m->sysno);
            emit(g, a_svc0());
            return;
        }
        if (strcmp(m->name, "close") == 0) {
            gen_expr(g, e->args[0]);
            emit_imm(g, X8, (u64)m->sysno);
            emit(g, a_svc0());
            return;
        }
    }
    die_at(src_name, e->line, e->col, "savc cannot yet generate %s.%s", d->name, m->name);
}

/* ---- expressions ------------------------------------------------------- */
static void gen_cmp_set(G *g, int op, int is_signed) {
    /* x0 op x1 -> x0 as 0 or 1 */
    emit(g, a_subs(XZR, X0, X1));
    int cc;
    switch (op) {
    case T_EQEQ: cc = CC_EQ; break;
    case T_NE:   cc = CC_NE; break;
    case T_LT:   cc = is_signed ? CC_LT : CC_LO; break;
    case T_LE:   cc = is_signed ? CC_LE : CC_LS; break;
    case T_GT:   cc = is_signed ? CC_GT : CC_HI; break;
    default:     cc = is_signed ? CC_GE : CC_HS; break;
    }
    emit(g, a_cset(X0, cc));
}

static void gen_expr(G *g, Expr *e) {
    switch (e->kind) {
    case E_INT: case E_BOOL:
        emit_imm(g, X0, (u64)e->ival);
        return;
    case E_STR: {
        i64 off = add_rodata(g, e->str, e->slen);
        emit_addr(g, X0, SEC_RODATA, off);
        emit_imm(g, X1, e->slen);
        return;
    }
    case E_VAR: {
        Sym *s = e->sym;
        if (s->kind == SYM_GLOBAL) {
            emit_addr(g, X16, SEC_BSS, s->offset);
            load_value(g, X16, 0, e->type);
        } else {
            if (is_aggregate(e->type)) local_addr(g, X0, s->offset);
            else frame_load(g, s->offset, e->type);
        }
        return;
    }
    case E_CALL: {
        for (int i = 0; i < e->nargs; i++) { gen_expr(g, e->args[i]); push_pair(g); }
        Sym *p = e->fn->params;
        /* walk to the last parameter, then pop back down: the last argument
         * pushed is the one on top */
        Sym *plist[8]; int n = 0;
        for (Sym *q = p; q; q = q->next) plist[n++] = q;
        for (int i = n - 1; i >= 0; i--) {
            int reg = plist[i]->reg_index;
            int second = (plist[i]->type->kind == TY_SLICE) ? reg + 1 : X17;
            pop_pair(g, reg, second);
        }
        /* An aggregate comes back in space the caller owns: x8 holds its
         * address, which is what AAPCS does and, more to the point, the only
         * thing that works — a value built in the callee's frame is gone the
         * instant the callee returns. */
        if (is_aggregate(e->fn->ret)) local_addr(g, X8, e->temp_off);
        emit_call(g, e->fn->code_off);      /* code_off holds the callee's label */
        return;
    }
    case E_METHOD: gen_method(g, e); return;
    case E_UNARY:
        gen_expr(g, e->a);
        if (e->op == T_MINUS) emit(g, a_neg(X0, X0));
        else if (e->op == T_TILDE) emit(g, a_mvn(X0, X0));
        else { emit_imm(g, X1, 0); emit(g, a_subs(XZR, X0, X1)); emit(g, a_cset(X0, CC_EQ)); }
        return;
    case E_BINARY: {
        int op = e->op;
        if (op == T_ANDAND || op == T_OROR) {
            int done = newlab(g);
            gen_expr(g, e->a);
            if (op == T_ANDAND) {
                emit_cbz(g, X0, done);            /* false, and the answer is false */
            } else {
                emit_imm(g, X1, 0);
                emit(g, a_subs(XZR, X0, X1));
                emit_bcond(g, CC_NE, done);       /* true, and the answer is true */
            }
            gen_expr(g, e->b);
            place(g, done);
            return;
        }
        gen_expr(g, e->a); push_pair(g);
        gen_expr(g, e->b);
        emit(g, a_mov(X2, X0));
        pop_pair(g, X0, X1);
        emit(g, a_mov(X1, X2));
        int is_signed = ty_is_int(e->a->type) ? e->a->type->is_signed : 1;
        switch (op) {
        case T_PLUS:
            if (e->a->type->kind == TY_PTR) {
                int esz = e->a->type->elem->size;
                if (esz != 1) { emit_imm(g, X17, (u64)esz); emit(g, a_mul(X1, X1, X17)); }
            }
            emit(g, a_add(X0, X0, X1)); break;
        case T_MINUS:
            if (e->a->type->kind == TY_PTR) {
                int esz = e->a->type->elem->size;
                if (esz != 1) { emit_imm(g, X17, (u64)esz); emit(g, a_mul(X1, X1, X17)); }
            }
            emit(g, a_sub(X0, X0, X1)); break;
        case T_STAR: emit(g, a_mul(X0, X0, X1)); break;
        case T_SLASH: emit(g, is_signed ? a_sdiv(X0, X0, X1) : a_udiv(X0, X0, X1)); break;
        case T_PERCENT:
            emit(g, is_signed ? a_sdiv(X2, X0, X1) : a_udiv(X2, X0, X1));
            emit(g, a_msub(X0, X2, X1, X0));
            break;
        case T_AMP: emit(g, a_and(X0, X0, X1)); break;
        case T_PIPE: emit(g, a_orr(X0, X0, X1)); break;
        case T_CARET: emit(g, a_eor(X0, X0, X1)); break;
        case T_SHL: emit(g, a_lslv(X0, X0, X1)); break;
        case T_SHR: emit(g, is_signed ? a_asrv(X0, X0, X1) : a_lsrv(X0, X0, X1)); break;
        default: gen_cmp_set(g, op, is_signed); break;
        }
        /* keep a narrow type narrow: the result of adding two u8s is a u8, and
         * the bits above it are not quietly kept */
        if (ty_is_int(e->type) && e->type->bits < 64 && op != T_EQEQ && op != T_NE &&
            op != T_LT && op != T_LE && op != T_GT && op != T_GE)
            emit(g, e->type->is_signed ? a_sxt(X0, X0, e->type->bits) : a_uxt(X0, X0, e->type->bits));
        return;
    }
    case E_INDEX:
        gen_addr(g, e);
        load_value(g, X0, 0, e->type);
        return;
    case E_FIELD:
        gen_addr(g, e);
        load_value(g, X0, 0, e->type);
        return;
    case E_LEN:
        if (e->a->type->kind == TY_ARRAY) { emit_imm(g, X0, (u64)e->a->type->nelem); return; }
        gen_expr(g, e->a);
        emit(g, a_mov(X0, X1));
        return;
    case E_SLICE: {
        Type *at = e->a->type;
        if (at->kind == TY_ARRAY) { gen_addr(g, e->a); emit_imm(g, X1, (u64)at->nelem); }
        else gen_expr(g, e->a);
        push_pair(g);                          /* [sp]  = base ptr, length   */
        gen_expr(g, e->b);
        push_pair(g);                          /* [sp]  = low, base at [sp+16] */
        if (e->c) gen_expr(g, e->c);
        else emit(g, a_ldr_off(X0, SP, 24, 8, 0));   /* to the end: the length */
        emit(g, a_mov(X9, X0));                /* high */
        pop_pair(g, X10, X17);                 /* low  */
        pop_pair(g, X0, X1);                   /* base pointer, length */
        /* A slice of a pointer is the one place a length is asserted rather
         * than carried, and there is nothing to check it against. Everywhere
         * else, both ends are checked. */
        if (at->kind != TY_PTR) {
            int ok1 = newlab(g), ok2 = newlab(g);
            emit(g, a_subs(XZR, X10, X9));
            emit_bcond(g, CC_LS, ok1);
            emit(g, a_mov(X2, X10));
            emit(g, a_mov(X3, X9));
            gen_range_fail(g, e->line, "a slice that starts after it ends", "start ", "end ");
            place(g, ok1);
            emit(g, a_subs(XZR, X9, X1));
            emit_bcond(g, CC_LS, ok2);
            emit(g, a_mov(X2, X9));
            emit(g, a_mov(X3, X1));
            gen_range_fail(g, e->line, "a slice that runs past the end", "end ", "length ");
            place(g, ok2);
        }
        int esz = at->elem->size;
        if (esz != 1) { emit_imm(g, X17, (u64)esz); emit(g, a_mul(X11, X10, X17)); }
        else emit(g, a_mov(X11, X10));
        emit(g, a_add(X0, X0, X11));
        emit(g, a_sub(X1, X9, X10));
        return;
    }
    case E_VARIANT: {
        /* The tag first, then whatever the case carries, into the slot the
         * checker set aside. The value of the expression is its address, the
         * same as a struct's. */
        emit_imm(g, X0, (u64)e->variant->index);
        frame_store_field(g, e->temp_off, 0, ty_int(64, 1));
        for (int i = 0; i < e->nargs; i++) {
            gen_expr(g, e->args[i]);
            frame_store_field(g, e->temp_off, e->variant->fields[i].offset,
                              e->variant->fields[i].type);
        }
        local_addr(g, X0, e->temp_off);
        return;
    }
    case E_ADDR: gen_addr(g, e->a); return;
    case E_DEREF:
        gen_expr(g, e->a);
        load_value(g, X0, 0, e->type);
        return;
    case E_CAST: {
        gen_expr(g, e->a);
        Type *from = e->a->type, *to = e->type;
        if (to->kind == TY_BOOL) {
            emit_imm(g, X1, 0); emit(g, a_subs(XZR, X0, X1)); emit(g, a_cset(X0, CC_NE));
        } else if (ty_is_int(to) && to->bits < 64) {
            emit(g, to->is_signed ? a_sxt(X0, X0, to->bits) : a_uxt(X0, X0, to->bits));
        } else if (ty_is_int(to) && to->bits == 64 && ty_is_int(from) && from->bits < 64) {
            emit(g, from->is_signed ? a_sxt(X0, X0, from->bits) : a_uxt(X0, X0, from->bits));
        }
        return;
    }
    case E_ASSIGN: {
        gen_addr(g, e->a);
        push_pair(g);
        gen_expr(g, e->b);
        pop_pair(g, X16, X17);
        store_value(g, X16, 0, e->a->type);
        return;
    }
    default:
        die_at(src_name, e->line, e->col, "savc does not know how to generate this expression");
    }
}

/* ---- contracts --------------------------------------------------------- */
static void gen_check(G *g, Expr *cond, const char *kind, const char *text, int line) {
    if (!opt_contracts) return;
    int ok = newlab(g);
    gen_expr(g, cond);
    /* cbnz: the check held, and nothing is said. A check that printed either
     * way would be the very defect this language exists to refuse. */
    fix_here(g, FX_B19 + 300, ok, 0, 0);
    emit(g, a_cbnz(X0, 0));
    char msg[512];
    int n = snprintf(msg, sizeof msg, "%s:%d: %s: %s\n", src_name, line, kind, text);
    i64 off = add_rodata(g, msg, (size_t)n);
    emit_addr(g, X0, SEC_RODATA, off);
    emit_imm(g, X1, (u64)n);
    emit_call(g, g->lbl_fail);
    place(g, ok);
}

static void gen_ensures(G *g) {
    for (int i = 0; i < g->fn->nens; i++)
        gen_check(g, g->fn->ens[i].expr, "promise broken", g->fn->ens[i].text, g->fn->ens[i].line);
}

/* ---- statements -------------------------------------------------------- */
static void gen_return(G *g, Stmt *s) {
    if (s->expr && is_aggregate(g->fn->ret)) {
        /* x0 is the address of the value the body built; copy it into the
         * caller's space and hand that address back. */
        gen_expr(g, s->expr);
        push_pair(g);
        frame_load(g, g->fn->sret_off, ty_ptr(ty_int(8, 0), 0));
        emit(g, a_mov(X9, X0));
        pop_pair(g, X0, X1);
        store_value(g, X9, 0, g->fn->ret);
        emit(g, a_mov(X0, X9));
        emit(g, a_mov_sp(SP, X29));
        emit(g, a_ldp_post(X29, X30, SP, 16));
        emit(g, a_ret());
        return;
    }
    if (s->expr) {
        gen_expr(g, s->expr);
        if (g->fn->nens) {
            frame_store(g, g->fn->result_off, g->fn->ret);
            gen_ensures(g);
            frame_load(g, g->fn->result_off, g->fn->ret);
        }
    } else if (g->fn->nens) {
        gen_ensures(g);
    }
    emit(g, a_mov_sp(SP, X29));
    emit(g, a_ldp_post(X29, X30, SP, 16));
    emit(g, a_ret());
}

static void gen_stmt(G *g, Stmt *s) {
    switch (s->kind) {
    case S_BLOCK:
        for (int i = 0; i < s->nstmts; i++) gen_stmt(g, s->stmts[i]);
        return;
    case S_LET:
        if (s->init) {
            gen_expr(g, s->init);
            if (is_aggregate(s->type)) {
                push_pair(g);
                local_addr(g, X9, s->sym->offset);
                pop_pair(g, X0, X1);
                store_value(g, X9, 0, s->type);
            } else {
                frame_store(g, s->sym->offset, s->type);
            }
        }
        return;
    case S_EXPR: gen_expr(g, s->expr); return;
    case S_IF: {
        int els = newlab(g), done = newlab(g);
        gen_expr(g, s->cond);
        emit_cbz(g, X0, els);
        gen_stmt(g, s->then_s);
        if (s->else_s) emit_branch(g, done);
        place(g, els);
        if (s->else_s) { gen_stmt(g, s->else_s); place(g, done); }
        else place(g, done);
        return;
    }
    case S_WHILE: {
        int top = newlab(g), out = newlab(g);
        int ob = g->brk_lab, oc = g->cont_lab;
        g->brk_lab = out; g->cont_lab = top;
        place(g, top);
        gen_expr(g, s->cond);
        emit_cbz(g, X0, out);
        gen_stmt(g, s->body);
        emit_branch(g, top);
        place(g, out);
        g->brk_lab = ob; g->cont_lab = oc;
        return;
    }
    case S_FOR: {
        int top = newlab(g), out = newlab(g), cont = newlab(g);
        int ob = g->brk_lab, oc = g->cont_lab;
        g->brk_lab = out; g->cont_lab = cont;
        gen_expr(g, s->lo);
        frame_store(g, s->sym->offset, s->sym->type);
        place(g, top);
        gen_expr(g, s->hi); push_pair(g);
        frame_load(g, s->sym->offset, s->sym->type);
        pop_pair(g, X1, X17);
        emit(g, a_subs(XZR, X0, X1));
        emit_bcond(g, CC_GE, out);
        gen_stmt(g, s->body);
        place(g, cont);
        frame_load(g, s->sym->offset, s->sym->type);
        emit(g, a_add_imm(X0, X0, 1, 0));
        frame_store(g, s->sym->offset, s->sym->type);
        emit_branch(g, top);
        place(g, out);
        g->brk_lab = ob; g->cont_lab = oc;
        return;
    }
    case S_MATCH: {
        /* The value is matched by its address: nothing is copied, and an arm
         * reads the payload out of the value itself. */
        gen_expr(g, s->expr);
        frame_store_field(g, s->subject_off, 0, ty_ptr(ty_int(8, 0), 0));

        int end = newlab(g);
        int *arm_lab = xmalloc(sizeof(int) * (size_t)s->narms);
        for (int i = 0; i < s->narms; i++) arm_lab[i] = newlab(g);

        frame_load(g, s->subject_off, ty_ptr(ty_int(8, 0), 0));
        emit(g, a_ldr_off(X1, X0, 0, 8, 0));            /* the tag */
        int else_arm = -1;
        for (int i = 0; i < s->narms; i++) {
            if (s->arms[i].is_else) { else_arm = i; continue; }
            emit_imm(g, X2, (u64)s->arms[i].index);
            emit(g, a_subs(XZR, X1, X2));
            emit_bcond(g, CC_EQ, arm_lab[i]);
        }
        emit_branch(g, else_arm >= 0 ? arm_lab[else_arm] : end);

        for (int i = 0; i < s->narms; i++) {
            place(g, arm_lab[i]);
            MatchArm *a = &s->arms[i];
            if (!a->is_else) {
                for (int b = 0; b < a->nbinds; b++) {
                    frame_load(g, s->subject_off, ty_ptr(ty_int(8, 0), 0));
                    load_value(g, X0, a->variant->fields[b].offset, a->variant->fields[b].type);
                    frame_store(g, a->bind_syms[b]->offset, a->variant->fields[b].type);
                }
            }
            gen_stmt(g, a->body);
            emit_branch(g, end);
        }
        place(g, end);
        return;
    }
    case S_RETURN: gen_return(g, s); return;
    case S_BREAK: emit_branch(g, g->brk_lab); return;
    case S_CONTINUE: emit_branch(g, g->cont_lab); return;
    case S_ASSERT: gen_check(g, s->cond, "check failed", s->name, s->line); return;
    }
}

/* ---- the pieces the compiler writes itself ------------------------------ */
static void gen_fail(G *g) {
    /* x0 = message, x1 = length. Says what broke, on standard error, and
     * stops with 70. A contract that failed and let the program continue
     * would be a claim that does not depend on what it claims. */
    place(g, g->lbl_fail);
    emit(g, a_mov(X2, X1));
    emit(g, a_mov(X1, X0));
    emit_imm(g, X0, 2);
    emit_imm(g, X8, 64);
    emit(g, a_svc0());
    emit_imm(g, X0, 70);
    emit_imm(g, X8, 93);
    emit(g, a_svc0());
    emit(g, a_b(0));            /* the kernel does not come back; if it did, stay here */
}

static void gen_argget(G *g) {
    /* x0 = index -> x0 = pointer, x1 = length, both zero when there is no
     * such argument. A program that asks for argv[7] of three gets an empty
     * slice rather than whatever was on the stack. */
    place(g, g->lbl_argget);
    int none = newlab(g), done = newlab(g), loop = newlab(g);
    emit_addr(g, X16, SEC_BSS, g->bss_sp);
    emit(g, a_ldr_off(X16, X16, 0, 8, 0));      /* the sp the kernel handed us */
    emit(g, a_ldr_off(X9, X16, 0, 8, 0));       /* argc */
    emit_imm(g, X10, 0);
    emit(g, a_subs(XZR, X0, X10));
    emit_bcond(g, CC_LT, none);
    emit(g, a_subs(XZR, X0, X9));
    emit_bcond(g, CC_GE, none);
    emit_imm(g, X17, 8);
    emit(g, a_mul(X0, X0, X17));
    emit(g, a_add(X16, X16, X17));              /* argv starts one word up */
    emit(g, a_add(X16, X16, X0));
    emit(g, a_ldr_off(X0, X16, 0, 8, 0));       /* argv[i] */
    emit_imm(g, X1, 0);
    place(g, loop);
    emit(g, a_add(X9, X0, X1));
    emit(g, a_ldr_off(X10, X9, 0, 1, 0));
    fix_here(g, FX_B19 + 200, done, 0, 0);
    emit(g, a_cbz(X10, 0));
    emit(g, a_add_imm(X1, X1, 1, 0));
    emit_branch(g, loop);
    place(g, none);
    emit_imm(g, X0, 0);
    emit_imm(g, X1, 0);
    place(g, done);
    emit(g, a_ret());
}

static void gen_cstr(G *g) {
    /* x0 = bytes, x1 = length -> x0 = a NUL-terminated copy. The kernel wants
     * a C string where sav has a slice; the copy is bounded and truncates
     * rather than running off the end. */
    place(g, g->lbl_cstr);
    int loop = newlab(g), done = newlab(g);
    emit_addr(g, X16, SEC_BSS, g->bss_cstr);
    emit_imm(g, X2, 0);
    emit_imm(g, X3, 255);
    place(g, loop);
    emit(g, a_subs(XZR, X2, X1));
    emit_bcond(g, CC_GE, done);
    emit(g, a_subs(XZR, X2, X3));
    emit_bcond(g, CC_GE, done);
    emit(g, a_add(X4, X0, X2));
    emit(g, a_ldr_off(X5, X4, 0, 1, 0));
    emit(g, a_add(X4, X16, X2));
    emit(g, a_str_off(X5, X4, 0, 1));
    emit(g, a_add_imm(X2, X2, 1, 0));
    emit_branch(g, loop);
    place(g, done);
    emit(g, a_add(X4, X16, X2));
    emit_imm(g, X5, 0);
    emit(g, a_str_off(X5, X4, 0, 1));
    emit(g, a_mov(X0, X16));
    emit(g, a_ret());
}

/* x0 = bytes, x1 = length -> write(2, ...). Every message this compiler's own
 * code prints goes out through here, so there is one place that names fd 2. */
static void gen_write2(G *g) {
    place(g, g->lbl_write2);
    emit(g, a_mov(X2, X1));
    emit(g, a_mov(X1, X0));
    emit_imm(g, X0, 2);
    emit_imm(g, X8, 64);
    emit(g, a_svc0());
    emit(g, a_ret());
}

/* x0 = a signed number -> x0 = bytes, x1 = length, written backwards into a
 * scratch buffer. A range failure that could not say which index and which
 * length would be a report that does not depend on what it reports. */
static void gen_num(G *g) {
    place(g, g->lbl_num);
    int loop = newlab(g), after = newlab(g), zero = newlab(g), nosign = newlab(g);
    emit_addr(g, X16, SEC_BSS, g->bss_numbuf);
    emit_imm(g, X2, 32);              /* write position, from the end */
    emit_imm(g, X3, 0);               /* was it negative */
    emit(g, a_mov(X4, X0));
    emit_imm(g, X5, 0);
    emit(g, a_subs(XZR, X4, X5));
    emit_bcond(g, CC_GE, nosign);
    emit(g, a_neg(X4, X4));
    emit_imm(g, X3, 1);
    place(g, nosign);
    fix_here(g, FX_B19 + 200, zero, 0, 0);
    emit(g, a_cbz(X4, 0));
    emit_imm(g, X6, 10);
    place(g, loop);
    fix_here(g, FX_B19 + 200, after, 0, 0);
    emit(g, a_cbz(X4, 0));
    emit(g, a_udiv(X5, X4, X6));
    emit(g, a_msub(X7, X5, X6, X4));         /* remainder */
    emit(g, a_add_imm(X7, X7, 48, 0));       /* '0' */
    emit(g, a_sub_imm(X2, X2, 1, 0));
    emit(g, a_add(X17, X16, X2));
    emit(g, a_str_off(X7, X17, 0, 1));
    emit(g, a_mov(X4, X5));
    emit_branch(g, loop);
    place(g, zero);
    emit(g, a_sub_imm(X2, X2, 1, 0));
    emit(g, a_add(X17, X16, X2));
    emit_imm(g, X7, 48);
    emit(g, a_str_off(X7, X17, 0, 1));
    place(g, after);
    fix_here(g, FX_B19 + 200, newlab(g), 0, 0);   /* placeholder, patched below */
    {   /* if it was negative, a minus goes in front */
        int done = g->nlab - 1;
        g->fix[g->nfix - 1].label = done;
        emit(g, a_cbz(X3, 0));
        emit(g, a_sub_imm(X2, X2, 1, 0));
        emit(g, a_add(X17, X16, X2));
        emit_imm(g, X7, 45);
        emit(g, a_str_off(X7, X17, 0, 1));
        place(g, done);
    }
    emit(g, a_add(X0, X16, X2));
    emit_imm(g, X1, 32);
    emit(g, a_sub(X1, X1, X2));
    emit(g, a_ret());
}

/* The one place a range failure is reported.
 *
 *   x0, x1  the message
 *   x2, x3  the two numbers
 *   x4, x5  what the first number is called
 *   x6, x7  what the second is called
 *
 * The labels come from the caller because "index" and "length" are the right
 * words for an index and the wrong ones for the two ends of a slice, and a
 * message that names the wrong thing is worse than one that names nothing.
 * It never returns, so it may use x30 freely.
 */
static void gen_range(G *g) {
    place(g, g->lbl_range);
    i64 o_open = add_rodata(g, " (", 2);
    i64 o_comma = add_rodata(g, ", ", 2);
    i64 o_end = add_rodata(g, ")\n", 2);

    emit_addr(g, X16, SEC_BSS, g->bss_save);
    emit(g, a_str_off(X2, X16, 0, 8));
    emit(g, a_str_off(X3, X16, 8, 8));
    emit(g, a_str_off(X4, X16, 16, 8));
    emit(g, a_str_off(X5, X16, 24, 8));
    emit(g, a_str_off(X6, X16, 32, 8));
    emit(g, a_str_off(X7, X16, 40, 8));
    emit_call(g, g->lbl_write2);                        /* the message */

    emit_addr(g, X0, SEC_RODATA, o_open); emit_imm(g, X1, 2);
    emit_call(g, g->lbl_write2);

    emit_addr(g, X16, SEC_BSS, g->bss_save);            /* the first label */
    emit(g, a_ldr_off(X0, X16, 16, 8, 0));
    emit(g, a_ldr_off(X1, X16, 24, 8, 0));
    emit_call(g, g->lbl_write2);
    emit_addr(g, X16, SEC_BSS, g->bss_save);            /* and its number */
    emit(g, a_ldr_off(X0, X16, 0, 8, 0));
    emit_call(g, g->lbl_num);
    emit_call(g, g->lbl_write2);

    emit_addr(g, X0, SEC_RODATA, o_comma); emit_imm(g, X1, 2);
    emit_call(g, g->lbl_write2);

    emit_addr(g, X16, SEC_BSS, g->bss_save);            /* the second label */
    emit(g, a_ldr_off(X0, X16, 32, 8, 0));
    emit(g, a_ldr_off(X1, X16, 40, 8, 0));
    emit_call(g, g->lbl_write2);
    emit_addr(g, X16, SEC_BSS, g->bss_save);            /* and its number */
    emit(g, a_ldr_off(X0, X16, 8, 8, 0));
    emit_call(g, g->lbl_num);
    emit_call(g, g->lbl_write2);

    emit_addr(g, X0, SEC_RODATA, o_end); emit_imm(g, X1, 2);
    emit_call(g, g->lbl_write2);

    emit_imm(g, X0, 70);
    emit_imm(g, X8, 93);
    emit(g, a_svc0());
    emit(g, a_b(0));
}

/* ---- a function -------------------------------------------------------- */
static void gen_fn(G *g, Fn *f, int label) {
    /* The message a broken contract prints is built here, and it names a file
     * and a line: they must be the ones the clause was written in. */
    if (f->file) { src_name = f->file; src_text = f->text; }
    g->fn = f;
    place(g, label);
    emit(g, a_stp_pre(X29, X30, SP, -16));
    emit(g, a_mov_sp(X29, SP));
    if (f->frame_size) emit_sub_sp(g, f->frame_size);
    /* Where the caller wants an aggregate result put, kept before anything
     * else runs — and written straight from x8, because x0 through x7 hold
     * the arguments and a save that clobbered one of them would break every
     * function that returns a value and takes one. */
    if (is_aggregate(f->ret)) {
        if (f->sret_off <= 240) emit(g, a_str_off(X8, X29, -(i64)f->sret_off, 8));
        else { local_addr(g, X16, f->sret_off); emit(g, a_str_off(X8, X16, 0, 8)); }
    }

    /* parameters arrive in registers and are written to the frame at once, so
     * that everything below has one place to look for them */
    for (Sym *p = f->params; p; p = p->next) {
        int reg = p->reg_index;
        if (p->type->kind == TY_SLICE) {
            emit(g, a_mov(X0, reg)); emit(g, a_mov(X1, reg + 1));
        } else {
            emit(g, a_mov(X0, reg));
        }
        frame_store(g, p->offset, p->type);
    }

    for (int i = 0; i < f->nreq; i++)
        gen_check(g, f->req[i].expr, "requirement broken", f->req[i].text, f->req[i].line);

    gen_stmt(g, f->body);

    /* falling off the end of a void function is a return */
    if (f->nens) gen_ensures(g);
    emit(g, a_mov_sp(SP, X29));
    emit(g, a_ldp_post(X29, X30, SP, 16));
    emit(g, a_ret());
}

/* ---- the entry code ----------------------------------------------------- */
static void gen_start(G *g, int main_label) {
    emit(g, a_mov_sp(X0, SP));
    emit_addr(g, X16, SEC_BSS, g->bss_sp);
    emit(g, a_str_off(X0, X16, 0, 8));
    /* Hand `main` the capabilities it asked for. A capability's value is its
     * identity and nothing else: what makes it authority is that the kernel
     * was told, in the note this compiler writes, which calls the program may
     * make — and refuses the rest. */
    int reg = 0;
    for (Sym *p = g->u->entry->params; p; p = p->next)
        emit_imm(g, reg++, (u64)(p->type->cap_id + 1));
    emit_call(g, main_label);
    if (g->u->entry->ret->kind == TY_VOID) emit_imm(g, X0, 0);
    emit_imm(g, X8, 93);
    emit(g, a_svc0());
    emit(g, a_b(0));
}

/* ---- putting it together ------------------------------------------------ */
void gen_unit(Unit *u, Image *img) {
    G g;
    memset(&g, 0, sizeof g);
    g.u = u; g.img = img;

    /* globals first, then the two words the compiler's own code needs */
    i64 bss = 0;
    for (Sym *s = u->globals; s; s = s->next) {
        if (s->kind != SYM_GLOBAL) continue;
        int a = s->type->align < 8 ? 8 : s->type->align;
        bss = (bss + a - 1) & ~(i64)(a - 1);
        s->offset = (int)bss;
        bss += s->type->size;
    }
    bss = (bss + 7) & ~7ll; g.bss_sp = bss; bss += 8;
    bss = (bss + 7) & ~7ll; g.bss_cstr = bss; bss += 256;
    bss = (bss + 7) & ~7ll; g.bss_numbuf = bss; bss += 32;
    bss = (bss + 7) & ~7ll; g.bss_save = bss; bss += 16;
    g.bss_size = bss;

    g.lbl_fail = newlab(&g);
    g.lbl_argget = newlab(&g);
    g.lbl_cstr = newlab(&g);
    g.lbl_num = newlab(&g);
    g.lbl_write2 = newlab(&g);
    g.lbl_range = newlab(&g);

    int nfn = 0;
    for (Fn *f = u->fns; f; f = f->next) nfn++;
    g.fn_lab = xmalloc(sizeof(int) * (size_t)(nfn + 1));
    g.nfn = nfn;
    int i = 0;
    for (Fn *f = u->fns; f; f = f->next) {
        g.fn_lab[i] = newlab(&g);
        f->code_off = g.fn_lab[i];      /* the label, until the address is known */
        i++;
    }

    int start_at = (int)g.code.n;
    gen_start(&g, u->entry->code_off);

    i = 0;
    for (Fn *f = u->fns; f; f = f->next) gen_fn(&g, f, g.fn_lab[i++]);

    gen_fail(&g);
    if (g.need_argget) gen_argget(&g);
    if (g.need_cstr) gen_cstr(&g);
    if (g.need_range) { gen_write2(&g); gen_num(&g); gen_range(&g); }

    /* rodata goes after the code, in the same segment: it is read, never
     * written, and one segment is one thing to get right */
    buf_align(&g.code, 8);
    size_t rodata_at = g.code.n;
    buf_put(&g.code, g.rodata.p, g.rodata.n);

    /* ---- fixups: every branch and every address, now that both ends exist */
    for (int k = 0; k < g.nfix; k++) {
        Fix *fx = &g.fix[k];
        u32 *slot = (u32 *)(g.code.p + fx->at);
        if (fx->kind == FX_B26 || fx->kind == FX_B26 + 100) {
            i64 target = g.lab[fx->label];
            if (target < 0) { fprintf(stderr, "savc: internal: unplaced label %d\n", fx->label); exit(2); }
            i64 delta = (target - (i64)fx->at) / 4;
            *slot = (fx->kind == FX_B26) ? a_b((int)delta) : a_bl((int)delta);
        } else if (fx->kind == FX_B19 || fx->kind == FX_B19 + 200 || fx->kind == FX_B19 + 300) {
            i64 target = g.lab[fx->label];
            if (target < 0) { fprintf(stderr, "savc: internal: unplaced label %d\n", fx->label); exit(2); }
            i64 delta = (target - (i64)fx->at) / 4;
            u32 old = *slot;
            if (fx->kind == FX_B19) *slot = a_bcond((int)(old & 15), (int)delta);
            else if (fx->kind == FX_B19 + 200) *slot = a_cbz((int)(old & 31), (int)delta);
            else *slot = a_cbnz((int)(old & 31), (int)delta);
        } else {
            u64 base = (fx->sect == SEC_BSS) ? DATA_VA
                     : (fx->sect == SEC_RODATA) ? TEXT_VA + rodata_at
                     : TEXT_VA;
            u64 target = base + (u64)fx->off;
            u64 pc = TEXT_VA + fx->at;
            if (fx->kind == FX_ADRP) {
                i64 page = (i64)((target >> 12) - (pc >> 12));
                *slot = a_adrp((int)(*slot & 31), (int)page);
            } else {
                *slot = a_add_imm((int)(*slot & 31), (int)((*slot >> 5) & 31), (u32)(target & 0xFFF), 0);
            }
        }
    }

    img->text = g.code;
    img->rodata_off = rodata_at;
    img->text_vaddr = TEXT_VA;
    img->data_vaddr = DATA_VA;
    img->bss_size = (size_t)g.bss_size;
    img->entry = TEXT_VA + (u64)start_at;
    img->syscalls = syscalls_used(&img->nsyscalls);
    img->capnames = caps_used_list(&img->ncapnames);
}
