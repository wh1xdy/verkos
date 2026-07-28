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
 *
 * ---- and then it was not read-only ----
 *
 * 2026-07-28: it writes. The paragraph above is left as written because the
 * objection in it is real and was answered rather than withdrawn — what makes
 * the second implementation acceptable is that the two are checked against each
 * other by a third party. `make test` boots the kernel, has it write a file,
 * shuts the machine down and reads the image back with `mtools` and `fsck.fat`,
 * neither of which is ours. A disagreement between vmkfs and this code is then
 * a test failure rather than something a user finds later (D-082).
 *
 * Long names are still vmkfs's job. A name that does not fit 8.3 is refused
 * here, by name, rather than written with a checksum this code would be the
 * only thing computing.
 */
#include "fat.h"
#include "virtio.h"
#include "gpt.h"
#include "console.h"

#include <stdint.h>

static uint32_t bytes_per_sec;
static uint32_t sec_per_clus;
static uint32_t first_data_sec;
static uint32_t fat_start;
static uint32_t root_clus;
static uint32_t fsinfo_sec;
static uint32_t total_clus;
static uint32_t nfats, fatsz;      /* for writing: every copy must be updated */
static int      mounted;
/* Where the filesystem starts on the device. Zero when the whole disk is one
 * filesystem, and the partition's first LBA otherwise: every read below adds
 * it, and nothing else in this file knows a partition table exists. */
static uint64_t part_lba;

/* One sector of scratch, and one of directory. Static because the kernel has no
 * heap and because every path through here is under one lock anyway. */
static uint8_t  secbuf[512];
static uint8_t  dirbuf[512];

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Every sector this reader asks for, relative to the filesystem. */
static int rd_sec(uint64_t s, void *out) { return virtio_blk_read(part_lba + s, out); }
static int wr_sec(uint64_t s, const void *in) { return virtio_blk_write(part_lba + s, in); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int fat_mount(void) {
    if (!virtio_blk_present()) return -1;

    /* A partitioned disk first. `make boot` produces one, with the filesystem
     * inside the EFI system partition rather than at sector 0, so a reader that
     * only understood a whole-device filesystem could not read the medium the
     * kernel was booted from — which is the one that matters. An unpartitioned
     * image still works: gpt_find_esp says so and this falls back. */
    uint64_t start = 0, count = 0;
    part_lba = (gpt_find_esp(&start, &count) == 0) ? start : 0;

    if (rd_sec(0, secbuf) != 0) return -1;

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
    nfats             = secbuf[16];
    fatsz             = rd32(secbuf + 36);         /* FAT32: the 16-bit field is 0 */
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

    fsinfo_sec     = rd16(secbuf + 48);
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

static uint64_t clus_sector(uint32_t c);

/* One FAT entry, written into every copy. Not one: a filesystem with two FATs
 * that disagree is one that `fsck.fat` repairs by picking, and which of the two
 * it picks is not ours to decide. */
static int fat_set(uint32_t c, uint32_t v) {
    if (c < 2 || c >= total_clus) return -1;
    uint64_t off = (uint64_t)c * 4;
    for (uint32_t k = 0; k < nfats; k++) {
        uint64_t s = fat_start + (uint64_t)k * fatsz + off / 512;
        if (rd_sec(s, secbuf) != 0) return -1;
        uint32_t old = rd32(secbuf + (off % 512));
        /* The top four bits are reserved and must be preserved, which is a rule
         * that costs nothing to follow and produces a filesystem other readers
         * complain about when it is not. */
        wr32(secbuf + (off % 512), (old & 0xf0000000u) | (v & 0x0fffffffu));
        if (wr_sec(s, secbuf) != 0) return -1;
    }
    return 0;
}

/* A free cluster, marked as the end of a chain so a second call cannot return
 * the same one. Linear from the start every time: FSInfo carries a hint and
 * trusting a hint that may be stale is how two files come to share a cluster,
 * so it is written and not read. */
static uint32_t clus_alloc(void) {
    /* A sector holds 128 entries. Reading it once per entry was reading it a
     * hundred and twenty-eight times for the same 512 bytes, through a driver
     * that polls — which is not a wrong answer, it is the same answer paid for
     * over and over, and it made a boot that does a few file creations take
     * minutes rather than seconds. */
    uint64_t cached = (uint64_t)-1;
    for (uint32_t c = 2; c < total_clus; c++) {
        uint64_t off = (uint64_t)c * 4;
        uint64_t sec = fat_start + off / 512;
        if (sec != cached) {
            if (rd_sec(sec, secbuf) != 0) return 0;
            cached = sec;
        }
        if ((rd32(secbuf + (off % 512)) & 0x0fffffffu) != 0) continue;
        if (fat_set(c, 0x0ffffff8u) != 0) return 0;
        /* A fresh cluster still holds whatever was there. For a directory that
         * matters — a stale entry is a file that appears out of nothing — and
         * for a file it is someone else's data handed to a program. */
        for (int i = 0; i < 512; i++) secbuf[i] = 0;
        for (uint32_t s = 0; s < sec_per_clus; s++)
            if (wr_sec(clus_sector(c) + s, secbuf) != 0) return 0;
        return c;
    }
    return 0;
}

/* FSInfo's free count, set to "not known".
 *
 * 0xFFFFFFFF is what the specification says to write when the number is not
 * being maintained, and every reader handles it — it is a hint for a formatter
 * looking for space, not something the layout depends on. The alternative is to
 * count the whole table, which on a 48 MiB volume is about 750 sector reads per
 * file closed, in a driver that polls. A number that costs that much to keep
 * true is a number that will be kept true only sometimes, and a free count that
 * is right on Tuesdays is worse than one that says it does not know.
 *
 * The next-free hint gets the same answer, and clus_alloc scans from the start
 * rather than trusting one: a stale hint is how two files come to share a
 * cluster.
 */
static void fsinfo_update(void);
void fat_sync(void) { if (mounted) fsinfo_update(); }

static void fsinfo_update(void) {
    if (!fsinfo_sec) return;
    if (rd_sec(fsinfo_sec, dirbuf) != 0) return;
    if (rd32(dirbuf) != 0x41615252u) return;          /* not an FSInfo sector */
    wr32(dirbuf + 488, 0xffffffffu);
    wr32(dirbuf + 492, 0xffffffffu);
    wr_sec(fsinfo_sec, dirbuf);
}

static uint64_t clus_sector(uint32_t c) {
    return first_data_sec + (uint64_t)(c - 2) * sec_per_clus;
}

/* The next cluster in a chain, or 0 at the end. Every entry is read from the
 * disk each time: a cache here would be four lines and one more thing that can
 * disagree with the medium. */
static uint32_t fat_next(uint32_t c) {
    if (c < 2 || c >= total_clus) return 0;
    uint64_t off = (uint64_t)c * 4;
    if (rd_sec(fat_start + off / 512, secbuf) != 0) return 0;
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
            if (rd_sec(clus_sector(c) + s, dirbuf) != 0) return -1;
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
                    out->dir_sec = clus_sector(c) + s;
                    out->dir_off = (uint32_t)i;
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

    fat_file_t cur = { .cluster = root_clus, .size = 0, .is_dir = 1,
                       .dir_sec = 0, .dir_off = 0 };
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
        if (rd_sec(clus_sector(c) + s, secbuf) != 0) return done ? (long)done : -1;
        uint64_t take = 512 - in_sec;
        if (take > len - done) take = len - done;
        for (uint64_t i = 0; i < take; i++) dst[done + i] = secbuf[in_sec + i];
        done   += take;
        within += take;
        if (within >= cluster_bytes) { within = 0; c = fat_next(c); }
    }
    return (long)done;
}

/* ---- writing ------------------------------------------------------------ */

/* Put the length and the first cluster back where the directory says they are.
 * Both, because a file that grew from nothing gained a chain it did not have,
 * and writing only the size leaves a directory entry describing zero clusters
 * of some number of bytes. */
static int dir_writeback(const fat_file_t *f) {
    if (!f->dir_sec) return -1;
    if (rd_sec(f->dir_sec, dirbuf) != 0) return -1;
    uint8_t *e = dirbuf + f->dir_off;
    wr32(e + 28, f->size);
    wr16(e + 20, (uint16_t)(f->cluster >> 16));
    wr16(e + 26, (uint16_t)(f->cluster & 0xffff));
    return wr_sec(f->dir_sec, dirbuf);
}

long fat_write(fat_file_t *f, uint64_t off, const void *buf, uint64_t len) {
    if (!mounted || f->is_dir || !f->dir_sec) return -1;
    if (!len) return 0;

    const uint8_t *src = buf;
    uint64_t cluster_bytes = (uint64_t)sec_per_clus * 512;

    /* A write past the end leaves a hole, and FAT has no way to say "these
     * bytes were never written" — the clusters exist and hold whatever was in
     * them. Refusing is the honest answer for a reader that cannot express it;
     * appending and seeking to the end both still work. */
    if (off > f->size) return -1;

    if (!f->cluster) {
        f->cluster = clus_alloc();
        if (!f->cluster) return -1;
    }

    /* Walk to the cluster the write starts in, extending if the file is being
     * appended to exactly at a boundary. */
    uint32_t c = f->cluster;
    uint64_t skip = off / cluster_bytes;
    while (skip--) {
        uint32_t n = fat_next(c);
        if (!n) {
            n = clus_alloc();
            if (!n || fat_set(c, n) != 0) return -1;
        }
        c = n;
    }
    uint64_t within = off % cluster_bytes;
    uint64_t done = 0;

    while (done < len) {
        uint32_t s = (uint32_t)(within / 512);
        uint32_t in_sec = (uint32_t)(within % 512);
        uint64_t take = 512 - in_sec;
        if (take > len - done) take = len - done;
        /* Read before write when only part of the sector changes: a sector is
         * the smallest thing the device takes, and writing a partly filled
         * buffer erases the rest of it. */
        if (take != 512 && rd_sec(clus_sector(c) + s, secbuf) != 0) return done ? (long)done : -1;
        for (uint64_t i = 0; i < take; i++) secbuf[in_sec + i] = src[done + i];
        if (wr_sec(clus_sector(c) + s, secbuf) != 0) return done ? (long)done : -1;
        done   += take;
        within += take;
        if (done < len && within >= cluster_bytes) {
            uint32_t n = fat_next(c);
            if (!n) {
                n = clus_alloc();
                if (!n || fat_set(c, n) != 0) break;
            }
            c = n;
            within = 0;
        }
    }

    if (off + done > f->size) f->size = (uint32_t)(off + done);
    if (dir_writeback(f) != 0) return done ? (long)done : -1;
    return (long)done;
}

int fat_truncate(fat_file_t *f) {
    if (!mounted || f->is_dir || !f->dir_sec) return -1;
    /* Every cluster back on the free list, following the chain before breaking
     * it: a chain freed from the head has no second link to follow, and the
     * rest are lost until something recounts the whole table. */
    uint32_t c = f->cluster;
    while (c) {
        uint32_t next = fat_next(c);
        if (fat_set(c, 0) != 0) break;
        c = next;
    }
    f->cluster = 0;
    f->size = 0;
    return dir_writeback(f);
}

/* An 8.3 name, padded with spaces, from an ordinary one. -1 if it does not fit,
 * which is a refusal and not a truncation: a silently shortened name is a file
 * written somewhere other than where the program asked. */
static int make_83(const char *name, uint8_t out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    int n = 0, e = 0, dot = -1;
    for (int i = 0; name[i]; i++) if (name[i] == '.') dot = i;
    for (int i = 0; name[i]; i++) {
        char ch = upper(name[i]);
        if (ch == '/' || ch == 0) return -1;
        if (i == dot) continue;
        if (dot >= 0 && i > dot) { if (e >= 3) return -1; out[8 + e++] = (uint8_t)ch; }
        else                     { if (n >= 8) return -1; out[n++] = (uint8_t)ch; }
    }
    return n ? 0 : -1;
}

int fat_create(const char *path, fat_file_t *out) {
    if (!mounted) return -1;
    if (fat_open(path, out) == 0) return -1;        /* the caller decides */

    /* Split off the last component; everything before it must already exist.
     * Creating intermediate directories is a decision a filesystem should not
     * make on a program's behalf. */
    while (*path == '/') path++;
    const char *last = path;
    for (const char *p = path; *p; p++) if (*p == '/') last = p + 1;

    uint32_t dir = root_clus;
    if (last != path) {
        char parent[128];
        unsigned n = 0;
        for (const char *p = path; p < last - 1 && n < 127; p++) parent[n++] = *p;
        parent[n] = 0;
        fat_file_t d;
        if (fat_open(parent, &d) != 0 || !d.is_dir) return -1;
        dir = d.cluster ? d.cluster : root_clus;
    }

    uint8_t name83[11];
    if (make_83(last, name83) != 0) return -1;

    /* A free slot, or the end of the directory. 0x00 means "nothing beyond
     * here", so writing into one must leave the next slot zero — which it does,
     * because clus_alloc hands out cleared clusters and a directory that has
     * never been written past this point is already zero. */
    uint32_t last_c = dir;
    for (uint32_t c = dir; c; c = fat_next(c)) {
        last_c = c;
        for (uint32_t s = 0; s < sec_per_clus; s++) {
            if (rd_sec(clus_sector(c) + s, dirbuf) != 0) return -1;
            for (int i = 0; i < 512; i += 32) {
                uint8_t *e = dirbuf + i;
                if (e[0] != 0x00 && e[0] != 0xe5) continue;
                for (int k = 0; k < 32; k++) e[k] = 0;
                for (int k = 0; k < 11; k++) e[k] = name83[k];
                e[11] = 0x20;                       /* an ordinary file */
                if (wr_sec(clus_sector(c) + s, dirbuf) != 0) return -1;
                out->cluster = 0;
                out->size    = 0;
                out->is_dir  = 0;
                out->dir_sec = clus_sector(c) + s;
                out->dir_off = (uint32_t)i;
                return 0;
            }
        }
    }

    /* Full: give it another cluster. A directory that cannot grow is one where
     * the eighth file in a root of sixteen slots fails with "no such file",
     * which is what it did — a true error code about the wrong thing.
     * clus_alloc hands out a cleared cluster, so every slot in it reads as
     * 0x00 and the first is free. */
    uint32_t nc = clus_alloc();
    if (!nc || fat_set(last_c, nc) != 0) return -1;
    if (rd_sec(clus_sector(nc), dirbuf) != 0) return -1;
    for (int k = 0; k < 32; k++) dirbuf[k] = 0;
    for (int k = 0; k < 11; k++) dirbuf[k] = name83[k];
    dirbuf[11] = 0x20;
    if (wr_sec(clus_sector(nc), dirbuf) != 0) return -1;
    out->cluster = 0;
    out->size    = 0;
    out->is_dir  = 0;
    out->dir_sec = clus_sector(nc);
    out->dir_off = 0;
    return 0;
}

/* ---- directories as things a program can look at ------------------------ */

/* The n-th name in a directory, skipping long-name parts, deleted slots and the
 * volume label. Returns 0, or -1 when there is no n-th.
 *
 * By index and not by a cursor, because a program's position in a directory is
 * a number it holds across system calls and a cursor here would be state the
 * kernel keeps per open file about a structure it re-reads anyway. The cost is
 * that reading a whole directory is quadratic in its length, which for a
 * directory small enough to print is not a cost.
 */
int fat_dirent(const fat_file_t *dir, uint32_t index,
               char name[261], uint32_t *size, int *is_dir) {
    if (!mounted || !dir->is_dir) return -1;
    uint32_t start = dir->cluster ? dir->cluster : root_clus;
    uint32_t seen = 0;
    char lfn[261];
    int  lfn_len = 0;

    for (uint32_t c = start; c; c = fat_next(c)) {
        for (uint32_t s = 0; s < sec_per_clus; s++) {
            if (rd_sec(clus_sector(c) + s, dirbuf) != 0) return -1;
            for (int i = 0; i < 512; i += 32) {
                const uint8_t *e = dirbuf + i;
                if (e[0] == 0x00) return -1;
                if (e[0] == 0xe5) { lfn_len = 0; continue; }
                if ((e[11] & 0x0f) == 0x0f) {
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

                if (seen++ == index) {
                    int n = 0;
                    if (lfn_len) {
                        for (int k = 0; k < lfn_len && k < 260 && lfn[k]; k++) name[n++] = lfn[k];
                    }
                    if (!n) { short_name(e, name); }
                    else    { name[n] = 0; }
                    *size   = rd32(e + 28);
                    *is_dir = (e[11] & 0x10) ? 1 : 0;
                    return 0;
                }
                lfn_len = 0;
            }
        }
    }
    return -1;
}

/* A directory: an entry with the directory bit, one cluster, and the two names
 * every FAT directory has. Without "." and ".." other readers call it corrupt
 * — they are ordinary entries and nothing else creates them. */
int fat_mkdir(const char *path) {
    fat_file_t d;
    if (fat_create(path, &d) != 0) return -1;

    uint32_t c = clus_alloc();
    if (!c) return -1;

    /* The parent, so ".." can name it. A directory in the root names cluster 0
     * there, which is the convention and not an oversight. */
    uint32_t parent = root_clus;
    {
        const char *p = path;
        while (*p == '/') p++;
        const char *last = p;
        for (const char *q = p; *q; q++) if (*q == '/') last = q + 1;
        if (last != p) {
            char pn[128]; unsigned n = 0;
            for (const char *q = p; q < last - 1 && n < 127; q++) pn[n++] = *q;
            pn[n] = 0;
            fat_file_t pf;
            if (fat_open(pn, &pf) == 0 && pf.is_dir) parent = pf.cluster;
        }
    }

    for (int i = 0; i < 512; i++) dirbuf[i] = 0;
    for (int k = 0; k < 11; k++) dirbuf[k] = ' ';
    dirbuf[0] = '.';  dirbuf[11] = 0x10;
    wr16(dirbuf + 20, (uint16_t)(c >> 16));
    wr16(dirbuf + 26, (uint16_t)(c & 0xffff));
    for (int k = 0; k < 11; k++) dirbuf[32 + k] = ' ';
    dirbuf[32] = '.'; dirbuf[33] = '.'; dirbuf[32 + 11] = 0x10;
    uint32_t pp = (parent == root_clus) ? 0 : parent;
    wr16(dirbuf + 32 + 20, (uint16_t)(pp >> 16));
    wr16(dirbuf + 32 + 26, (uint16_t)(pp & 0xffff));
    if (wr_sec(clus_sector(c), dirbuf) != 0) return -1;

    d.cluster = c;
    d.is_dir  = 1;
    if (rd_sec(d.dir_sec, dirbuf) != 0) return -1;
    dirbuf[d.dir_off + 11] = 0x10;                    /* it is a directory */
    wr32(dirbuf + d.dir_off + 28, 0);                 /* directories have no size */
    wr16(dirbuf + d.dir_off + 20, (uint16_t)(c >> 16));
    wr16(dirbuf + d.dir_off + 26, (uint16_t)(c & 0xffff));
    return wr_sec(d.dir_sec, dirbuf);
}

int fat_unlink(const char *path) {
    fat_file_t f;
    if (fat_open(path, &f) != 0 || !f.dir_sec) return -1;
    if (f.is_dir) {
        /* Only an empty one. Removing a directory with entries in it means
         * walking the tree, and a partial walk that stopped halfway would leave
         * clusters allocated to a directory that no longer exists — lost until
         * something recounts the whole table. */
        char name[261]; uint32_t sz; int isd;
        for (uint32_t i = 0; ; i++) {
            if (fat_dirent(&f, i, name, &sz, &isd) != 0) break;
            if (name_eq(name, ".") || name_eq(name, "..")) continue;
            return -1;                                /* -ENOTEMPTY */
        }
    }
    uint32_t c = f.cluster;
    while (c) { uint32_t n = fat_next(c); if (fat_set(c, 0) != 0) break; c = n; }
    if (rd_sec(f.dir_sec, dirbuf) != 0) return -1;
    /* 0xE5 and not 0x00: the first byte being zero means "the directory ends
     * here", and every entry after it becomes invisible. D-065 found that from
     * the writing side. */
    dirbuf[f.dir_off] = 0xe5;
    return wr_sec(f.dir_sec, dirbuf);
}
