#ifndef STAM_CPIO_H
#define STAM_CPIO_H
#include <stdint.h>
/* Find a file in a newc cpio archive. Returns 0 and where the data is. */
int  cpio_find(uint64_t image, uint64_t size, const char *want,
               uint64_t *addr_out, uint64_t *size_out);
void cpio_list(uint64_t image, uint64_t size);
#endif
