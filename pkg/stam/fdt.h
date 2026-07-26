/* stam — device tree (flattened, .dtb) reader.
 *
 * This is why the kernel can be portable at all. A kernel that writes to
 * 0x9000000 because that is where QEMU's serial port lives will never run
 * anywhere else, and no amount of later work fixes it — the assumption spreads
 * into every driver written afterwards. Reading the tree the firmware hands us
 * costs a few hundred lines once, at the very beginning.
 */
#ifndef STAM_FDT_H
#define STAM_FDT_H
#include <stdint.h>

typedef struct {
    const uint8_t *strukt;      /* the structure block                    */
    uint32_t       strukt_size;
    const char    *strings;     /* the strings block                      */
    uint32_t       total;       /* total blob size, for sanity checks     */
    uint32_t       addr_cells;  /* of the root node: how many 32-bit      */
    uint32_t       size_cells;  /* words an address and a size occupy     */
} fdt_t;

/* A node is referred to by its byte offset into the structure block. */
#define FDT_NONE (-1)

int  fdt_init(fdt_t *f, const void *dtb);
int  fdt_root(const fdt_t *f);
/* Walk every node in order. `depth` is updated as we descend and return. */
int  fdt_next_node(const fdt_t *f, int off, int *depth);
const char *fdt_node_name(const fdt_t *f, int off);
const void *fdt_getprop(const fdt_t *f, int off, const char *name, uint32_t *len);
/* "compatible" is a list of strings, most specific first; match any of them. */
int  fdt_is_compatible(const fdt_t *f, int off, const char *compat);
int  fdt_find_compatible(const fdt_t *f, const char *compat);
int  fdt_find_path(const fdt_t *f, const char *path);
/* Read `cells` big-endian 32-bit words as one address or size. */
uint64_t fdt_cells(const void *p, uint32_t cells);
/* The address in a node's "reg", using the root's #address-cells. */
int  fdt_reg(const fdt_t *f, int off, uint64_t *addr, uint64_t *size);

/* The tree this machine was described by, kept after handover because
 * drivers still need to look things up in it. */
const fdt_t *machine_tree(void);

#endif
