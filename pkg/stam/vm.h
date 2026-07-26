#ifndef STAM_VM_H
#define STAM_VM_H
#include <stdint.h>
/* Map one 4 KiB page so EL0 may reach it. Splits any larger block it meets. */
int map_user_page(uint64_t va, uint64_t page, int executable);
uint64_t vm_new_space(void);
uint64_t vm_copy_space(uint64_t src_l1_kva);
#endif
