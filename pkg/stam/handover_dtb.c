/* stam — the device-tree handover shim.
 *
 * Turns "a pointer to a DTB" into a boot_info_t. This is the only file that
 * knows the kernel was started the ARM way; everything above it just reads
 * the struct.
 */
#include "bootinfo.h"
#include "fdt.h"
#include <stddef.h>

extern int strcmp(const char *, const char *);

static fdt_t tree;

/* Set by a shim that learned the command line some other way than from the
 * tree — UEFI's LoadOptions, for instance. It wins, because a loader that
 * went to the trouble of passing one meant it. */
static const char *cmdline_override;
void handover_set_cmdline(const char *s) { if (s && *s) cmdline_override = s; }

const fdt_t *machine_tree(void) { return &tree; }

int handover_dtb(const void *dtb, boot_info_t *bi) {
    if (fdt_init(&tree, dtb) != 0) return -1;

    bi->dtb   = dtb;
    bi->model = "unknown machine";
    uint32_t len = 0;

    int root = fdt_root(&tree);
    const char *m = fdt_getprop(&tree, root, "model", &len);
    if (m && len) bi->model = m;

    /* The command line and the initrd both live under /chosen. */
    int chosen = fdt_find_path(&tree, "/chosen");
    if (chosen != FDT_NONE) {
        const char *args = fdt_getprop(&tree, chosen, "bootargs", &len);
        if (args && len) bi->cmdline = args;
        if (cmdline_override) bi->cmdline = cmdline_override;
        /* These are written as either 32- or 64-bit values depending on who
         * built the tree, so trust the property length rather than assuming. */
        const void *p = fdt_getprop(&tree, chosen, "linux,initrd-start", &len);
        if (p && (len == 4 || len == 8)) bi->initrd_start = fdt_cells(p, len / 4);
        p = fdt_getprop(&tree, chosen, "linux,initrd-end", &len);
        if (p && (len == 4 || len == 8)) bi->initrd_end = fdt_cells(p, len / 4);
    }

    /* Memory. A machine may have several banks; milestone 1 needs to know
     * that memory exists and roughly how much, so take the first bank and
     * leave a proper multi-bank allocator for when there is an allocator. */
    int depth = 0;
    for (int n = fdt_root(&tree); n != FDT_NONE; n = fdt_next_node(&tree, n, &depth)) {
        const char *dt = fdt_getprop(&tree, n, "device_type", &len);
        if (dt && !strcmp(dt, "memory")) {
            fdt_reg(&tree, n, &bi->ram_base, &bi->ram_size);
            break;
        }
    }

    /* CPUs: every node under /cpus with device_type = "cpu". */
    depth = 0;
    for (int n = fdt_root(&tree); n != FDT_NONE; n = fdt_next_node(&tree, n, &depth)) {
        const char *dt = fdt_getprop(&tree, n, "device_type", &len);
        if (dt && !strcmp(dt, "cpu")) bi->cpus++;
    }

    return 0;
}
