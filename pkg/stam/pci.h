#ifndef STAM_PCI_H
#define STAM_PCI_H
#include <stdint.h>
#include "fdt.h"

typedef struct {
    unsigned bus, dev, fn;
    uint16_t vendor, device;
} pci_dev_t;

/* The ECAM host bridge, from the device tree. 0 if there is one. */
int      pci_init(const fdt_t *f);
int      pci_present(void);
/* The first device on bus 0 whose vendor matches and whose device id is in
 * [dev_lo, dev_hi]. 0 if found. */
/* klass is the base class byte, or 0xff for any. It is what identifies the
 * device: a transitional virtio card's id names the legacy device it stands in
 * for, and that numbering is not the modern one. */
int      pci_find(uint16_t vendor, uint16_t dev_lo, uint16_t dev_hi,
                  uint8_t klass, pci_dev_t *out);

uint32_t pci_cfg_read32(const pci_dev_t *d, unsigned off);
void     pci_cfg_write32(const pci_dev_t *d, unsigned off, uint32_t v);
uint16_t pci_cfg_read16(const pci_dev_t *d, unsigned off);
uint8_t  pci_cfg_read8(const pci_dev_t *d, unsigned off);

/* Size and place a base address register, enabling memory decoding and bus
 * mastering. Returns the physical address it now decodes, or 0. */
uint64_t pci_bar_assign(const pci_dev_t *d, unsigned idx);
/* The offset of a capability with this id, searching after `after` (0 to start
 * from the head). 0 if there is none. */
unsigned pci_find_cap(const pci_dev_t *d, uint8_t id, unsigned after);
#endif
