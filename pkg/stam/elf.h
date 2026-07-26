#ifndef STAM_ELF_H
#define STAM_ELF_H
#include <stdint.h>
/* Load a static aarch64 ELF into the current address space. Returns 0 and the
 * entry point, or -1 having said on the console exactly which check failed. */
int elf_load(uint64_t image, uint64_t size, uint64_t *entry_out);
#endif
