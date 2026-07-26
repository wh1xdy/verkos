/* stam — physical page allocation and the MMU.
 *
 * Two things happen here, and the order is not negotiable: we need somewhere
 * to put page tables before we can build page tables.
 *
 * The mapping is deliberately coarse — 1 GiB blocks, one table, no second
 * level. A finer mapping is only useful once there is something to protect
 * from something else, and there is not yet. What matters now is that memory
 * gets the right *attributes*: normal write-back for RAM so the caches can be
 * turned on, device-nGnRnE for everything else so a write to a UART register
 * is not sitting in a cache line when we expect the character to appear.
 * Getting that wrong produces a machine that works until it doesn't.
 */
#include "mmu.h"
#include "console.h"

/* Hidden, for the same reason the assembly symbols are: a linker-script symbol
 * referenced from position-independent code is otherwise fetched through the
 * GOT, and the GOT is not relocated. */
extern char _kernel_end[] __attribute__((visibility("hidden")));

/* ---- physical pages ---------------------------------------------------- */
/* A bump allocator. It cannot free, which is exactly right for page tables
 * that live as long as the kernel; a real allocator arrives with userspace. */
static uint64_t free_next, free_top;
static unsigned pages_taken;

/* `ram_base`/`ram_size` is either the whole of memory — in which case the
 * kernel image sits inside it and must be stepped over — or a region the
 * firmware has already told us is free, in which case it is authoritative and
 * stepping over the image would skip most of it. Distinguish by asking whether
 * the image is actually inside the region. */
void pmm_init(uint64_t ram_base, uint64_t ram_size, uint64_t avoid, uint64_t avoid_size) {
    uint64_t kend = (uint64_t)(uintptr_t)_kernel_end;
    uint64_t start = ram_base;
    if (kend > ram_base && kend < ram_base + ram_size) start = kend;
    /* Do not hand out the device tree we are still reading from. */
    if (avoid_size && avoid + avoid_size > start && avoid < ram_base + ram_size)
        if (avoid + avoid_size > start) start = avoid + avoid_size;
    free_next = (start + 0xfff) & ~0xffful;
    free_top  = ram_base + ram_size;
}

void *alloc_page(void) {
    if (free_next + PAGE_SIZE > free_top) return 0;
    uint64_t p = free_next;
    free_next += PAGE_SIZE;
    pages_taken++;
    for (unsigned i = 0; i < PAGE_SIZE / 8; i++) ((volatile uint64_t *)(uintptr_t)p)[i] = 0;
    return (void *)(uintptr_t)p;
}

/* Contiguous by construction — the allocator only ever bumps forward — but
 * taking them in one call says so, instead of leaving the next reader to work
 * out whether two alloc_page() calls may be assumed adjacent. */
void *alloc_pages(unsigned n) {
    if (!n) return 0;
    if (free_next + (uint64_t)n * PAGE_SIZE > free_top) return 0;
    uint64_t p = free_next;
    free_next += (uint64_t)n * PAGE_SIZE;
    pages_taken += n;
    for (uint64_t i = 0; i < (uint64_t)n * PAGE_SIZE / 8; i++)
        ((volatile uint64_t *)(uintptr_t)p)[i] = 0;
    return (void *)(uintptr_t)p;
}

unsigned pmm_pages_used(void) { return pages_taken; }
uint64_t pmm_free_bytes(void) { return free_top - free_next; }

/* ---- the MMU ----------------------------------------------------------- */
#define ATTR_DEVICE   0ul          /* MAIR index 0: Device-nGnRnE          */
#define ATTR_NORMAL   1ul          /* MAIR index 1: Normal write-back      */

#define DESC_BLOCK    0x1ul
#define DESC_AF       (1ul << 10)  /* access flag: unset means fault       */
#define DESC_SH_INNER (3ul << 8)
#define DESC_PXN      (1ul << 53)
#define DESC_UXN      (1ul << 54)

#define GIB           (1ul << 30)

static uint64_t *l1;

/* Push a range out of the data cache into memory. The line size is read from
 * CTR_EL0 rather than assumed: 64 bytes is right on most cores and wrong on
 * enough of them to matter, and a too-large stride skips lines silently. */
static void dcache_clean_to_memory(void *addr, uint64_t size) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t line = 4ul << ((ctr >> 16) & 0xf);          /* DminLine, in words */
    uint64_t p = (uint64_t)(uintptr_t)addr & ~(line - 1);
    uint64_t end = (uint64_t)(uintptr_t)addr + size;
    for (; p < end; p += line) __asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint64_t block(uint64_t pa, unsigned attr, int executable) {
    uint64_t d = (pa & ~(GIB - 1)) | DESC_AF | (attr << 2) | DESC_BLOCK;
    if (attr == ATTR_NORMAL) d |= DESC_SH_INNER;
    if (!executable) d |= DESC_PXN | DESC_UXN;
    return d;
}

int mmu_init(uint64_t ram_base, uint64_t ram_size) {
    l1 = alloc_page();
    if (!l1) return -1;

    /* A 39-bit address space: 512 entries of 1 GiB. Everything that is not
     * RAM is treated as device memory — which on a real machine it is. */
    for (unsigned i = 0; i < 512; i++) {
        uint64_t va = (uint64_t)i * GIB;
        int is_ram = (va + GIB > ram_base) && (va < ram_base + ram_size);
        l1[i] = block(va, is_ram ? ATTR_NORMAL : ATTR_DEVICE, is_ram);
    }

    /* attr0 = Device-nGnRnE, attr1 = Normal, write-back, read/write allocate */
    uint64_t mair = (0xfful << 8) | 0x00ul;

    /* Read what physical address size this CPU actually supports rather than
     * assuming: a machine with more RAM than we claim it can address faults
     * in ways that look like corrupt page tables. */
    uint64_t mmfr0;
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    uint64_t ips = mmfr0 & 0xf;
    if (ips > 5) ips = 5;

    uint64_t tcr = (25ul)          /* T0SZ = 25 → 39-bit user/kernel VA    */
                 | (1ul << 8)      /* IRGN0: write-back                    */
                 | (1ul << 10)     /* ORGN0: write-back                    */
                 | (3ul << 12)     /* SH0: inner shareable                 */
                 | (0ul << 14)     /* TG0: 4 KiB granule                   */
                 | (25ul << 16)    /* T1SZ, unused but kept sane           */
                 | (1ul << 23)     /* EPD1: no TTBR1 walks — no table yet  */
                 | (2ul << 30)     /* TG1: 4 KiB                           */
                 | (ips << 32);

    /* The table was written with the caches on, so it may still be sitting in
     * one. The page-table walker reads memory, not our cache — push it out
     * before anything can look for it. */
    dcache_clean_to_memory(l1, PAGE_SIZE);

    /* Switching TTBR0 and TCR while the MMU is enabled is not architecturally
     * permitted, and firmware hands us a machine with the MMU already on and
     * its own tables installed. So the MMU has to go off, be reconfigured, and
     * come back on.
     *
     * The whole sequence is one assembly block with no memory access in it,
     * and that is not tidiness. Between switching the data cache off and back
     * on, any load or store — including a function call's use of the stack —
     * would bypass a cache that still holds the newest copy of that memory,
     * and read something stale. A stack frame written before the switch and
     * read after it is exactly this bug, and it presents as a machine that
     * stops with no fault at all. */
    __asm__ volatile(
        "dsb  sy\n"
        "mrs  x0, sctlr_el1\n"
        "bic  x0, x0, #(1 << 0)\n"      /* MMU off        */
        "bic  x0, x0, #(1 << 2)\n"      /* data cache off */
        "msr  sctlr_el1, x0\n"
        "isb\n"
        "msr  mair_el1,  %0\n"
        "msr  tcr_el1,   %1\n"
        "msr  ttbr0_el1, %2\n"
        "isb\n"
        "tlbi vmalle1\n"
        "ic   iallu\n"
        "dsb  nsh\n"
        "isb\n"
        "mrs  x0, sctlr_el1\n"
        "orr  x0, x0, #(1 << 0)\n"      /* MMU on                */
        "orr  x0, x0, #(1 << 2)\n"      /* data cache on         */
        "orr  x0, x0, #(1 << 12)\n"     /* instruction cache on  */
        "msr  sctlr_el1, x0\n"
        "isb\n"
        :: "r"(mair), "r"(tcr), "r"((uint64_t)(uintptr_t)l1) : "x0", "memory");
    return 0;
}

uint64_t mmu_table(void) { return (uint64_t)(uintptr_t)l1; }
