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
#include "lock.h"

/* Hidden, for the same reason the assembly symbols are: a linker-script symbol
 * referenced from position-independent code is otherwise fetched through the
 * GOT, and the GOT is not relocated. */
extern char _kernel_end[] __attribute__((visibility("hidden")));

/* ---- physical pages ---------------------------------------------------- */
/* A bump allocator. It cannot free, which is exactly right for page tables
 * that live as long as the kernel; a real allocator arrives with userspace. */
static uint64_t free_next, free_top;
static uint64_t pmm_start;      /* where the allocator began, before any page */
static unsigned pages_taken;

/* Before the move, a kernel pointer *is* a physical address. After it, every
 * kernel pointer is the high alias. Rather than dust conversions through every
 * caller, the allocator hands back whichever is currently correct, and
 * kva_to_phys goes the other way for the one place that needs the real
 * address: a page-table entry, which the hardware reads. */
static int high_half;
void mmu_note_high(void) { high_half = 1; }
uint64_t kva_to_phys(const void *p) {
    uint64_t v = (uint64_t)(uintptr_t)p;
    return high_half ? v - KVA_OFFSET : v;
}
static void *usable(uint64_t phys) {
    return (void *)(uintptr_t)(high_half ? phys + KVA_OFFSET : phys);
}

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
    pmm_start = free_next;
}

/* Pages that have been given back, threaded through themselves: the first
 * eight bytes of a free page hold the address of the next one. It costs
 * nothing and needs no separate bookkeeping, which matters when the thing
 * doing the bookkeeping would itself need pages. */
static uint64_t free_list;
static unsigned pages_returned;

/* One byte per page of RAM: 128 KiB for a 512 MiB machine, taken from the
 * allocator itself once its bounds are known. A page nobody has shared has a
 * count of zero rather than one — the common case costs nothing to maintain,
 * and only vm_copy_space ever raises it. */
static unsigned char *refs;
static uint64_t refs_base, refs_pages;

void pmm_init_refs(void) {
    if (refs) return;
    /* From where the allocator began, not from where it has reached: a page
     * freed early and handed out again later must still have an entry, or a
     * shared page would report a count of one and copy-on-write would hand
     * both processes the same memory. */
    refs_base  = pmm_start;
    refs_pages = (free_top - pmm_start) / PAGE_SIZE;
    uint64_t need = (refs_pages + PAGE_SIZE - 1) / PAGE_SIZE;
    refs = alloc_pages((unsigned)need);
    if (!refs) { refs_pages = 0; return; }
    for (uint64_t i = 0; i < refs_pages; i++) refs[i] = 0;
}

static long refs_index(uint64_t pa) {
    if (!refs || pa < refs_base) return -1;
    uint64_t i = (pa - refs_base) / PAGE_SIZE;
    return i < refs_pages ? (long)i : -1;
}

void page_ref(uint64_t pa) {
    long i = refs_index(pa);
    if (i < 0) return;
    uint64_t f = irq_save();
    if (refs[i] < 255) refs[i]++;
    irq_restore(f);
}

/* Holders remaining *after* letting go — zero meaning the page is nobody's and
 * may be freed.
 *
 * The first version returned the count from before the decrement, which reads
 * naturally and is exactly wrong: two processes sharing a page gave 1, the
 * first to exit freed it, and the survivor carried on executing from memory
 * the allocator had already handed to somebody else. It surfaced as an
 * undefined instruction at a code address that had been running fine a moment
 * earlier — the quietest possible corruption, and only reachable once fork
 * stopped copying.
 *
 * refs[i] holds (holders - 1), so an untracked page is one holder and freeing
 * it is correct. */
int page_unref(uint64_t pa) {
    long i = refs_index(pa);
    if (i < 0) return 0;
    uint64_t f = irq_save();
    int left = 0;
    if (refs[i] > 0) { refs[i]--; left = refs[i] + 1; }
    irq_restore(f);
    return left;
}

int page_count(uint64_t pa) {
    long i = refs_index(pa);
    if (i < 0) return 1;
    return refs[i] ? refs[i] + 1 : 1;
}

void free_page(void *p) {
    if (!p) return;
    uint64_t f = irq_save();
    uint64_t *page = p;
    *page = free_list;
    free_list = (uint64_t)(uintptr_t)p;
    pages_returned++;
    irq_restore(f);
}

unsigned pmm_pages_free_listed(void) { return pages_returned; }

/* Taking a page off the list is a read, a follow and a store, and a timer tick
 * in the middle of it reschedules — after which a second thread reads the same
 * head and both walk away owning the same page. Two address spaces, one piece
 * of memory, and nothing reports anything until they disagree about what is in
 * it. The zeroing is inside the section too: the page is not this caller's
 * until the head has moved past it. */
void *alloc_page(void) {
    uint64_t f = irq_save();
    if (free_list) {
        uint64_t *page = (uint64_t *)(uintptr_t)free_list;
        free_list = *page;
        pages_returned--;
        irq_restore(f);
        for (unsigned i = 0; i < PAGE_SIZE / 8; i++) page[i] = 0;
        return page;
    }
    if (free_next + PAGE_SIZE > free_top) { irq_restore(f); return 0; }
    uint64_t p = free_next;
    free_next += PAGE_SIZE;
    pages_taken++;
    irq_restore(f);
    volatile uint64_t *z = usable(p);
    for (unsigned i = 0; i < PAGE_SIZE / 8; i++) z[i] = 0;
    return (void *)z;
}

/* Contiguous by construction — the allocator only ever bumps forward — but
 * taking them in one call says so, instead of leaving the next reader to work
 * out whether two alloc_page() calls may be assumed adjacent. */
void *alloc_pages(unsigned n) {
    if (!n) return 0;
    uint64_t f = irq_save();
    if (free_next + (uint64_t)n * PAGE_SIZE > free_top) { irq_restore(f); return 0; }
    uint64_t p = free_next;
    free_next += (uint64_t)n * PAGE_SIZE;
    pages_taken += n;
    irq_restore(f);
    volatile uint64_t *z = usable(p);
    for (uint64_t i = 0; i < (uint64_t)n * PAGE_SIZE / 8; i++) z[i] = 0;
    return (void *)z;
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

/* ---- moving the kernel to the high half -------------------------------- */
/* Why bother: while the kernel lives in the low half it shares TTBR0 with any
 * program, so every process would need its own copy of the kernel's mappings
 * and switching process would mean switching the kernel's map too. With the
 * kernel in TTBR1 the two halves are independent — the kernel is mapped once,
 * for everyone, and a process switch touches only TTBR0.
 *
 * This is possible at all because the kernel is built position-independent and
 * needs no relocations: the same instructions run correctly at 0x40080000 and
 * at 0xffffff8040080000. Nothing has to be rewritten, only re-pointed. */

extern void go_high(uint64_t offset);

static uint64_t *l1_high;      /* TTBR1: the kernel's own map              */
static uint64_t *l1_user;      /* TTBR0: empty, until a process fills it   */

void mmu_go_high(void) {
    l1_high = alloc_page();
    l1_user = alloc_page();
    if (!l1_high || !l1_user) { kprintf("  mmu       no memory for the high map\n"); return; }

    /* The high half is a straight window onto physical memory: the kernel sees
     * address P as P + KVA_OFFSET, always, with no exceptions to remember. */
    for (unsigned i = 0; i < 512; i++) l1_high[i] = l1[i];
    for (unsigned i = 0; i < 512; i++) l1_user[i] = 0;

    dcache_clean_to_memory(l1_high, PAGE_SIZE);
    dcache_clean_to_memory(l1_user, PAGE_SIZE);

    /* Install both halves and clear EPD1, which has been disabling TTBR1
     * walks since milestone 2 precisely because there was no table there. */
    uint64_t tcr;
    __asm__ volatile("mrs %0, tcr_el1" : "=r"(tcr));
    tcr &= ~(1ul << 23);
    __asm__ volatile(
        "msr  ttbr1_el1, %0\n"
        "msr  ttbr0_el1, %1\n"
        "msr  tcr_el1,   %2\n"
        "isb\n"
        "tlbi vmalle1\n"
        "dsb  ish\n"
        "isb\n"
        :: "r"(kva_to_phys(l1_high)),
           "r"(kva_to_phys(l1)),              /* keep the low map for one more moment */
           "r"(tcr) : "memory");

    /* From here the kernel exists at both addresses. Step across, abandon the
     * low one, and never come back. */
    l1      = (uint64_t *)(uintptr_t)phys_to_kva(l1);
    l1_high = (uint64_t *)(uintptr_t)phys_to_kva(l1_high);
    l1_user = (uint64_t *)(uintptr_t)phys_to_kva(l1_user);
    go_high(KVA_OFFSET);
}

/* Called from kmain_high, once the low half is no longer needed. Dropping it
 * means any pointer still holding a physical address faults immediately and
 * loudly, instead of working by accident until something moves. */
void mmu_drop_low_half(void) {
    __asm__ volatile(
        "msr  ttbr0_el1, %0\n"
        "isb\n"
        "tlbi vmalle1\n"
        "dsb  ish\n"
        "isb\n"
        :: "r"(kva_to_phys(l1_user)) : "memory");
}

uint64_t mmu_user_table(void)  { return kva_to_phys(l1_user); }
uint64_t mmu_user_table_kva(void) { return (uint64_t)(uintptr_t)l1_user; }

static uint64_t cur_user_kva;
uint64_t mmu_current_user_table_kva(void) { return cur_user_kva ? cur_user_kva : (uint64_t)(uintptr_t)l1_user; }

/* Change which table map_user_page writes into, without touching the hardware.
 * Copying an address space needs exactly this: build into the child's tables
 * while still running with the parent's installed. */
void mmu_set_current_user_table_kva(uint64_t l1_kva) { cur_user_kva = l1_kva; }

void mmu_switch_user(uint64_t l1_kva) {
    cur_user_kva = l1_kva;
    mmu_set_user_table(kva_to_phys((const void *)(uintptr_t)l1_kva));
}

void mmu_set_user_table(uint64_t phys) {
    __asm__ volatile(
        "msr  ttbr0_el1, %0\n"
        "isb\n"
        "tlbi vmalle1\n"
        "dsb  ish\n"
        "isb\n"
        :: "r"(phys) : "memory");
}
