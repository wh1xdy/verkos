#ifndef STAM_VIRTIO_H
#define STAM_VIRTIO_H
#include <stdint.h>
#include "fdt.h"

/* A block device, if the machine has one. Returns 0 when a virtio-blk device
 * was found and set up; -1 otherwise, which is a fact about the machine and not
 * a failure — stam still boots from an initrd. */
int      virtio_blk_init(const fdt_t *f);
int      virtio_blk_present(void);
uint64_t virtio_blk_sectors(void);
/* One 512-byte sector into `out`. Synchronous and polled. */
int      virtio_blk_read(uint64_t sector, void *out);
int      virtio_blk_write(uint64_t sector, const void *in);
#endif
