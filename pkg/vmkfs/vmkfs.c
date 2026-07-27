/* vmkfs.vfat — VerkOS' own FAT32 formatter.
 *
 * UEFI requires the EFI System Partition to be FAT, so an installer that
 * cannot create one cannot install anything. FAT32 is small enough and
 * specified precisely enough to own outright: a boot sector describing the
 * layout, two copies of a table of 32-bit cluster links, and a root directory
 * that is just another cluster chain. (ext4 is a different proposition and
 * stays e2fsprogs' job — see docs/DECISIONS.md D-014.)
 *
 *   vmkfs.vfat [-n LABEL] [-v] DEVICE_OR_FILE [SECTORS]
 *
 * Deliberately FAT32 only. FAT12/16 differ in the width of every table entry
 * and in having a fixed-size root directory; supporting them would double the
 * code to serve volumes below 33 MB, which is smaller than any sane ESP.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#define SECTOR       512
#define RESERVED     32          /* boot sector, FSInfo, backup pair, slack */
#define NUM_FATS     2
#define BACKUP_SEC   6
#define FSINFO_SEC   1
#define ROOT_CLUSTER 2
#define FAT32_MIN_CLUSTERS 65525 /* below this the volume is not FAT32 at all */

static int verbose;

static void die(const char *msg) { fprintf(stderr, "vmkfs.vfat: %s\n", msg); exit(1); }
static void die_errno(const char *msg) {
    fprintf(stderr, "vmkfs.vfat: %s: %s\n", msg, strerror(errno));
    exit(1);
}

/* FAT is little-endian on every platform, so write the fields explicitly
 * rather than memcpy-ing structs and hoping the host agrees. */
static void put16(unsigned char *p, unsigned v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void put32(unsigned char *p, unsigned long v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

static void write_at(int fd, off_t sector, const void *buf, size_t len) {
    if (pwrite(fd, buf, len, sector * SECTOR) != (ssize_t)len) die_errno("write");
}

/* How many sectors the device holds: a block device knows, a plain file is
 * measured, and an explicit count on the command line beats both. */
static unsigned long device_sectors(int fd, const char *path) {
    struct stat st;
    if (fstat(fd, &st)) die_errno(path);
    if (S_ISBLK(st.st_mode)) {
        unsigned long long bytes = 0;
        if (ioctl(fd, BLKGETSIZE64, &bytes)) die_errno("BLKGETSIZE64");
        return (unsigned long)(bytes / SECTOR);
    }
    return (unsigned long)(st.st_size / SECTOR);
}

/* Microsoft's sizing table. The constraint that actually matters is at the
 * bottom: too few clusters and the volume is FAT16 by definition, and a driver
 * reading it as FAT32 would misread every chain. */
static unsigned pick_cluster_size(unsigned long total_sectors) {
    unsigned long mb = total_sectors / 2048;
    unsigned spc;
    if (mb <= 260)        spc = 1;
    else if (mb <= 8192)  spc = 8;
    else if (mb <= 16384) spc = 16;
    else if (mb <= 32768) spc = 32;
    else                  spc = 64;
    return spc;
}

/* The FAT has to be big enough to hold one entry per cluster, but the number of
 * clusters depends on how much space the FAT itself takes. Microsoft's
 * closed-form approximation, then verified below. */
static unsigned long fat_sectors_for(unsigned long total_sectors, unsigned spc) {
    unsigned long avail = total_sectors - RESERVED;
    unsigned long denom = ((256UL * spc) + NUM_FATS) / 2;
    return (avail + (denom - 1)) / denom;
}

static void format(int fd, unsigned long total_sectors, const char *label) {
    if (total_sectors < 2048) die("device is too small to hold a filesystem");

    unsigned spc = pick_cluster_size(total_sectors);
    unsigned long fat_sz, clusters;
    for (;;) {
        fat_sz = fat_sectors_for(total_sectors, spc);
        unsigned long data = total_sectors - RESERVED - (unsigned long)NUM_FATS * fat_sz;
        clusters = data / spc;
        if (clusters >= FAT32_MIN_CLUSTERS) break;
        if (spc == 1)
            die("too small for FAT32 (needs about 33 MB); FAT12/16 are not supported");
        spc /= 2;                       /* smaller clusters, more of them */
    }
    if (clusters > 0x0FFFFFF5UL) die("too large for FAT32");

    unsigned long fat_start  = RESERVED;
    unsigned long data_start = RESERVED + (unsigned long)NUM_FATS * fat_sz;
    /* A volume serial derived from the clock alone collides for every
     * filesystem made in the same second, and blkid then cannot tell two disks
     * apart. Take it from the kernel's pool, and only fall back to the clock. */
    unsigned long volume_id;
    int rnd = open("/dev/urandom", O_RDONLY);
    if (rnd < 0 || read(rnd, &volume_id, sizeof volume_id) != (ssize_t)sizeof volume_id)
        volume_id = (unsigned long)time(NULL) ^ ((unsigned long)getpid() << 16);
    if (rnd >= 0) close(rnd);

    if (verbose)
        fprintf(stderr,
                "vmkfs.vfat: %lu sectors, %u sectors/cluster, %lu clusters,\n"
                "            FAT %lu sectors x%d, data starts at sector %lu\n",
                total_sectors, spc, clusters, fat_sz, NUM_FATS, data_start);

    /* ---- boot sector ---- */
    unsigned char bs[SECTOR];
    memset(bs, 0, sizeof bs);
    bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90;      /* jmp short +0x58; nop */
    memcpy(bs + 3, "VerkOS  ", 8);                 /* OEM name, exactly 8 bytes */
    put16(bs + 11, SECTOR);                        /* bytes per sector */
    bs[13] = (unsigned char)spc;                   /* sectors per cluster */
    put16(bs + 14, RESERVED);                      /* reserved sectors */
    bs[16] = NUM_FATS;
    put16(bs + 17, 0);                             /* root entries: 0 on FAT32 */
    put16(bs + 19, 0);                             /* 16-bit total: 0 on FAT32 */
    bs[21] = 0xF8;                                 /* media: fixed disk */
    put16(bs + 22, 0);                             /* 16-bit FAT size: 0 on FAT32 */
    put16(bs + 24, 32);                            /* sectors per track (legacy) */
    put16(bs + 26, 64);                            /* heads (legacy) */
    put32(bs + 28, 0);                             /* hidden sectors */
    put32(bs + 32, total_sectors);
    put32(bs + 36, fat_sz);                        /* 32-bit FAT size */
    put16(bs + 40, 0);                             /* flags: both FATs mirrored */
    put16(bs + 42, 0);                             /* version 0.0 */
    put32(bs + 44, ROOT_CLUSTER);
    put16(bs + 48, FSINFO_SEC);
    put16(bs + 50, BACKUP_SEC);
    bs[64] = 0x80;                                 /* drive number */
    bs[66] = 0x29;                                 /* extended boot signature */
    put32(bs + 67, volume_id);
    memset(bs + 71, ' ', 11);
    for (int i = 0; i < 11 && label[i]; i++) bs[71 + i] = (unsigned char)toupper((unsigned char)label[i]);
    memcpy(bs + 82, "FAT32   ", 8);
    /* A machine that tries to boot this partition directly should say so
     * rather than execute whatever happens to be here. */
    static const char nomedia[] = "Not a bootable disk.\r\n";
    memcpy(bs + 90, nomedia, sizeof nomedia - 1);
    bs[510] = 0x55; bs[511] = 0xAA;

    /* ---- FSInfo ---- */
    unsigned char fsi[SECTOR];
    memset(fsi, 0, sizeof fsi);
    put32(fsi + 0,   0x41615252UL);                /* "RRaA" */
    put32(fsi + 484, 0x61417272UL);                /* "rrAa" */
    put32(fsi + 488, clusters - 1);                /* free clusters (root uses one) */
    put32(fsi + 492, ROOT_CLUSTER + 1);            /* hint: next free cluster */
    put32(fsi + 508, 0xAA550000UL);
    fsi[510] = 0x55; fsi[511] = 0xAA;

    write_at(fd, 0, bs, SECTOR);
    write_at(fd, FSINFO_SEC, fsi, SECTOR);
    /* The backup pair is what a driver falls back to when sector 0 is damaged;
     * writing it is the whole reason it is useful. */
    write_at(fd, BACKUP_SEC, bs, SECTOR);
    write_at(fd, BACKUP_SEC + FSINFO_SEC, fsi, SECTOR);

    /* ---- the FATs ---- */
    unsigned char *zero = calloc(1, SECTOR * 64);
    if (!zero) die("out of memory");
    for (int copy = 0; copy < NUM_FATS; copy++) {
        unsigned long base = fat_start + (unsigned long)copy * fat_sz;
        for (unsigned long s = 0; s < fat_sz; s += 64) {
            unsigned long n = fat_sz - s < 64 ? fat_sz - s : 64;
            write_at(fd, base + s, zero, n * SECTOR);
        }
        unsigned char first[SECTOR];
        memset(first, 0, sizeof first);
        put32(first + 0, 0x0FFFFFF8UL);            /* media descriptor + reserved */
        put32(first + 4, 0x0FFFFFFFUL);            /* end-of-chain marker */
        put32(first + 8, 0x0FFFFFFFUL);            /* cluster 2: root dir, one cluster */
        write_at(fd, base, first, SECTOR);
    }

    /* ---- root directory ---- */
    for (unsigned s = 0; s < spc; s += 64) {
        unsigned n = spc - s < 64 ? spc - s : 64;
        write_at(fd, data_start + s, zero, (size_t)n * SECTOR);
    }
    /* The label lives twice: in the boot sector, and as a volume-label entry in
     * the root directory. Tools disagree about which they read, so write both. */
    if (label[0]) {
        unsigned char ent[32];
        memset(ent, 0, sizeof ent);
        memset(ent, ' ', 11);
        for (int i = 0; i < 11 && label[i]; i++) ent[i] = (unsigned char)toupper((unsigned char)label[i]);
        ent[11] = 0x08;                            /* ATTR_VOLUME_ID */
        write_at(fd, data_start, ent, sizeof ent);
    }
    free(zero);

    if (fsync(fd)) die_errno("fsync");
    if (verbose) fprintf(stderr, "vmkfs.vfat: done\n");
}


/* ==========================================================================
 * Writing files into a volume we made.
 *
 * The build used mtools for this — `mmd` and `mcopy` — which meant the last
 * third-party tool in the boot path was the one that filled the partition the
 * machine starts from. It is also where D-052 hid: nothing checked what came
 * out, so an ESP was FAT16 for a year.
 *
 * The geometry is read back from the volume rather than recomputed. vmkfs
 * halves the cluster size in a loop until the cluster count is legal, so the
 * size table alone does not predict what is on the disk — and a writer that
 * disagrees with the boot sector by one sector corrupts silently.
 * ========================================================================== */

typedef struct {
    int           fd;
    unsigned      bps;          /* bytes per sector                          */
    unsigned      spc;          /* sectors per cluster                       */
    unsigned      nfats;
    unsigned long rsvd, fatsz, totsec, rootclus, fsinfo, backup;
    unsigned long data_start;   /* first sector of the data region           */
    unsigned long nclus;        /* count of data clusters; last is nclus + 1 */
} vol_t;

static unsigned get16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static unsigned long get32(const unsigned char *p) {
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static void read_at(int fd, unsigned long sector, void *buf, size_t len) {
    if (pread(fd, buf, len, (off_t)sector * SECTOR) != (ssize_t)len) die_errno("read");
}

static void vol_open(vol_t *v, int fd) {
    unsigned char bs[SECTOR];
    read_at(fd, 0, bs, SECTOR);
    if (bs[510] != 0x55 || bs[511] != 0xAA) die("no boot signature: not a FAT volume");

    v->fd     = fd;
    v->bps    = get16(bs + 11);
    v->spc    = bs[13];
    v->rsvd   = get16(bs + 14);
    v->nfats  = bs[16];
    v->fatsz  = get32(bs + 36);
    v->totsec = get32(bs + 32);
    v->rootclus = get32(bs + 44);
    v->fsinfo = get16(bs + 48);
    v->backup = get16(bs + 50);

    /* Everything that would make this not a FAT32 volume, checked rather than
     * assumed. A FAT16 image has a non-zero root-entry count and a 16-bit FAT
     * size, and writing FAT32 structures into one produces a filesystem that
     * mounts and is wrong. */
    if (v->bps != SECTOR)          die("only 512-byte sectors are supported");
    if (get16(bs + 17) != 0)       die("root entry count is not zero: this is not FAT32");
    if (get16(bs + 22) != 0)       die("16-bit FAT size is set: this is not FAT32");
    if (!v->spc || (v->spc & (v->spc - 1))) die("sectors per cluster is not a power of two");
    if (!v->nfats || !v->fatsz)    die("no FATs");
    if (!v->totsec) v->totsec = get16(bs + 19);

    v->data_start = v->rsvd + (unsigned long)v->nfats * v->fatsz;
    if (v->totsec <= v->data_start) die("the data region starts past the end of the volume");
    v->nclus = (v->totsec - v->data_start) / v->spc;
    if (v->nclus < FAT32_MIN_CLUSTERS) die("fewer than 65525 clusters: this is not FAT32");
}

static unsigned long clus_sector(const vol_t *v, unsigned long c) {
    return v->data_start + (c - 2) * v->spc;
}

/* ---- the FAT, with one sector cached ------------------------------------
 *
 * A 30 MB kernel at 512 bytes per cluster is about 61 000 links across 477
 * sectors, and two copies of each. Reading and writing a sector per link is
 * not merely slow, it is slow enough that nobody would run it — so one sector
 * is held. Which makes the flush the load-bearing part: drop it and the last
 * hundred-odd links stay zero, the file reads back 99.8% correct, and the
 * failure lands somewhere far away. */
static unsigned char fat_buf[SECTOR];
static long          fat_cached = -1;
static int           fat_dirty;

static void fat_flush(vol_t *v) {
    if (fat_cached < 0 || !fat_dirty) return;
    for (unsigned i = 0; i < v->nfats; i++)
        write_at(v->fd, v->rsvd + (unsigned long)i * v->fatsz + (unsigned long)fat_cached,
                 fat_buf, SECTOR);
    fat_dirty = 0;
}

static void fat_load(vol_t *v, unsigned long sec) {
    if (fat_cached == (long)sec) return;
    fat_flush(v);
    read_at(v->fd, v->rsvd + sec, fat_buf, SECTOR);
    fat_cached = (long)sec;
}

static unsigned long fat_get(vol_t *v, unsigned long clus) {
    fat_load(v, clus / (SECTOR / 4));
    return get32(fat_buf + (clus % (SECTOR / 4)) * 4) & 0x0FFFFFFFUL;
}

/* The top four bits of every entry are reserved and must survive a write. */
static void fat_set(vol_t *v, unsigned long clus, unsigned long val) {
    fat_load(v, clus / (SECTOR / 4));
    unsigned char *e = fat_buf + (clus % (SECTOR / 4)) * 4;
    unsigned long keep = get32(e) & 0xF0000000UL;
    put32(e, keep | (val & 0x0FFFFFFFUL));
    fat_dirty = 1;
}

#define FAT_EOC 0x0FFFFFFFUL

static unsigned long alloc_hint = 2;
static unsigned long alloc_count;          /* how many we have taken */

/* Clusters run from 2 to nclus + 1 inclusive. The FAT is over-provisioned —
 * vmkfs sizes it with Microsoft's approximation, which rounds up — so there
 * are entries past the last real cluster that read as zero and look exactly
 * like free space. Allocating one puts data past the end of the image: a
 * regular file simply grows, and stage 70 then dd's those bytes over the
 * backup GPT. Bounded here, once. */
static unsigned long clus_alloc(vol_t *v) {
    unsigned long last = v->nclus + 1;
    for (unsigned long tries = 0; tries <= v->nclus; tries++) {
        unsigned long c = alloc_hint + tries;
        if (c > last) c = 2 + (c - 2) % (last - 1);
        if (fat_get(v, c) == 0) {
            fat_set(v, c, FAT_EOC);
            alloc_hint = (c + 1 > last) ? 2 : c + 1;
            alloc_count++;
            return c;
        }
    }
    die("the volume is full");
    return 0;
}

static void clus_zero(vol_t *v, unsigned long c) {
    unsigned char zero[SECTOR];
    memset(zero, 0, sizeof zero);
    for (unsigned s = 0; s < v->spc; s++)
        write_at(v->fd, clus_sector(v, c) + s, zero, SECTOR);
}

/* ---- short names and long names ----------------------------------------- */

static int short_ok(unsigned char c) {
    return isalnum(c) || strchr("$%'-_@~`!(){}^#&", (char)c) != NULL;
}

/* The 11-byte on-disk field: eight of name, three of extension, space padded,
 * no dot. Anything that does not fit exactly gets a ~N tail, which is why the
 * long name has to exist at all. */
static void short_name(const char *name, unsigned char out[11], int seq) {
    memset(out, ' ', 11);
    const char *dot = strrchr(name, '.');
    if (dot == name) dot = NULL;
    size_t blen = dot ? (size_t)(dot - name) : strlen(name);

    char tail[8];
    int taillen = 0;
    if (seq > 0) taillen = snprintf(tail, sizeof tail, "~%d", seq);

    unsigned o = 0;
    for (size_t i = 0; i < blen && o < 8 - (unsigned)taillen; i++) {
        unsigned char c = (unsigned char)toupper((unsigned char)name[i]);
        if (c == ' ') continue;
        out[o++] = short_ok(c) ? c : '_';
    }
    for (int i = 0; i < taillen; i++) out[o++] = (unsigned char)tail[i];
    if (dot) {
        unsigned e = 0;
        for (size_t i = 1; dot[i] && e < 3; i++) {
            unsigned char c = (unsigned char)toupper((unsigned char)dot[i]);
            out[8 + e++] = short_ok(c) ? c : '_';
        }
    }
}

/* Does the name fit the 8.3 field exactly, so no long name is needed? */
static int fits_short(const char *name) {
    const char *dot = strrchr(name, '.');
    if (dot == name) return 0;
    size_t blen = dot ? (size_t)(dot - name) : strlen(name);
    size_t elen = dot ? strlen(dot + 1) : 0;
    if (blen == 0 || blen > 8 || elen > 3) return 0;
    if (dot && strchr(dot + 1, '.')) return 0;
    for (const char *p = name; *p; p++) {
        if (*p == '.') continue;
        if (islower((unsigned char)*p)) return 0;      /* lower case needs an LFN */
        if (!short_ok((unsigned char)*p)) return 0;
    }
    return 1;
}

/* Over the eleven raw short-name bytes, before any of them are interpreted.
 * Computing it over the long name, or over the name with its dot, is the
 * classic way to produce a volume Linux reads and firmware does not. */
static unsigned char lfn_checksum(const unsigned char sn[11]) {
    unsigned char sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (unsigned char)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + sn[i]);
    return sum;
}


/* ---- directory entries --------------------------------------------------
 *
 * A cluster of 512 bytes holds sixteen entries, and a long name costs one
 * entry per thirteen characters *plus* the short one. The shipped ESP already
 * needs fourteen of the sixteen in its root, so growing a directory is not a
 * later refinement — it happens on the eighth file. */

#define ATTR_RO 0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYSTEM 0x04
#define ATTR_VOL 0x08
#define ATTR_DIR 0x10
#define ATTR_LFN 0x0F

/* Walk a directory chain looking for `need` consecutive free slots, growing it
 * by a cluster if there are none. Returns the cluster and the entry index
 * within it. */
static void dir_space(vol_t *v, unsigned long dir, unsigned need,
                      unsigned long *out_clus, unsigned *out_idx) {
    unsigned per = (v->spc * SECTOR) / 32;
    unsigned char buf[SECTOR];
    unsigned long c = dir, prev = dir;

    while (c >= 2 && c < FAT_EOC) {
        unsigned run = 0, start = 0;
        for (unsigned s = 0; s < v->spc; s++) {
            read_at(v->fd, clus_sector(v, c) + s, buf, SECTOR);
            for (unsigned e = 0; e < SECTOR / 32; e++) {
                unsigned idx = s * (SECTOR / 32) + e;
                unsigned char first = buf[e * 32];
                if (first == 0x00 || first == 0xE5) {
                    if (!run) start = idx;
                    if (++run == need) { *out_clus = c; *out_idx = start; return; }
                } else run = 0;
            }
        }
        prev = c;
        c = fat_get(v, c);
        if (c < 2 || c >= 0x0FFFFFF8UL) break;
    }
    /* Out of room. One more cluster, zeroed — an unzeroed directory cluster is
     * sixteen entries of whatever was there before, and the first byte of each
     * decides whether it is a file.
     *
     * But first: any slot still holding 0x00 in the cluster we are leaving has
     * to become 0xE5. A first byte of zero does not mean "this slot is free",
     * it means "the directory ends here, and nothing after this is a file" —
     * so a gap left behind when a directory grows makes every entry in every
     * later cluster invisible. It is invisible in the worst way, too: fsck.fat
     * kept scanning and reported all 32 files, while mtools stopped at the
     * gap and saw 5. A checker that walks the chain and a reader that honours
     * the marker disagree, and the reader is the one that is right. */
    unsigned char buf2[SECTOR];
    for (unsigned s = 0; s < v->spc; s++) {
        int touched = 0;
        read_at(v->fd, clus_sector(v, prev) + s, buf2, SECTOR);
        for (unsigned e = 0; e < SECTOR / 32; e++)
            if (buf2[e * 32] == 0x00) { buf2[e * 32] = 0xE5; touched = 1; }
        if (touched) write_at(v->fd, clus_sector(v, prev) + s, buf2, SECTOR);
    }

    unsigned long fresh = clus_alloc(v);
    clus_zero(v, fresh);
    fat_set(v, prev, fresh);
    (void)per;
    *out_clus = fresh;
    *out_idx = 0;
}

static void dir_write(vol_t *v, unsigned long clus, unsigned idx,
                      const unsigned char ent[32]) {
    unsigned sec = idx / (SECTOR / 32), off = (idx % (SECTOR / 32)) * 32;
    unsigned char buf[SECTOR];
    read_at(v->fd, clus_sector(v, clus) + sec, buf, SECTOR);
    memcpy(buf + off, ent, 32);
    write_at(v->fd, clus_sector(v, clus) + sec, buf, SECTOR);
}

/* Is this short name already taken in this directory? ~1 has to become ~2. */
static int short_taken(vol_t *v, unsigned long dir, const unsigned char sn[11]) {
    unsigned char buf[SECTOR];
    for (unsigned long c = dir; c >= 2 && c < 0x0FFFFFF8UL; c = fat_get(v, c))
        for (unsigned s = 0; s < v->spc; s++) {
            read_at(v->fd, clus_sector(v, c) + s, buf, SECTOR);
            for (unsigned e = 0; e < SECTOR / 32; e++) {
                unsigned char *d = buf + e * 32;
                if (d[0] == 0x00) return 0;
                if (d[0] == 0xE5 || (d[11] & ATTR_LFN) == ATTR_LFN) continue;
                if (!memcmp(d, sn, 11)) return 1;
            }
        }
    return 0;
}

/* Create the entry (long name first, in reverse order, then the short one) and
 * return nothing — the caller already knows the cluster and size it asked for. */
static void dir_add(vol_t *v, unsigned long dir, const char *name,
                    unsigned char attr, unsigned long first_clus, unsigned long size) {
    unsigned char sn[11];
    int need_lfn = !fits_short(name);
    if (need_lfn) {
        int seq;
        for (seq = 1; seq < 1000; seq++) {
            short_name(name, sn, seq);
            if (!short_taken(v, dir, sn)) break;
        }
        if (verbose) fprintf(stderr, "  short name for %s: %.11s (seq %d)\n", name, sn, seq);
    } else {
        short_name(name, sn, 0);
        if (short_taken(v, dir, sn)) die("a file of that name is already there");
    }

    size_t nlen = strlen(name);
    unsigned lfn_entries = need_lfn ? (unsigned)((nlen + 12) / 13) : 0;
    unsigned long clus; unsigned idx;
    dir_space(v, dir, lfn_entries + 1, &clus, &idx);

    unsigned char sum = lfn_checksum(sn);
    /* Highest ordinal first, with 0x40 marking it the last of the set. The
     * order is the part everyone gets wrong; on disk the name reads backwards. */
    for (unsigned n = lfn_entries; n >= 1; n--) {
        unsigned char e[32];
        memset(e, 0, sizeof e);
        e[0] = (unsigned char)n | (n == lfn_entries ? 0x40 : 0);
        e[11] = ATTR_LFN;
        e[13] = sum;
        static const unsigned char slots[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
        for (unsigned k = 0; k < 13; k++) {
            size_t pos = (size_t)(n - 1) * 13 + k;
            unsigned short ch;
            if (pos < nlen)       ch = (unsigned char)name[pos];
            else if (pos == nlen) ch = 0;
            else                  ch = 0xFFFF;
            e[slots[k]]     = (unsigned char)(ch & 0xFF);
            e[slots[k] + 1] = (unsigned char)(ch >> 8);
        }
        dir_write(v, clus, idx++, e);
    }

    unsigned char e[32];
    memset(e, 0, sizeof e);
    memcpy(e, sn, 11);
    e[11] = attr;
    put16(e + 20, (unsigned)(first_clus >> 16));
    put16(e + 26, (unsigned)(first_clus & 0xFFFF));
    put32(e + 28, size);
    dir_write(v, clus, idx, e);
}

/* ---- resolving a path ---------------------------------------------------- */

static int name_eq_entry(const unsigned char sn[11], const char *want) {
    unsigned char probe[11];
    short_name(want, probe, 0);
    return !memcmp(sn, probe, 11);
}

/* One component. Returns the directory's first cluster, or 0. Long names are
 * matched through their short alias, which is enough because every directory
 * this build creates has an 8.3 name. */
static unsigned long dir_lookup(vol_t *v, unsigned long dir, const char *name) {
    unsigned char buf[SECTOR];
    for (unsigned long c = dir; c >= 2 && c < 0x0FFFFFF8UL; c = fat_get(v, c))
        for (unsigned s = 0; s < v->spc; s++) {
            read_at(v->fd, clus_sector(v, c) + s, buf, SECTOR);
            for (unsigned e = 0; e < SECTOR / 32; e++) {
                unsigned char *d = buf + e * 32;
                if (d[0] == 0x00) return 0;
                if (d[0] == 0xE5 || (d[11] & ATTR_LFN) == ATTR_LFN) continue;
                if (!(d[11] & ATTR_DIR)) continue;
                if (name_eq_entry(d, name))
                    return ((unsigned long)get16(d + 20) << 16) | get16(d + 26);
            }
        }
    return 0;
}

static unsigned long make_dir(vol_t *v, unsigned long parent, const char *name) {
    unsigned long c = clus_alloc(v);
    clus_zero(v, c);

    /* "." points at this directory and ".." at the parent — except that a ".."
     * whose parent is the root holds zero, not the root's cluster number. A
     * driver that walks up from a subdirectory relies on that exact zero. */
    unsigned char e[32];
    memset(e, 0, sizeof e); memset(e, ' ', 11);
    e[0] = '.'; e[11] = ATTR_DIR;
    put16(e + 20, (unsigned)(c >> 16)); put16(e + 26, (unsigned)(c & 0xFFFF));
    dir_write(v, c, 0, e);

    memset(e, 0, sizeof e); memset(e, ' ', 11);
    e[0] = '.'; e[1] = '.'; e[11] = ATTR_DIR;
    unsigned long up = (parent == v->rootclus) ? 0 : parent;
    put16(e + 20, (unsigned)(up >> 16)); put16(e + 26, (unsigned)(up & 0xFFFF));
    dir_write(v, c, 1, e);

    dir_add(v, parent, name, ATTR_DIR, c, 0);
    return c;
}

/* Walk a path, creating directories along the way when asked to. */
static unsigned long walk(vol_t *v, const char *path, int create, char *leaf, size_t leafsz) {
    char tmp[512];
    if (strlen(path) >= sizeof tmp) die("path too long");
    strcpy(tmp, path);

    unsigned long dir = v->rootclus;
    char *p = tmp;
    while (*p == '/') p++;
    for (;;) {
        char *slash = strchr(p, '/');
        if (!slash) break;
        *slash = 0;
        if (*p) {
            unsigned long next = dir_lookup(v, dir, p);
            if (!next) {
                if (!create) die("no such directory on the volume");
                next = make_dir(v, dir, p);
            }
            dir = next;
        }
        p = slash + 1;
        while (*p == '/') p++;
    }
    if (leaf) snprintf(leaf, leafsz, "%s", p);
    return dir;
}

/* ---- FSInfo -------------------------------------------------------------- */

/* Both copies, or a driver that reads the backup gets a number from before any
 * of this ran. Kept accurate rather than marked unknown: we know exactly how
 * many clusters were taken. */
static void fsinfo_update(vol_t *v) {
    if (!v->fsinfo) return;
    unsigned char fsi[SECTOR];
    read_at(v->fd, v->fsinfo, fsi, SECTOR);
    unsigned long free_now = get32(fsi + 488);
    if (free_now != 0xFFFFFFFFUL) {
        free_now = free_now > alloc_count ? free_now - alloc_count : 0;
        put32(fsi + 488, free_now);
    }
    put32(fsi + 492, alloc_hint);
    write_at(v->fd, v->fsinfo, fsi, SECTOR);
    if (v->backup) write_at(v->fd, v->backup + v->fsinfo, fsi, SECTOR);
}

/* ---- the two operations the build needs ---------------------------------- */

static void do_mkdir(int fd, const char *path) {
    vol_t v; vol_open(&v, fd);
    char leaf[256];
    unsigned long parent = walk(&v, path, 1, leaf, sizeof leaf);
    if (leaf[0]) {
        if (!dir_lookup(&v, parent, leaf)) make_dir(&v, parent, leaf);
    }
    fat_flush(&v);
    fsinfo_update(&v);
    if (fsync(fd)) die_errno("fsync");
}

static void do_copy(int fd, const char *src, const char *dest) {
    vol_t v; vol_open(&v, fd);

    int in = open(src, O_RDONLY);
    if (in < 0) die_errno(src);
    struct stat st;
    if (fstat(in, &st)) die_errno(src);
    unsigned long size = (unsigned long)st.st_size;

    char leaf[256];
    unsigned long dir = walk(&v, dest, 1, leaf, sizeof leaf);
    if (!leaf[0]) die("no file name in the destination");

    unsigned long clus_bytes = (unsigned long)v.spc * SECTOR;
    unsigned long first = 0, prev = 0, written = 0;
    unsigned char *buf = malloc(clus_bytes);
    if (!buf) die("out of memory");

    /* A zero-length file gets no chain at all and a first cluster of zero.
     * Giving it one cluster is the kind of wrong that reads back correctly and
     * fails fsck. */
    while (written < size) {
        unsigned long want = size - written < clus_bytes ? size - written : clus_bytes;
        memset(buf, 0, clus_bytes);
        /* Looped, because a short read leaves the tail as zeroes, the size
         * field still right, and every structural check still passing. */
        unsigned long got = 0;
        while (got < want) {
            ssize_t n = read(in, buf + got, want - got);
            if (n < 0) die_errno(src);
            if (n == 0) break;
            got += (unsigned long)n;
        }
        if (got != want) die("the source file ended early");

        unsigned long c = clus_alloc(&v);
        if (!first) first = c; else fat_set(&v, prev, c);
        prev = c;
        for (unsigned s = 0; s < v.spc; s++)
            write_at(v.fd, clus_sector(&v, c) + s, buf + (unsigned long)s * SECTOR, SECTOR);
        written += want;
    }
    free(buf);
    close(in);

    dir_add(&v, dir, leaf, 0x20 /* ATTR_ARCHIVE */, first, size);
    fat_flush(&v);
    fsinfo_update(&v);
    if (fsync(fd)) die_errno("fsync");
    if (verbose)
        fprintf(stderr, "vmkfs.vfat: %s -> %s (%lu bytes, %lu clusters)\n",
                src, dest, size, alloc_count);
}

int main(int argc, char **argv) {
    const char *label = "";
    const char *path = NULL;
    unsigned long sectors = 0;

    /* Two extra modes, which is what removes mtools from the build:
     *   vmkfs.vfat --mkdir IMAGE PATH
     *   vmkfs.vfat --copy  IMAGE SRC  PATH
     * Both operate on a volume that already exists and read its geometry from
     * the boot sector rather than assuming what vmkfs would have chosen.
     *
     * The mode is looked for anywhere in the arguments, and a misplaced one
     * used to fall through to the formatter — so `vmkfs -v --copy img src dst`
     * did not copy into the image, it stood one option away from *reformatting
     * the thing it was asked to add a file to*. A mode that is present must be
     * obeyed or refused, never ignored. */
    int mode = 0;                       /* 1 = mkdir, 2 = copy */
    const char *rest[3]; int nrest = 0;
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--mkdir")) mode = 1;
        else if (!strcmp(argv[i], "--copy"))  mode = 2;
        else if (!strcmp(argv[i], "-v"))      verbose = 1;
        else if (argv[i][0] == '-' && mode)
            die("unknown option in --mkdir/--copy mode");
        else if (mode) {
            if (nrest == 3) die("too many arguments");
            rest[nrest++] = argv[i];
        }
    }
    if (mode) {
        int want = mode == 2 ? 3 : 2;
        if (nrest != want) {
            fprintf(stderr, "usage: vmkfs.vfat --mkdir IMAGE PATH\n"
                            "       vmkfs.vfat --copy  IMAGE SRC PATH\n");
            return 1;
        }
        int fd = open(rest[0], O_RDWR);
        if (fd < 0) die_errno(rest[0]);
        if (mode == 2) do_copy(fd, rest[1], rest[2]);
        else           do_mkdir(fd, rest[1]);
        close(fd);
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) label = argv[++i];
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-F") && i + 1 < argc) {
            if (atoi(argv[++i]) != 32) die("only FAT32 is supported (-F 32)");
        }
        else if (!strcmp(argv[i], "-I") || !strcmp(argv[i], "-f")) continue;  /* accepted, ignored */
        else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: vmkfs.vfat [-n LABEL] [-v] DEVICE [SECTORS]\n");
            return 1;
        }
        else if (!path) path = argv[i];
        else sectors = strtoul(argv[i], NULL, 10);
    }
    if (!path) { fprintf(stderr, "usage: vmkfs.vfat [-n LABEL] [-v] DEVICE [SECTORS]\n"); return 1; }
    if (strlen(label) > 11) die("a volume label is at most 11 characters");

    int fd = open(path, O_RDWR);
    if (fd < 0) die_errno(path);
    if (!sectors) sectors = device_sectors(fd, path);
    if (!sectors) die("cannot determine the size of the device");

    format(fd, sectors, label);
    close(fd);
    return 0;
}
