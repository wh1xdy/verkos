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

#define D_AP_RO     (1ul << 7)      /* AP[2]: read-only for everyone         */
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
/* Install a descriptor that already exists, rather than building one from a
 * page and a permission. vm_copy_space needs this: the child's entry must be
 * the parent's entry, attributes and all, or a copy-on-write page could come
 * back executable when the original was not. */
int map_user_page_raw(uint64_t va, uint64_t desc) {
    uint64_t *l1 = (uint64_t *)(uintptr_t)mmu_current_user_table_kva();
    if (!l1) return -1;
    uint64_t *l2 = descend(l1, (unsigned)((va >> 30) & 511), 1ul << 21);
    if (!l2) return -1;
    uint64_t *l3 = descend(l2, (unsigned)((va >> 21) & 511), 4096);
    if (!l3) return -1;
    l3[(va >> 12) & 511] = desc;
    return 0;
}

/* A write to a shared page. If nobody else holds it, the sharing is over and
 * the permission simply comes back; otherwise this space gets a private copy.
 * Returns 0 if it handled the fault, -1 if this was not a copy-on-write page
 * at all — in which case the program really did do something it may not, and
 * telling those two apart is the whole job of this function. */
int vm_cow_fault(uint64_t va) {
    uint64_t *l1 = (uint64_t *)(uintptr_t)mmu_current_user_table_kva();
    if (!l1) return -1;
    uint64_t d1 = l1[(va >> 30) & 511];
    if ((d1 & 0x3ul) != D_TABLE) return -1;
    uint64_t *l2 = table_at(d1);
    uint64_t d2 = l2[(va >> 21) & 511];
    if ((d2 & 0x3ul) != D_TABLE) return -1;
    uint64_t *l3 = table_at(d2);
    uint64_t *slot = &l3[(va >> 12) & 511];
    uint64_t d = *slot;

    if ((d & 0x3ul) != D_PAGE) return -1;      /* nothing mapped: a real fault */
    if (!(d & D_AP_EL0)) return -1;            /* the kernel's, not theirs     */
    if (!(d & D_AP_RO)) return -1;             /* already writable: not us     */

    uint64_t pa = d & ADDR_MASK;
    if (page_count(pa) > 1) {
        unsigned char *fresh = alloc_page();
        if (!fresh) return -1;
        const unsigned char *from = (const unsigned char *)(uintptr_t)phys_to_kva(pa);
        for (unsigned b = 0; b < PAGE_SIZE; b++) fresh[b] = from[b];
        /* The copy was made with ordinary stores, which land in the data
         * cache. If the page holds instructions — and a copy-on-write page
         * may, because fork shares everything including the text — the
         * instruction cache has no idea. The CPU then fetches whatever was at
         * that physical address before, and executes it: an undefined
         * instruction at an address that looks perfectly valid.
         *
         * Clean to the point of unification and invalidate I-cache. This is
         * the same class of mistake as the MMU-off sequence in D-034 — a cache
         * holding the newest copy of something the other side of the machine
         * is reading. */
        uint64_t ctr;
        __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
        uint64_t dline = 4ul << ((ctr >> 16) & 0xf);
        uint64_t iline = 4ul << (ctr & 0xf);
        uint64_t base  = (uint64_t)(uintptr_t)fresh;
        for (uint64_t p = base; p < base + PAGE_SIZE; p += dline)
            __asm__ volatile("dc cvau, %0" :: "r"(p) : "memory");
        __asm__ volatile("dsb ish" ::: "memory");
        for (uint64_t p = base; p < base + PAGE_SIZE; p += iline)
            __asm__ volatile("ic ivau, %0" :: "r"(p) : "memory");
        __asm__ volatile("dsb ish\n isb" ::: "memory");

        page_unref(pa);
        *slot = (kva_to_phys(fresh) & ADDR_MASK) | (d & ~ADDR_MASK & ~D_AP_RO);
    } else {
        *slot = d & ~D_AP_RO;
    }
    __asm__ volatile("dsb ishst\n tlbi vaae1is, %0\n dsb ish\n isb"
                     :: "r"(va >> 12) : "memory");
    return 0;
}

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

/* --- is this address really the caller's? ---------------------------------
 *
 * Every pointer a system call is handed came from a program, and until now the
 * kernel simply dereferenced it. `write(1, (char *)kernel_address, n)` would
 * have printed kernel memory; a `read` into an unmapped address would have
 * faulted at EL1, where a fault is not a program being killed but the machine
 * stopping. The protection the MMU gives us is enforced on EL0's own accesses
 * and says nothing about what EL1 does on EL0's behalf.
 *
 * So the range is walked before it is touched. The walk is deliberately the
 * hardware's: the same tables, the same descriptors, so there is no second
 * opinion about what is mapped that could disagree with the first.
 *
 * AP=01 (D_AP_EL0) is the bit that says EL0 may reach this page at all, which
 * is exactly the question — a kernel page has AP=00 and fails here, whatever
 * address the program claimed. Bit 7 is AP[2], read-only; nothing sets it yet,
 * and it is checked anyway so that the day something does, this does not have
 * to be remembered.
 */
static uint64_t *walk_to_leaf(uint64_t va, uint64_t *level_size) {
    uint64_t *l1 = (uint64_t *)(uintptr_t)mmu_current_user_table_kva();
    if (!l1) return 0;

    uint64_t d1 = l1[(va >> 30) & 511];
    if ((d1 & 0x3ul) == D_BLOCK) { *level_size = 1ul << 30; return &l1[(va >> 30) & 511]; }
    if ((d1 & 0x3ul) != D_TABLE) return 0;

    uint64_t *l2 = table_at(d1);
    uint64_t d2 = l2[(va >> 21) & 511];
    if ((d2 & 0x3ul) == D_BLOCK) { *level_size = 1ul << 21; return &l2[(va >> 21) & 511]; }
    if ((d2 & 0x3ul) != D_TABLE) return 0;

    uint64_t *l3 = table_at(d2);
    uint64_t *slot = &l3[(va >> 12) & 511];
    if ((*slot & 0x3ul) != D_PAGE) return 0;
    *level_size = 4096;
    return slot;
}

/* The low half is 512 GiB — one L1 table of 1 GiB entries. An address above it
 * is not a user address at all, whatever the tables happen to say, and saying
 * so here means the walk never runs on a kernel pointer. */
#define USER_LIMIT (1ul << 39)

int vm_user_ok(uint64_t va, uint64_t len, int write) {
    if (len == 0) return 1;
    if (va + len < va) return 0;                  /* wrapped: no such range   */
    if (va >= USER_LIMIT || va + len > USER_LIMIT) return 0;

    for (uint64_t p = va & ~4095ul; p < va + len; ) {
        uint64_t size = 0;
        uint64_t *slot = walk_to_leaf(p, &size);
        if (!slot) return 0;
        uint64_t d = *slot;
        if (!(d & D_VALID)) return 0;
        if (!(d & D_AP_EL0)) return 0;            /* the kernel's, not theirs */
        if (write && (d & D_AP_RO)) return 0;
        p = (p & ~(size - 1)) + size;
    }
    return 1;
}

/* A string's length is not known until it has been read, and reading it is the
 * thing that has to be made safe. So it is walked one page at a time: check the
 * page, scan to the end of it, and only then look at the next. A string that
 * runs off the end of its last mapped page stops at the boundary rather than
 * faulting one byte past it. */
long vm_user_strlen(uint64_t va, uint64_t max) {
    if (va >= USER_LIMIT) return -1;
    for (uint64_t i = 0; i < max; ) {
        uint64_t page = (va + i) & ~4095ul;
        if (!vm_user_ok(page, 1, 0)) return -1;
        uint64_t end = page + 4096;
        for (uint64_t a = va + i; a < end && i < max; a++, i++)
            if (*(const char *)(uintptr_t)a == 0) return (long)i;
    }
    return -1;                                    /* no terminator in range   */
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
/* Share a page between two spaces instead of copying it, and take the write
 * permission away from both. Neither side can tell — until one writes, and the
 * fault handler gives it a private copy. */
static uint64_t cow_share(uint64_t *slot) {
    uint64_t d = *slot;
    *slot = d | D_AP_RO;
    page_ref(d & ADDR_MASK);
    return *slot;
}

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
                /* The parent's entry loses write permission too, so the next
                 * write by *either* side faults. Copying only for the child
                 * would let the parent scribble on memory the child believes
                 * is its own. */
                uint64_t shared = cow_share(&l3[i3]);
                if (map_user_page_raw(va, shared) != 0) {
                    mmu_set_current_user_table_kva(saved);
                    return 0;
                }
            }
        }
    }
    /* The parent is running with these tables installed, and half its pages
     * just became read-only. Nothing has invalidated them yet. */
    __asm__ volatile("dsb ishst\n tlbi vmalle1is\n dsb ish\n isb" ::: "memory");
    mmu_set_current_user_table_kva(saved);
    return dst_l1;
}

/* Give an address space back, page by page, tables included.
 *
 * Only the low half is walked: the kernel's mappings live in TTBR1 and are not
 * this process's to release. Nothing here is shared — vm_copy_space copies
 * outright rather than sharing — so every page reached is genuinely ours.
 * That would stop being true the day copy-on-write arrives, and this is the
 * function that would have to learn about reference counts.
 */
void vm_free_space(uint64_t l1_kva) {
    if (!l1_kva) return;
    uint64_t *l1 = (uint64_t *)(uintptr_t)l1_kva;
    for (unsigned i1 = 0; i1 < 512; i1++) {
        if ((l1[i1] & 0x3ul) != D_TABLE) continue;
        uint64_t *l2 = table_at(l1[i1]);
        for (unsigned i2 = 0; i2 < 512; i2++) {
            if ((l2[i2] & 0x3ul) != D_TABLE) continue;
            uint64_t *l3 = table_at(l2[i2]);
            for (unsigned i3 = 0; i3 < 512; i3++)
                if ((l3[i3] & 0x3ul) == D_PAGE) {
                    /* Shared pages belong to whoever is still holding them.
                     * Freeing one here would hand a live page back to the
                     * allocator while another process is reading it — the
                     * quietest possible corruption. */
                    uint64_t pa = l3[i3] & ADDR_MASK;
                    if (page_unref(pa) == 0)
                        free_page((void *)(uintptr_t)phys_to_kva(pa));
                }
            free_page(l3);
        }
        free_page(l2);
    }
    free_page(l1);
}
