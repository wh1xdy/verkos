/* stam — reading FAT32.
 *
 * The filesystem is FAT32 and not something better, for one reason: it is the
 * one VerkOS already writes. `vmkfs` formats it, populates it and is what builds
 * every EFI system partition the project produces (D-065, D-068). A reader here
 * makes the two halves check each other — an image our formatter wrote, read by
 * our kernel, with neither able to agree with the other by sharing a bug,
 * because they were written for opposite jobs and neither shares a line.
 *
 * It is also the filesystem the firmware already reads, so a kernel that can
 * read it can read the partition it was booted from. That is the point of
 * starting here rather than with ext4.
 *
 * Read-only. Writing FAT means allocating clusters, updating two copies of the
 * table and keeping FSInfo honest, and vmkfs already does all three; doing it
 * again in the kernel would be a second implementation of the same on-disk
 * rules, which is the arrangement D-042 is about.
 */
#include "fat.h"
#include "virtio.h"
#include "console.h"

#include <stdint.h>

static uint32_t bytes_per_sec;
static uint32_t sec_per_clus;
static uint32_t first_data_sec;
static uint32_t fat_start;
static uint32_t root_clus;
static uint32_t total_clus;
static int      mounted;

/* One sector of scratch, and one of directory. Static because the kernel has no
 * heap and because every path through here is under one lock anyway. */
static uint8_t  secbuf[512];
static uint8_t  dirbuf[512];

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int fat_mount(void) {
    if (!virtio_blk_present()) return -1;
    if (virtio_blk_read(0, secbuf) != 0) return -1;

    /* The signature first. Without it every field below is a number read out of
     * whatever the disk happens to hold, and the mount would "succeed" on an
     * empty file. */
    if (secbuf[510] != 0x55 || secbuf[511] != 0xaa) {
        kprintf("  fat       sector 0 has no boot signature — not a filesystem\n");
        return -1;
    }

    bytes_per_sec = rd16(secbuf + 11);
    sec_per_clus  = secbuf[13];
    uint32_t reserved = rd16(secbuf + 14);
    uint32_t nfats    = secbuf[16];
    uint32_t fatsz    = rd32(secbuf + 36);         /* FAT32: the 16-bit field is 0 */
    root_clus         = rd32(secbuf + 44);
    uint32_t totsec   = rd32(secbuf + 32);

    /* Refused rather than worked around. A reader that copes with a geometry
     * nothing writes is a reader whose untested paths outnumber its tested
     * ones — and every one of these is a value vmkfs never produces, so a
     * mismatch means the image is not what it claims. */
    if (bytes_per_sec != 512 || !sec_per_clus || !fatsz || !nfats || root_clus < 2) {
        kprintf("  fat       not a FAT32 this reader understands "
                "(%u byte sectors, %u per cluster, %u FATs)\n",
                bytes_per_sec, sec_per_clus, nfats);
        return -1;
    }

    fat_start      = reserved;
    first_data_sec = reserved + nfats * fatsz;
    total_clus     = (totsec - first_data_sec) / sec_per_clus + 2;
    mounted = 1;

    /* The label, read back off the disk rather than assumed, because it is the
     * cheapest thing that proves the sector really came from the device. */
    char label[12];
    for (int i = 0; i < 11; i++) label[i] = (char)secbuf[71 + i];
    label[11] = 0;
    for (int i = 10; i >= 0 && label[i] == ' '; i--) label[i] = 0;

    kprintf("  fat       FAT32 \"%s\", %u sectors per cluster, %u clusters\n",
            label, sec_per_clus, total_clus);
    return 0;
}

int fat_mounted(void) { return mounted; }

/* Where a cluster's data begins. Cluster numbers start at 2 — 0 and 1 are the
 * media descriptor and the end marker, and have never been storage. */
static uint64_t clus_sector(uint32_t c) {
    return first_data_sec + (uint64_t)(c - 2) * sec_per_clus;
}

/* The next cluster in a chain, or 0 at the end. Every entry is read from the
 * disk each time: a cache here would be four lines and one more thing that can
 * disagree with the medium. */
static uint32_t fat_next(uint32_t c) {
    if (c < 2 || c >= total_clus) return 0;
    uint64_t off = (uint64_t)c * 4;
    if (virtio_blk_read(fat_start + off / 512, secbuf) != 0) return 0;
    uint32_t v = rd32(secbuf + (off % 512)) & 0x0fffffff;
    /* 0x0ffffff8 and above is the end of the chain; anything below 2 is a
     * corrupt entry and is treated as the end rather than followed. */
    if (v >= 0x0ffffff8 || v < 2) return 0;
    return v;
}

static char upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* A directory entry's 8.3 name, as a string: "HELLO   ELF" becomes "HELLO.ELF".
 * Compared case-insensitively, so a program may ask for /bin/hello. */
static void short_name(const uint8_t *e, char *out) {
    int n = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) out[n++] = (char)e[i];
    if (e[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) out[n++] = (char)e[i];
    }
    out[n] = 0;
}

/* One long-name entry holds thirteen UTF-16 characters in three runs. Only the
 * low byte is kept: this reader answers questions about names the build system
 * wrote, and those are ASCII. A name with a character outside it will not
 * match, which is a lookup that fails rather than one that matches the wrong
 * file. */
static void lfn_chars(const uint8_t *e, char *out13) {
    static const int off[13] = {1,3,5,7,9, 14,16,18,20,22,24, 28,30};
    for (int i = 0; i < 13; i++) {
        uint16_t u = rd16(e + off[i]);
        out13[i] = (u == 0 || u == 0xffff) ? 0 : (char)(u & 0xff);
    }
}

static int name_eq(const char *a, const char *b) {
    while (*a && *b) { if (upper(*a) != upper(*b)) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

/* Find `name` directly inside the directory starting at cluster `dir`. */
static int dir_find(uint32_t dir, const char *name, fat_file_t *out) {
    char lfn[261];
    int  lfn_len = 0;

    for (uint32_t c = dir; c; c = fat_next(c)) {
        for (uint32_t s = 0; s < sec_per_clus; s++) {
            if (virtio_blk_read(clus_sector(c) + s, dirbuf) != 0) return -1;
            for (int i = 0; i < 512; i += 32) {
                const uint8_t *e = dirbuf + i;
                if (e[0] == 0x00) return -1;         /* nothing further exists */
                if (e[0] == 0xe5) { lfn_len = 0; continue; }   /* deleted */
                if ((e[11] & 0x0f) == 0x0f) {
                    /* A long-name part. They are written last-first, and the
                     * sequence number says which; 13 characters each. */
                    int seq = (e[0] & 0x3f);
                    if (seq >= 1 && seq <= 20) {
                        char part[13];
                        lfn_chars(e, part);
                        int at = (seq - 1) * 13;
                        for (int k = 0; k < 13; k++) lfn[at + k] = part[k];
                        if (at + 13 > lfn_len) lfn_len = at + 13;
                        lfn[260] = 0;
                    }
                    continue;
                }
                if (e[11] & 0x08) { lfn_len = 0; continue; }   /* volume label */

                char sname[13];
                short_name(e, sname);
                char lname[261];
                int have_lfn = 0;
                if (lfn_len) {
                    int n = 0;
                    for (int k = 0; k < lfn_len && k < 260 && lfn[k]; k++) lname[n++] = lfn[k];
                    lname[n] = 0;
                    have_lfn = n > 0;
                }
                lfn_len = 0;

                if (name_eq(sname, name) || (have_lfn && name_eq(lname, name))) {
                    out->cluster = ((uint32_t)rd16(e + 20) << 16) | rd16(e + 26);
                    out->size    = rd32(e + 28);
                    out->is_dir  = (e[11] & 0x10) ? 1 : 0;
                    /* A zero-length file has no first cluster, and a directory
                     * entry naming cluster 0 is not a chain to follow. */
                    if (out->cluster < 2) out->cluster = 0;
                    return 0;
                }
            }
        }
    }
    return -1;
}

int fat_open(const char *path, fat_file_t *out) {
    if (!mounted) return -1;
    while (*path == '/') path++;

    fat_file_t cur = { .cluster = root_clus, .size = 0, .is_dir = 1 };
    if (!*path) { *out = cur; return 0; }          /* the root itself */

    char part[64];
    while (*path) {
        int n = 0;
        while (*path && *path != '/' && n < 63) part[n++] = *path++;
        part[n] = 0;
        while (*path == '/') path++;
        if (!n) continue;
        if (!cur.is_dir) return -1;                /* a component under a file */
        if (dir_find(cur.cluster ? cur.cluster : root_clus, part, &cur) != 0) return -1;
    }
    *out = cur;
    return 0;
}

long fat_read(const fat_file_t *f, uint64_t off, void *buf, uint64_t len) {
    if (!mounted || f->is_dir) return -1;
    if (off >= f->size) return 0;
    if (off + len > f->size) len = f->size - off;

    uint8_t *dst = buf;
    uint64_t done = 0;
    uint64_t cluster_bytes = (uint64_t)sec_per_clus * 512;

    /* Walk the chain to where the read starts. Following it from the beginning
     * every time makes a sequential read quadratic in the number of clusters,
     * which for a 67 KB program is fourteen steps and for a large file would
     * not be acceptable — the honest place to fix that is a cached position in
     * fat_file_t, and it is not needed yet. */
    uint32_t c = f->cluster;
    uint64_t skip = off / cluster_bytes;
    while (skip-- && c) c = fat_next(c);
    uint64_t within = off % cluster_bytes;

    while (done < len && c) {
        uint32_t s = (uint32_t)(within / 512);
        uint32_t in_sec = (uint32_t)(within % 512);
        if (virtio_blk_read(clus_sector(c) + s, secbuf) != 0) return done ? (long)done : -1;
        uint64_t take = 512 - in_sec;
        if (take > len - done) take = len - done;
        for (uint64_t i = 0; i < take; i++) dst[done + i] = secbuf[in_sec + i];
        done   += take;
        within += take;
        if (within >= cluster_bytes) { within = 0; c = fat_next(c); }
    }
    return (long)done;
}
