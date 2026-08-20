#ifndef STAM_ELF_H
#define STAM_ELF_H
#include <stdint.h>
#include "savnote.h"
/* Load a static aarch64 ELF into the current address space. Returns 0 and the
 * entry point, or -1 having said on the console exactly which check failed.
 *
 * `manifest` may be null. When it is not, it comes back describing what the
 * image says it may ask of the kernel: present and populated for a program
 * savc built, and present = 0 for every other program, which is then not
 * restricted at all. */
int elf_load(uint64_t image, uint64_t size, uint64_t *entry_out, sav_manifest_t *manifest);
#endif
