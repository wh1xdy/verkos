#ifndef STAM_VM_H
#define STAM_VM_H
#include <stdint.h>
/* Map one 4 KiB page so EL0 may reach it. Splits any larger block it meets. */
int map_user_page(uint64_t va, uint64_t page, int executable);
/* Is [va, va+len) mapped in the caller's own address space, and reachable by
 * EL0? Every pointer a system call is given must go through this before it is
 * dereferenced — the MMU protects EL0 from the kernel, not the kernel from a
 * pointer EL0 handed it. */
int  vm_user_ok(uint64_t va, uint64_t len, int write);
/* Length of a NUL-terminated user string, or -1 if it is unreachable or runs
 * past `max` without a terminator. */
long vm_user_strlen(uint64_t va, uint64_t max);
uint64_t vm_new_space(void);
uint64_t vm_copy_space(uint64_t src_l1_kva);
void     vm_free_space(uint64_t l1_kva);
#endif
