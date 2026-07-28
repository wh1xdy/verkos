#ifndef STAM_MMU_H
#define STAM_MMU_H
#include <stdint.h>
#define PAGE_SIZE 4096ul

void     pmm_init(uint64_t ram_base, uint64_t ram_size, uint64_t avoid, uint64_t avoid_size);
void    *alloc_page(void);
void    *alloc_pages(unsigned n);
void     free_page(void *p);
/* How many pages came back off the free list with their marker gone. */
unsigned pmm_free_after_use(void);
unsigned pmm_pages_free_listed(void);
unsigned pmm_pages_used(void);
uint64_t pmm_free_bytes(void);

/* The distance between a physical address and where the kernel sees it once it
 * has moved to the high half. 39-bit address spaces: TTBR1 covers everything
 * from 0xffffff8000000000 upwards, TTBR0 everything below 0x8000000000. */
#define KVA_OFFSET 0xffffff8000000000ul
#define phys_to_kva(p) ((uint64_t)(p) + KVA_OFFSET)

int      mmu_init(uint64_t ram_base, uint64_t ram_size);
/* Build the kernel's high mapping, install it in TTBR1, and continue running
 * from it. Does not return: it calls kmain_high. */
void     mmu_go_high(void);
uint64_t mmu_user_table(void);
uint64_t mmu_identity_table(void);   /* the low map, for a starting processor */
/* A kernel address for a physical one the straight window cannot reach — which
 * is anything at or above 510 GiB. Returns 0 and writes *out; -1 if there is no
 * window left. Inside the straight window it is phys_to_kva and costs nothing. */
uint64_t mmu_map_device(uint64_t phys, uint64_t *out);
void     mmu_set_user_table(uint64_t table);
void     mmu_drop_low_half(void);
void     mmu_note_high(void);
uint64_t kva_to_phys(const void *p);
/* How many address spaces hold this physical page. One means the holder may
 * write to it; more means the next write has to make a private copy. The
 * counts live in a table sized at pmm_init, because a page's identity is its
 * frame number and nothing else is small enough to index by. */
void     page_ref(uint64_t pa);
int      page_unref(uint64_t pa);     /* returns the count after decrementing */
int      page_count(uint64_t pa);
uint64_t mmu_user_table_kva(void);
uint64_t mmu_current_user_table_kva(void);
void     mmu_switch_user(uint64_t l1_kva);
void     mmu_set_current_user_table_kva(uint64_t l1_kva);
uint64_t mmu_table(void);
/* Push a range out of this CPU's data cache into memory. A processor that is
 * still starting has its caches off and reads RAM directly, so anything
 * prepared for it has to be there and not here. */
void     mmu_clean(void *addr, uint64_t size);
void     exceptions_install(void);
#endif
