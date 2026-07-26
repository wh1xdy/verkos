/* stam — fine-grained mapping, so userspace can exist.
 *
 * Milestone 2 mapped the machine in 1 GiB blocks, which was right when the only
 * question was "what attributes does this memory have". A user program needs
 * pages of its own that the kernel can reach but that carry different
 * permissions from everything around them, and a gigabyte is not a useful unit
 * for that.
 *
 * So this walks down to 4 KiB pages, creating the intermediate tables as it
 * goes — and, where it meets a block that already covers the address, splits it
 * into a table of smaller blocks that describe exactly the same memory. That
 * split is the part worth being careful about: the first gigabyte contains the
 * UART, so replacing that block wholesale instead of splitting it would
 * silently take away the console, which is also the only means of finding out
 * what went wrong.
 *
 * There is one address space, not two. The kernel is mapped with AP=00, which
 * means EL1 may touch it and EL0 may not; user pages are mapped AP=01, which
 * means both may. The hardware enforces the boundary on every access. A split
 * TTBR0/TTBR1 layout is the eventual answer — it makes the kernel's half of the
 * map shared between processes instead of copied — but it is not what makes the
 * protection work, and doing it now would be a change of address for every
 * kernel symbol at the same time as introducing userspace.
 */
#include "vm.h"
#include "mmu.h"
#include "console.h"

/* pa is a physical address throughout: it goes into a descriptor. */

#define D_TABLE     0x3ul
#define D_BLOCK     0x1ul
#define D_PAGE      0x3ul
#define D_VALID     0x1ul
#define D_AF        (1ul << 10)
#define D_SH_INNER  (3ul << 8)
#define D_AP_EL0    (1ul << 6)      /* AP=01: EL1 and EL0 may both access   */
#define D_PXN       (1ul << 53)     /* EL1 may not execute this             */
#define D_UXN       (1ul << 54)     /* EL0 may not execute this             */
#define ATTR_NORMAL 1ul

#define ADDR_MASK   0x0000fffffffff000ul

/* A descriptor holds a physical address; the kernel has to look at the table
 * through its own mapping of that memory. */
static uint64_t *table_at(uint64_t desc) {
    return (uint64_t *)(uintptr_t)phys_to_kva(desc & ADDR_MASK);
}

/* Replace a block descriptor with a table of 512 descriptors covering exactly
 * the same memory with the same attributes, one level finer. */
static uint64_t *split(uint64_t *slot, uint64_t child_size) {
    uint64_t old = *slot;
    uint64_t *tbl = alloc_page();
    if (!tbl) return 0;
    if (old & D_VALID) {
        uint64_t base  = old & ~(child_size * 512 - 1) & ADDR_MASK;
        uint64_t attrs = old & ~ADDR_MASK & ~0x3ul;
        for (unsigned i = 0; i < 512; i++)
            tbl[i] = (base + (uint64_t)i * child_size) | attrs |
                     (child_size == 4096 ? D_PAGE : D_BLOCK);
    } else {
        for (unsigned i = 0; i < 512; i++) tbl[i] = 0;
    }
    *slot = kva_to_phys(tbl) | D_TABLE;
    return tbl;
}

static uint64_t *descend(uint64_t *tbl, unsigned idx, uint64_t child_size) {
    uint64_t d = tbl[idx];
    if ((d & 0x3ul) == D_TABLE) return table_at(d);
    return split(&tbl[idx], child_size);
}

/* `page` is a kernel pointer, as returned by alloc_page; the descriptor needs
 * the physical address behind it. Converting here rather than at every call
 * site means one place to be wrong. */
int map_user_page(uint64_t va, uint64_t page, int executable) {
    uint64_t pa = kva_to_phys((const void *)(uintptr_t)page);
    /* Into the process's own table, not the kernel's. Before the kernel moves
     * to the high half these are the same table; afterwards they are not, and
     * mapping a user page into the kernel's would put it somewhere no program
     * can reach. */
    uint64_t *l1 = (uint64_t *)(uintptr_t)mmu_current_user_table_kva();
    if (!l1) return -1;

    uint64_t *l2 = descend(l1, (unsigned)((va >> 30) & 511), 1ul << 21);
    if (!l2) return -1;
    uint64_t *l3 = descend(l2, (unsigned)((va >> 21) & 511), 4096);
    if (!l3) return -1;

    uint64_t d = (pa & ADDR_MASK) | D_PAGE | D_AF | D_SH_INNER
               | (ATTR_NORMAL << 2) | D_AP_EL0 | D_PXN;
    /* The kernel never executes user code — PXN above, unconditionally. Whether
     * *userspace* may execute it is the caller's decision. */
    if (!executable) d |= D_UXN;
    l3[(va >> 12) & 511] = d;

    /* A stale entry for this address may be cached; the block we split is very
     * likely one the CPU has already walked. */
    __asm__ volatile("dsb ishst\n"
                     "tlbi vaae1is, %0\n"
                     "dsb ish\n"
                     "isb\n"
                     :: "r"(va >> 12) : "memory");
    return 0;
}

/* A fresh, empty low half. Nothing is shared with any other process — not even
 * an intermediate table — so two programs may put different memory at the same
 * address and never see each other's. */
uint64_t vm_new_space(void) {
    uint64_t *l1 = alloc_page();
    if (!l1) return 0;
    for (unsigned i = 0; i < 512; i++) l1[i] = 0;
    return (uint64_t)(uintptr_t)l1;
}

/* Duplicate an address space, page by page.
 *
 * Copy-on-write is the thing to do eventually, and it is not what to do first:
 * it needs write faults, reference counts on pages, and a fault handler that
 * can distinguish "not allowed" from "not yet". Copying outright is obviously
 * correct, and being able to see fork work is worth more right now than being
 * able to see it work quickly.
 *
 * The parent's pages are read through the kernel's window onto physical
 * memory, not through the user addresses, so this does not depend on which
 * address space is currently installed.
 */
uint64_t vm_copy_space(uint64_t src_l1_kva) {
    uint64_t *src = (uint64_t *)(uintptr_t)src_l1_kva;
    uint64_t dst_l1 = vm_new_space();
    if (!dst_l1) return 0;

    uint64_t saved = mmu_current_user_table_kva();
    mmu_set_current_user_table_kva(dst_l1);      /* map_user_page targets it */

    for (unsigned i1 = 0; i1 < 512; i1++) {
        if ((src[i1] & 0x3ul) != D_TABLE) continue;
        uint64_t *l2 = table_at(src[i1]);
        for (unsigned i2 = 0; i2 < 512; i2++) {
            if ((l2[i2] & 0x3ul) != D_TABLE) continue;
            uint64_t *l3 = table_at(l2[i2]);
            for (unsigned i3 = 0; i3 < 512; i3++) {
                uint64_t d = l3[i3];
                if ((d & 0x3ul) != D_PAGE) continue;
                uint64_t va = ((uint64_t)i1 << 30) | ((uint64_t)i2 << 21) | ((uint64_t)i3 << 12);
                unsigned char *from = (unsigned char *)(uintptr_t)phys_to_kva(d & ADDR_MASK);
                unsigned char *to = alloc_page();
                if (!to) { mmu_set_current_user_table_kva(saved); return 0; }
                for (unsigned b = 0; b < PAGE_SIZE; b++) to[b] = from[b];
                int exec = !(d & D_UXN);
                if (map_user_page(va, (uint64_t)(uintptr_t)to, exec)) {
                    mmu_set_current_user_table_kva(saved);
                    return 0;
                }
            }
        }
    }
    mmu_set_current_user_table_kva(saved);
    return dst_l1;
}
