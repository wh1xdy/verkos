/* fdtgen.h — build a flattened device tree, from a UEFI application.
 *
 * This exists because of an asymmetry: on aarch64 the firmware usually hands
 * the kernel a device tree, and on x86_64 nothing ever does. Rather than give
 * the kernel a second way to learn about hardware — ACPI tables on one
 * architecture, a tree on the other, and every driver written twice — the
 * loader builds a tree and installs it under the same GUID the firmware would
 * have used. The kernel cannot tell the difference, and does not need to.
 *
 * A .dtb is four blocks: a header, a reserved-memory list, a structure block of
 * nested tokens, and a string table the structure block indexes into. All of it
 * is big-endian regardless of the machine, which is the single easiest thing to
 * get wrong here.
 */
#ifndef FDTGEN_H
#define FDTGEN_H

#define FDT_MAGIC_     0xd00dfeedu
#define FDT_BEGIN_NODE_ 1u
#define FDT_END_NODE_   2u
#define FDT_PROP_       3u
#define FDT_END_        9u

typedef struct {
    unsigned char *buf;
    unsigned       cap;
    unsigned       stroff;      /* next free byte in the string table    */
    unsigned       stcap;
    unsigned       off;         /* next free byte in the structure block */
    int            overflow;    /* set once, checked at the end          */
} fdtgen_t;

/* Layout inside the caller's buffer: header, then the structure block, then
 * the strings. The structure block is given three quarters of the space; a
 * tree with more property *names* than content would be unusual. */
#define FDTGEN_HDR 64

static void fg_be32(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}

static void fg_init(fdtgen_t *g, void *buf, unsigned cap) {
    g->buf = (unsigned char *)buf; g->cap = cap; g->off = 0;
    g->stcap = cap / 4; g->stroff = 0; g->overflow = 0;
    for (unsigned i = 0; i < cap; i++) g->buf[i] = 0;
}

static unsigned char *fg_struct(fdtgen_t *g) { return g->buf + FDTGEN_HDR; }
static char *fg_strings(fdtgen_t *g) { return (char *)g->buf + g->cap - g->stcap; }

static void fg_put32(fdtgen_t *g, unsigned v) {
    if (FDTGEN_HDR + g->off + 4 > g->cap - g->stcap) { g->overflow = 1; return; }
    fg_be32(fg_struct(g) + g->off, v); g->off += 4;
}

static void fg_putbytes(fdtgen_t *g, const void *src, unsigned n) {
    unsigned pad = (4 - (n & 3)) & 3;
    if (FDTGEN_HDR + g->off + n + pad > g->cap - g->stcap) { g->overflow = 1; return; }
    const unsigned char *s = (const unsigned char *)src;
    for (unsigned i = 0; i < n; i++) fg_struct(g)[g->off + i] = s[i];
    g->off += n + pad;
}

static unsigned fg_slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }

/* Property names are pooled: the same name used on ten nodes is stored once,
 * which is what the string block is for. */
static unsigned fg_intern(fdtgen_t *g, const char *name) {
    char *st = fg_strings(g);
    for (unsigned i = 0; i < g->stroff; ) {
        const char *cand = st + i;
        unsigned j = 0;
        while (cand[j] && cand[j] == name[j]) j++;
        if (!cand[j] && !name[j]) return i;
        i += fg_slen(cand) + 1;
    }
    unsigned n = fg_slen(name) + 1;
    if (g->stroff + n > g->stcap) { g->overflow = 1; return 0; }
    unsigned at = g->stroff;
    for (unsigned i = 0; i < n; i++) st[at + i] = name[i];
    g->stroff += n;
    return at;
}

static void fg_node(fdtgen_t *g, const char *name) {
    fg_put32(g, FDT_BEGIN_NODE_);
    fg_putbytes(g, name, fg_slen(name) + 1);
}
static void fg_end_node(fdtgen_t *g) { fg_put32(g, FDT_END_NODE_); }

static void fg_prop(fdtgen_t *g, const char *name, const void *val, unsigned len) {
    unsigned noff = fg_intern(g, name);
    fg_put32(g, FDT_PROP_);
    fg_put32(g, len);
    fg_put32(g, noff);
    if (len) fg_putbytes(g, val, len);
}

static void fg_prop_str(fdtgen_t *g, const char *name, const char *s) {
    fg_prop(g, name, s, fg_slen(s) + 1);
}
static void fg_prop_u32(fdtgen_t *g, const char *name, unsigned v) {
    unsigned char b[4]; fg_be32(b, v); fg_prop(g, name, b, 4);
}
static void fg_prop_u64(fdtgen_t *g, const char *name, unsigned long long v) {
    unsigned char b[8];
    fg_be32(b, (unsigned)(v >> 32)); fg_be32(b + 4, (unsigned)v);
    fg_prop(g, name, b, 8);
}
/* An address/size pair as two 64-bit cells — what #address-cells = <2> and
 * #size-cells = <2> mean, which is what we declare on the root. */
static void fg_prop_reg(fdtgen_t *g, unsigned long long addr, unsigned long long size) {
    unsigned char b[16];
    fg_be32(b, (unsigned)(addr >> 32));     fg_be32(b + 4,  (unsigned)addr);
    fg_be32(b + 8, (unsigned)(size >> 32)); fg_be32(b + 12, (unsigned)size);
    fg_prop(g, "reg", b, 16);
}

/* Assemble the header and move the strings up against the structure block.
 * Returns the total size, or 0 if anything overflowed along the way. */
static unsigned fg_finish(fdtgen_t *g) {
    fg_put32(g, FDT_END_);
    if (g->overflow) return 0;

    unsigned struct_off = FDTGEN_HDR;
    unsigned struct_size = g->off;
    unsigned strings_off = struct_off + struct_size;

    char *from = fg_strings(g);
    unsigned char *to = g->buf + strings_off;
    for (unsigned i = 0; i < g->stroff; i++) to[i] = (unsigned char)from[i];
    /* clear whatever the strings used to occupy, past the new end */
    for (unsigned char *p = to + g->stroff; p < g->buf + g->cap; p++) *p = 0;

    unsigned total = strings_off + g->stroff;
    unsigned char *h = g->buf;
    fg_be32(h +  0, FDT_MAGIC_);
    fg_be32(h +  4, total);
    fg_be32(h +  8, struct_off);
    fg_be32(h + 12, strings_off);
    fg_be32(h + 16, 40);              /* memory reservation block          */
    fg_be32(h + 20, 17);              /* version                           */
    fg_be32(h + 24, 16);              /* last compatible version           */
    fg_be32(h + 28, 0);               /* boot cpu                          */
    fg_be32(h + 32, g->stroff);
    fg_be32(h + 36, struct_size);
    /* the reservation block: one terminating all-zero entry, already zero */
    return total;
}

#endif
