#ifndef STAM_MMU_H
#define STAM_MMU_H
#include <stdint.h>
#define PAGE_SIZE 4096ul

void     pmm_init(uint64_t ram_base, uint64_t ram_size, uint64_t avoid, uint64_t avoid_size);
void    *alloc_page(void);
void    *alloc_pages(unsigned n);
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
void     mmu_set_user_table(uint64_t table);
void     mmu_drop_low_half(void);
void     mmu_note_high(void);
uint64_t kva_to_phys(const void *p);
uint64_t mmu_user_table_kva(void);
uint64_t mmu_table(void);
void     exceptions_install(void);
#endif
