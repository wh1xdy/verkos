/* vkern — flattened device tree reader. See fdt.h for why this exists. */
#include "fdt.h"
#include <stddef.h>

extern size_t strlen(const char *);
extern int strcmp(const char *, const char *);
extern int strncmp(const char *, const char *, size_t);

#define FDT_MAGIC       0xd00dfeedu
#define FDT_BEGIN_NODE  1u
#define FDT_END_NODE    2u
#define FDT_PROP        3u
#define FDT_NOP         4u
#define FDT_END         9u

/* Everything in a device tree is big-endian, whatever the machine is. */
static uint32_t be32(const void *p) {
    const uint8_t *b = p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  |  (uint32_t)b[3];
}

static uint32_t align4(uint32_t n) { return (n + 3) & ~3u; }

int fdt_init(fdt_t *f, const void *dtb) {
    const uint8_t *h = dtb;
    if (!dtb || be32(h) != FDT_MAGIC) return -1;
    f->total       = be32(h + 4);
    f->strukt      = h + be32(h + 8);
    f->strings     = (const char *)h + be32(h + 12);
    f->strukt_size = be32(h + 36);
    /* Defaults from the specification, overridden by the root node if it says
     * otherwise. Getting these wrong shifts every address we read by a word. */
    f->addr_cells = 2;
    f->size_cells = 1;
    int root = fdt_root(f);
    if (root != FDT_NONE) {
        uint32_t len;
        const void *p = fdt_getprop(f, root, "#address-cells", &len);
        if (p && len == 4) f->addr_cells = be32(p);
        p = fdt_getprop(f, root, "#size-cells", &len);
        if (p && len == 4) f->size_cells = be32(p);
    }
    return 0;
}

int fdt_root(const fdt_t *f) {
    uint32_t off = 0;
    while (off + 4 <= f->strukt_size) {
        uint32_t tok = be32(f->strukt + off);
        if (tok == FDT_BEGIN_NODE) return (int)off;
        if (tok == FDT_END) break;
        off += 4;
    }
    return FDT_NONE;
}

/* Step over one token, returning the offset of the next. */
static int step(const fdt_t *f, uint32_t off, uint32_t *tok_out) {
    if (off + 4 > f->strukt_size) return FDT_NONE;
    uint32_t tok = be32(f->strukt + off);
    *tok_out = tok;
    switch (tok) {
    case FDT_BEGIN_NODE: {
        const char *name = (const char *)f->strukt + off + 4;
        return (int)(off + 4 + align4((uint32_t)strlen(name) + 1));
    }
    case FDT_PROP: {
        if (off + 12 > f->strukt_size) return FDT_NONE;
        uint32_t len = be32(f->strukt + off + 4);
        return (int)(off + 12 + align4(len));
    }
    case FDT_END_NODE:
    case FDT_NOP:
        return (int)(off + 4);
    default:
        return FDT_NONE;          /* FDT_END, or a corrupt tree */
    }
}

int fdt_next_node(const fdt_t *f, int off, int *depth) {
    uint32_t tok;
    int p = off;
    /* Move past the node we are standing on, then find the next BEGIN_NODE,
     * tracking how deep we are as we cross node boundaries. */
    p = step(f, (uint32_t)p, &tok);
    while (p != FDT_NONE) {
        uint32_t t;
        int nxt = step(f, (uint32_t)p, &t);
        if (t == FDT_BEGIN_NODE) { (*depth)++; return p; }
        if (t == FDT_END_NODE)   { (*depth)--; if (*depth < 0) return FDT_NONE; }
        if (t == FDT_END || nxt == FDT_NONE) return FDT_NONE;
        p = nxt;
    }
    return FDT_NONE;
}

const char *fdt_node_name(const fdt_t *f, int off) {
    if (off == FDT_NONE) return "";
    return (const char *)f->strukt + off + 4;
}

const void *fdt_getprop(const fdt_t *f, int off, const char *name, uint32_t *len) {
    if (off == FDT_NONE) return 0;
    uint32_t tok;
    int p = step(f, (uint32_t)off, &tok);      /* past BEGIN_NODE + name */
    /* A node's own properties come before any child node. Stop at the first
     * BEGIN_NODE or END_NODE so we never read a child's property as our own. */
    while (p != FDT_NONE && p + 4 <= (int)f->strukt_size) {
        uint32_t t = be32(f->strukt + p);
        if (t == FDT_NOP) { p += 4; continue; }
        if (t != FDT_PROP) return 0;
        uint32_t plen = be32(f->strukt + p + 4);
        uint32_t noff = be32(f->strukt + p + 8);
        if (!strcmp(f->strings + noff, name)) {
            if (len) *len = plen;
            return f->strukt + p + 12;
        }
        p = (int)(p + 12 + align4(plen));
    }
    return 0;
}

int fdt_is_compatible(const fdt_t *f, int off, const char *compat) {
    uint32_t len = 0;
    const char *p = fdt_getprop(f, off, "compatible", &len);
    if (!p) return 0;
    /* "compatible" is a list of NUL-separated strings, most specific first. */
    for (uint32_t i = 0; i < len; ) {
        if (!strcmp(p + i, compat)) return 1;
        i += (uint32_t)strlen(p + i) + 1;
    }
    return 0;
}

int fdt_find_compatible(const fdt_t *f, const char *compat) {
    int depth = 0;
    for (int n = fdt_root(f); n != FDT_NONE; n = fdt_next_node(f, n, &depth))
        if (fdt_is_compatible(f, n, compat)) return n;
    return FDT_NONE;
}

int fdt_find_path(const fdt_t *f, const char *path) {
    if (path[0] != '/') return FDT_NONE;
    if (!path[1]) return fdt_root(f);
    /* Nodes carry a unit address ("pl011@9000000"); a path may give the name
     * with or without it, so match up to the '@' when the path has none. */
    int depth = 0, want = 1, cur = 0;
    const char *seg = path + 1;
    for (int n = fdt_root(f); n != FDT_NONE; n = fdt_next_node(f, n, &depth)) {
        if (depth != want) continue;
        const char *nm = fdt_node_name(f, n);
        const char *e = seg; while (*e && *e != '/') e++;
        size_t sl = (size_t)(e - seg);
        size_t nl = strlen(nm);
        const char *at = nm; while (*at && *at != '@') at++;
        size_t bare = (size_t)(at - nm);
        if ((sl == nl && !strncmp(nm, seg, sl)) ||
            (sl == bare && !strncmp(nm, seg, sl))) {
            if (!*e) return n;
            seg = e + 1; want++; cur = n; (void)cur;
        }
    }
    return FDT_NONE;
}

uint64_t fdt_cells(const void *p, uint32_t cells) {
    const uint8_t *b = p;
    uint64_t v = 0;
    for (uint32_t i = 0; i < cells; i++) v = (v << 32) | be32(b + i * 4);
    return v;
}

int fdt_reg(const fdt_t *f, int off, uint64_t *addr, uint64_t *size) {
    uint32_t len = 0;
    const void *p = fdt_getprop(f, off, "reg", &len);
    if (!p || len < (f->addr_cells + f->size_cells) * 4) return -1;
    if (addr) *addr = fdt_cells(p, f->addr_cells);
    if (size) *size = fdt_cells((const uint8_t *)p + f->addr_cells * 4, f->size_cells);
    return 0;
}
