/* stam — finding devices on a PCI bus.
 *
 * virtio over MMIO (D-079) exists because a hypervisor offers it, and a device
 * tree lists it. Neither is true of a machine booted by firmware: UEFI hands
 * over ACPI, vboot builds a tree out of what ACPI describes (D-072), and what
 * ACPI describes on this machine is a PCI host bridge — not the thirty-two
 * memory-mapped transports QEMU also happens to provide. So on the path that
 * matters, the one a real machine takes, the disk is behind PCI or it is not
 * reachable at all (D-080 said so plainly rather than drawing the line).
 *
 * ---- what enumeration is ----
 *
 * A bus number, a device number and a function number address a 4 KiB
 * configuration space. ECAM makes that arithmetic: base + (bus << 20) +
 * (dev << 15) + (fn << 12). The first two words are the vendor and device id,
 * and a vendor of 0xffff means nothing is there — which is the whole of "is
 * there a device here".
 *
 * ---- what firmware would have done ----
 *
 * A device's memory windows are not at fixed addresses. Each base address
 * register is written with all ones, read back to learn which bits are
 * writable — that is the size — and then written with an address the bridge
 * routes to it. Firmware normally does this before handing over. Nothing has,
 * on a kernel started with -kernel, so this does it: a device whose BARs are
 * zero is a device whose registers are at address zero, and every access to it
 * would be a fault with no obvious cause.
 *
 * The window to allocate from is the bridge's own, out of `ranges` in the tree.
 * Not a constant: the address a bridge routes is the bridge's to say, and a
 * kernel with a number in it is a kernel that works on one machine.
 */
#include "pci.h"
#include "fdt.h"
#include "mmu.h"
#include "console.h"
#include <stdint.h>

static uint64_t ecam;              /* the configuration space, in our window */
static uint64_t mem32_base, mem32_next, mem32_end;
static int      have_bridge;

static volatile uint8_t *cfg_at(unsigned bus, unsigned dev, unsigned fn) {
    return (volatile uint8_t *)(uintptr_t)(ecam + ((uint64_t)bus << 20)
                                                + ((uint64_t)dev << 15)
                                                + ((uint64_t)fn  << 12));
}

uint32_t pci_cfg_read32(const pci_dev_t *d, unsigned off) {
    return *(volatile uint32_t *)(cfg_at(d->bus, d->dev, d->fn) + off);
}
void pci_cfg_write32(const pci_dev_t *d, unsigned off, uint32_t v) {
    *(volatile uint32_t *)(cfg_at(d->bus, d->dev, d->fn) + off) = v;
}
uint16_t pci_cfg_read16(const pci_dev_t *d, unsigned off) {
    return *(volatile uint16_t *)(cfg_at(d->bus, d->dev, d->fn) + off);
}
uint8_t pci_cfg_read8(const pci_dev_t *d, unsigned off) {
    return *(volatile uint8_t *)(cfg_at(d->bus, d->dev, d->fn) + off);
}

/* The bridge's 32-bit memory window, from `ranges`.
 *
 * Each entry is a child address of three cells, a parent address of the
 * parent's #address-cells, and a size of two. The top byte of the first child
 * cell holds the space code in bits 24-25: 2 is 32-bit memory, which is the one
 * a BAR of the ordinary kind can live in. */
static int bridge_window(const fdt_t *f, int node) {
    uint32_t len = 0;
    const uint8_t *r = fdt_getprop(f, node, "ranges", &len);
    if (!r) return -1;
    unsigned per = (3 + f->addr_cells + 2) * 4;
    for (uint32_t at = 0; at + per <= len; at += per) {
        uint32_t hi = (uint32_t)fdt_cells(r + at, 1);
        unsigned space = (hi >> 24) & 3;
        uint64_t child  = fdt_cells(r + at + 4, 2);
        uint64_t size   = fdt_cells(r + at + 4 + f->addr_cells * 4, 2);
        if (space != 2) continue;                  /* not 32-bit memory */
        mem32_base = child;
        mem32_next = child;
        mem32_end  = child + size;
        return 0;
    }
    return -1;
}

int pci_init(const fdt_t *f) {
    int node = fdt_find_compatible(f, "pci-host-ecam-generic");
    if (node == FDT_NONE) return -1;

    uint32_t len = 0;
    const void *reg = fdt_getprop(f, node, "reg", &len);
    if (!reg || len < 8) return -1;
    ecam = phys_to_kva(fdt_cells(reg, f->addr_cells));

    /* A window is only needed to *place* a device's registers, and only a
     * machine where nothing has placed them needs that. Firmware assigns every
     * BAR before it hands over, so a tree that describes the bridge without its
     * ranges — which is what a bootloader can build out of ACPI's MCFG, since
     * the routing lives in the DSDT as AML — is still enough to enumerate and
     * to use what is already there. Assignment refuses in that case and says
     * so, rather than writing an address the bridge does not route: an access
     * that goes nowhere and reads back all ones, which looks exactly like a
     * device that is not there. */
    int windowed = (bridge_window(f, node) == 0);
    have_bridge = 1;
    if (windowed)
        kprintf("  pci       ECAM at 0x%llx, memory window 0x%llx..0x%llx\n",
                (unsigned long long)fdt_cells(reg, f->addr_cells),
                (unsigned long long)mem32_base, (unsigned long long)mem32_end);
    else
        kprintf("  pci       ECAM at 0x%llx, no window in the tree — "
                "using the regions firmware placed\n",
                (unsigned long long)fdt_cells(reg, f->addr_cells));
    return 0;
}

int pci_present(void) { return have_bridge; }

/* By vendor, by device id range, and by class.
 *
 * The class is what actually identifies the device. A transitional virtio
 * device's id says which *legacy* device it is, and the numbering is not the
 * modern one — the first version of this searched a range of ids and found
 * 1af4:1000, which is the network card QEMU adds by default and not the disk.
 * The base class byte says "mass storage" whichever generation it is. */
int pci_find(uint16_t vendor, uint16_t dev_lo, uint16_t dev_hi,
             uint8_t klass, pci_dev_t *out) {
    if (!have_bridge) return -1;
    /* Bus 0 only. The `virt` machine puts everything on it, and walking behind
     * a bridge means programming that bridge's secondary bus numbers — work
     * with nothing here to exercise it, which is work that would be wrong in a
     * way nothing would notice. */
    for (unsigned d = 0; d < 32; d++) {
        for (unsigned fn = 0; fn < 8; fn++) {
            pci_dev_t p = { 0, d, fn, 0, 0 };
            uint32_t id = pci_cfg_read32(&p, 0);
            if ((id & 0xffff) == 0xffff) { if (fn == 0) break; else continue; }
            uint16_t v = (uint16_t)(id & 0xffff);
            uint16_t did = (uint16_t)(id >> 16);
            uint8_t base_class = pci_cfg_read8(&p, 0x0b);
            if (v != vendor || did < dev_lo || did > dev_hi ||
                (klass != 0xff && base_class != klass)) {
                /* A device that is not multi-function has nothing at fn > 0,
                 * and probing it reads the function 0 registers again. */
                if (fn == 0 && !(pci_cfg_read8(&p, 0x0e) & 0x80)) break;
                continue;
            }
            *out = p;
            out->vendor = v;
            out->device = did;
            return 0;
        }
    }
    return -1;
}

/* Size a BAR, place it, and say where it went.
 *
 * Writing all ones and reading back gives a mask whose low clear bits are the
 * size: the device only implements the bits it needs. The command register's
 * memory-decode bit is cleared first, because a device that is decoding while
 * its base register is a size mask is a device claiming most of the address
 * space for the duration. */
uint64_t pci_bar_assign(const pci_dev_t *d, unsigned idx) {
    unsigned off = 0x10 + idx * 4;
    uint16_t cmd = pci_cfg_read16(d, 0x04);
    pci_cfg_write32(d, 0x04, (cmd & ~0x2u));

    uint32_t orig = pci_cfg_read32(d, off);
    pci_cfg_write32(d, off, 0xffffffffu);
    uint32_t mask = pci_cfg_read32(d, off);
    pci_cfg_write32(d, off, orig);
    if (!mask || mask == 0xffffffffu) { pci_cfg_write32(d, 0x04, cmd); return 0; }

    int is_io  = (orig & 1);
    int is_64  = !is_io && ((orig >> 1) & 3) == 2;
    if (is_io) { pci_cfg_write32(d, 0x04, cmd); return 0; }

    uint64_t size = (uint64_t)(~(mask & ~0xfu)) + 1;
    if (!size) { pci_cfg_write32(d, 0x04, cmd); return 0; }

    /* Already placed by firmware: leave it. Rewriting an address the firmware
     * chose is how two devices end up at one address. */
    uint64_t addr = orig & ~0xful;
    if (is_64) addr |= (uint64_t)pci_cfg_read32(d, off + 4) << 32;
    if (!addr) {
        if (!mem32_end) {
            pci_cfg_write32(d, 0x04, cmd);
            kprintf("  pci       a region nothing has placed, and no window to place it in\n");
            return 0;
        }
        mem32_next = (mem32_next + size - 1) & ~(size - 1);
        if (mem32_next + size > mem32_end) {
            pci_cfg_write32(d, 0x04, cmd);
            kprintf("  pci       the bridge's window is too small for a %llu byte region\n",
                    (unsigned long long)size);
            return 0;
        }
        addr = mem32_next;
        mem32_next += size;
        pci_cfg_write32(d, off, (uint32_t)addr);
        if (is_64) pci_cfg_write32(d, off + 4, (uint32_t)(addr >> 32));
    }

    /* Memory decoding and bus mastering: the first so the registers answer, the
     * second so the device may read the queue out of memory by itself. Without
     * bus mastering the ring is written, the device is kicked, and nothing ever
     * happens — which reads as a device that does not work. */
    pci_cfg_write32(d, 0x04, (uint32_t)(cmd | 0x2u | 0x4u));
    return addr;
}

/* Walk the capability list for one of a given id, returning its offset in
 * configuration space. */
unsigned pci_find_cap(const pci_dev_t *d, uint8_t id, unsigned after) {
    if (!(pci_cfg_read16(d, 0x06) & (1u << 4))) return 0;   /* no list at all */
    unsigned at = after ? pci_cfg_read8(d, after + 1) : (pci_cfg_read8(d, 0x34) & 0xfc);
    /* Bounded: a corrupt list that points at itself is a kernel that never
     * finishes booting, and 48 is more capabilities than anything has. */
    for (int guard = 0; at >= 0x40 && at < 0x100 && guard < 48; guard++) {
        if (pci_cfg_read8(d, at) == id) return at;
        at = pci_cfg_read8(d, at + 1) & 0xfc;
        if (!at) break;
    }
    return 0;
}
