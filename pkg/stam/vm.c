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

static uint64_t *table_at(uint64_t desc) {
    return (uint64_t *)(uintptr_t)(desc & ADDR_MASK);
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
    *slot = (uint64_t)(uintptr_t)tbl | D_TABLE;
    return tbl;
}

static uint64_t *descend(uint64_t *tbl, unsigned idx, uint64_t child_size) {
    uint64_t d = tbl[idx];
    if ((d & 0x3ul) == D_TABLE) return table_at(d);
    return split(&tbl[idx], child_size);
}

int map_user_page(uint64_t va, uint64_t pa, int executable) {
    uint64_t *l1 = (uint64_t *)(uintptr_t)mmu_table();
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
