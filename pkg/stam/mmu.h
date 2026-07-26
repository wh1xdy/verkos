#ifndef STAM_MMU_H
#define STAM_MMU_H
#include <stdint.h>
#define PAGE_SIZE 4096ul

void     pmm_init(uint64_t ram_base, uint64_t ram_size, uint64_t avoid, uint64_t avoid_size);
void    *alloc_page(void);
unsigned pmm_pages_used(void);
uint64_t pmm_free_bytes(void);

int      mmu_init(uint64_t ram_base, uint64_t ram_size);
uint64_t mmu_table(void);
void     exceptions_install(void);
#endif
