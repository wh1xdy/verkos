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

extern char _kernel_end[];

/* ---- physical pages ---------------------------------------------------- */
/* A bump allocator. It cannot free, which is exactly right for page tables
 * that live as long as the kernel; a real allocator arrives with userspace. */
static uint64_t free_next, free_top;
static unsigned pages_taken;

void pmm_init(uint64_t ram_base, uint64_t ram_size, uint64_t avoid, uint64_t avoid_size) {
    uint64_t start = (uint64_t)(uintptr_t)_kernel_end;
    if (start < ram_base) start = ram_base;
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

    __asm__ volatile(
        "dsb  ishst\n"
        "msr  mair_el1, %0\n"
        "msr  tcr_el1,  %1\n"
        "msr  ttbr0_el1,%2\n"
        "isb\n"
        "tlbi vmalle1\n"
        "dsb  ish\n"
        "isb\n"
        :: "r"(mair), "r"(tcr), "r"((uint64_t)(uintptr_t)l1) : "memory");

    /* Turn it on, with the caches. Until this instruction retires every load
     * has gone to memory; after it the machine is perhaps thirty times faster. */
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ul << 0)    /* M: MMU on              */
          |  (1ul << 2)    /* C: data cache on       */
          |  (1ul << 12);  /* I: instruction cache on */
    __asm__ volatile("msr sctlr_el1, %0\n isb\n" :: "r"(sctlr) : "memory");
    return 0;
}

uint64_t mmu_table(void) { return (uint64_t)(uintptr_t)l1; }
