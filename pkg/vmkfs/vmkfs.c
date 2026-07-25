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

int main(int argc, char **argv) {
    const char *label = "";
    const char *path = NULL;
    unsigned long sectors = 0;

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
