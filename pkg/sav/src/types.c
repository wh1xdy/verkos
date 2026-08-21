/* Types, and the capabilities — which are types whose values only the
 * compiler can make.
 *
 * The capability table is the single place that says which system call a
 * program can reach and by which name. The manifest written into the
 * executable is built from these numbers, so a capability cannot gain a call
 * without the note gaining it too: there is no second list to forget.
 */
#include "sav.h"

static Type prim_void = { .kind = TY_VOID, .size = 0, .align = 1 };
static Type prim_bool = { .kind = TY_BOOL, .size = 1, .align = 1 };

Type *ty_void(void) { return &prim_void; }
Type *ty_bool(void) { return &prim_bool; }

Type *ty_int(int bits, int is_signed) {
    static Type cache[2][4];
    int i = bits == 8 ? 0 : bits == 16 ? 1 : bits == 32 ? 2 : 3;
    Type *t = &cache[is_signed ? 1 : 0][i];
    if (!t->size) {
        t->kind = TY_INT; t->bits = bits; t->is_signed = is_signed;
        t->size = bits / 8; t->align = bits / 8;
    }
    return t;
}

Type *ty_ptr(Type *elem, int is_dev) {
    Type *t = xmalloc(sizeof *t);
    t->kind = TY_PTR; t->elem = elem; t->is_dev = is_dev;
    t->size = 8; t->align = 8;
    return t;
}

/* A slice is a pointer and a length, and the length travels with it. An
 * array that decayed into a bare pointer is how a buffer overrun starts, so
 * sav has no decay: the only way to talk about a run of elements is a slice,
 * and it always knows how many. */
Type *ty_slice(Type *elem) {
    Type *t = xmalloc(sizeof *t);
    t->kind = TY_SLICE; t->elem = elem;
    t->size = 16; t->align = 8;
    return t;
}

Type *ty_array(Type *elem, i64 n) {
    Type *t = xmalloc(sizeof *t);
    t->kind = TY_ARRAY; t->elem = elem; t->nelem = n;
    t->size = (int)(elem->size * n); t->align = elem->align;
    return t;
}

int ty_is_int(Type *t) { return t->kind == TY_INT; }

int ty_same(Type *a, Type *b) {
    if (a == b) return 1;
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
    case TY_INT:   return a->bits == b->bits && a->is_signed == b->is_signed;
    case TY_PTR:   return a->is_dev == b->is_dev && ty_same(a->elem, b->elem);
    case TY_SLICE: return ty_same(a->elem, b->elem);
    case TY_ARRAY: return a->nelem == b->nelem && ty_same(a->elem, b->elem);
    case TY_STRUCT: case TY_ENUM: return strcmp(a->name, b->name) == 0;
    case TY_CAP:   return a->cap_id == b->cap_id;
    default: return 1;
    }
}

const char *ty_str(Type *t) {
    static char bufs[8][128];
    static int which = 0;
    char *b = bufs[which = (which + 1) % 8];
    switch (t->kind) {
    case TY_VOID: snprintf(b, 128, "void"); break;
    case TY_BOOL: snprintf(b, 128, "bool"); break;
    case TY_INT:  snprintf(b, 128, "%c%d", t->is_signed ? 'i' : 'u', t->bits); break;
    case TY_PTR:  snprintf(b, 128, "%s*%s", t->is_dev ? "dev " : "", ty_str(t->elem)); break;
    case TY_SLICE: snprintf(b, 128, "[]%s", ty_str(t->elem)); break;
    case TY_ARRAY: snprintf(b, 128, "[%lld]%s", (long long)t->nelem, ty_str(t->elem)); break;
    case TY_STRUCT: case TY_ENUM: snprintf(b, 128, "%s", t->name); break;
    case TY_CAP:  snprintf(b, 128, "%s", cap_defs[t->cap_id].name); break;
    }
    return b;
}

/* ---- the capabilities ---------------------------------------------------
 * sysno 0 means the method needs nothing from the kernel: Args reads the
 * stack the kernel already handed the program, and costs it no authority.
 * That is worth having in the table, because it shows in the manifest as a
 * capability that grants no call. */
CapDef cap_defs[] = {
    { "Console", 0, {
        { "write", 64, 1, "s", 'i', 1 },   /* write(1, ptr, len) */
        { "error", 64, 1, "s", 'i', 2 },   /* write(2, ptr, len) */
        { "read",  63, 1, "s", 'i', 0 },   /* read(0, ptr, len)  */
      }, 3 },
    { "Args", 1, {
        { "count", 0, 0, "",  'i', -1 },
        { "get",   0, 1, "i", 's', -1 },
      }, 2 },
    { "Files", 2, {
        { "open",  56, 2, "si", 'i', -1 },  /* openat(AT_FDCWD, path, flags, 0644) */
        { "read",  63, 2, "is", 'i', -1 },
        { "write", 64, 2, "is", 'i', -1 },
        { "close", 57, 1, "i",  'i', -1 },
      }, 4 },
    { "Proc", 3, {
        { "pid",   172, 0, "", 'i', -1 },
      }, 1 },
};
int n_cap_defs = (int)(sizeof cap_defs / sizeof cap_defs[0]);

CapDef *cap_by_name(const char *name) {
    for (int i = 0; i < n_cap_defs; i++)
        if (strcmp(cap_defs[i].name, name) == 0) return &cap_defs[i];
    return NULL;
}

Type *ty_cap(CapDef *d) {
    static Type cache[8];
    Type *t = &cache[d->id];
    if (!t->size) { t->kind = TY_CAP; t->cap_id = d->id; t->size = 8; t->align = 8; }
    return t;
}
