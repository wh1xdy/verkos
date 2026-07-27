/* stam — finding the partition the filesystem is in.
 *
 * The disk `make boot` produces is built end to end by this project: `vpart`
 * writes the table, `vmkfs` formats and fills the EFI system partition, `vboot`
 * is the loader in it, and `stam` is what the loader starts. Until now the
 * kernel was the one link that could not read the medium it came off — it could
 * mount a whole device formatted as one filesystem, which is not what a bootable
 * disk looks like.
 *
 * So: read the table, find the EFI system partition by its type, and give the
 * FAT reader the sector it starts at. Every read is then offset by that, and
 * nothing else in the reader knows a partition exists.
 *
 * ---- what is checked, and what is not ----
 *
 * The signature and the header size, because those say the structure is a GPT
 * at all. Not the CRCs: `vpart` computes both and `sfdisk` verifies them in
 * `make test` (D-066), so a header that reaches this code has already been
 * checked by something that is not us. Recomputing them here would be a third
 * implementation of the same polynomial, and a disagreement between three
 * copies is harder to place than one between two.
 *
 * The protective MBR is not consulted. It exists to stop tools that only know
 * MBR from seeing free space, and it says nothing this needs.
 */
#include "gpt.h"
#include "virtio.h"
#include "console.h"
#include <stdint.h>

static uint8_t sec[512];

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* C12A7328-F81F-11D2-BA4B-00A0C93EC93B, as it is written on the disk.
 *
 * The first three fields are little-endian and the last two are not, which is
 * a rule with no reason behind it and one that a reader who types the GUID out
 * left to right will get wrong. vpart writes the same bytes; this is the other
 * end of D-066's mixed-endian note. */
static const uint8_t esp_type[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
};

int gpt_find_esp(uint64_t *first_lba, uint64_t *sectors) {
    if (!virtio_blk_present()) return -1;
    if (virtio_blk_read(1, sec) != 0) return -1;

    /* "EFI PART". A disk formatted as one big filesystem has something else
     * here, and saying so is how "no partition table" is told apart from "a
     * table this code could not read". */
    static const char sig[8] = { 'E','F','I',' ','P','A','R','T' };
    for (int i = 0; i < 8; i++) if (sec[i] != (uint8_t)sig[i]) return -1;

    uint64_t entries_lba = rd64(sec + 72);
    uint32_t n_entries   = rd32(sec + 80);
    uint32_t entry_size  = rd32(sec + 84);

    /* 128 is the only entry size anything writes, and a different one means
     * every offset below lands somewhere else. Refused rather than assumed. */
    if (entry_size != 128 || !n_entries || n_entries > 256) {
        kprintf("  gpt       header says %u entries of %u bytes — not a layout this reads\n",
                n_entries, entry_size);
        return -1;
    }

    for (uint32_t i = 0; i < n_entries; i++) {
        uint64_t lba = entries_lba + (uint64_t)i * entry_size / 512;
        uint32_t off = (uint32_t)((uint64_t)i * entry_size % 512);
        if (virtio_blk_read(lba, sec) != 0) return -1;
        const uint8_t *e = sec + off;

        int match = 1;
        for (int k = 0; k < 16; k++) if (e[k] != esp_type[k]) { match = 0; break; }
        if (!match) continue;

        uint64_t start = rd64(e + 32);
        uint64_t end   = rd64(e + 40);          /* inclusive */
        if (end < start) continue;
        *first_lba = start;
        *sectors   = end - start + 1;
        kprintf("  gpt       EFI system partition %u: %llu sectors at LBA %llu\n",
                i + 1, (unsigned long long)*sectors, (unsigned long long)start);
        return 0;
    }
    kprintf("  gpt       a partition table with no EFI system partition in it\n");
    return -1;
}
