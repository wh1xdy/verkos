/* vpart — VerkOS' own GPT writer.
 *
 * The last third-party tool in the boot disk. vmkfs makes the EFI system
 * partition and now fills it (D-065); parted was still the thing that said
 * where that partition *is*, which is the one question the firmware asks
 * before anything else.
 *
 *   vpart [-v] DISK --esp MB [--linux MB]
 *
 * A GPT is four structures and two checksums, and every one of them exists in
 * duplicate: a protective MBR so that a tool which only understands MBRs sees
 * one occupied disk rather than an empty one, a header at LBA 1 and its mirror
 * at the last sector, and an array of 128 entries after each. The mirrors are
 * the part worth being careful about — firmware is entitled to read either,
 * and a backup that disagrees with the primary is a disk that boots on one
 * machine and not the next.
 *
 * Deliberately not a partition editor. It writes a table from nothing; it does
 * not read one, resize one or move anything. An installer that needs to add a
 * partition beside somebody else's data is a different program with a much
 * higher bar (D-015), and this is not it.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

#define SECTOR      512
#define ENTRIES     128            /* the standard count; firmware assumes it */
#define ENTRY_SIZE  128
#define ARRAY_SECS  ((ENTRIES * ENTRY_SIZE) / SECTOR)     /* 32 */

static int verbose;

static void die(const char *m)        { fprintf(stderr, "vpart: %s\n", m); exit(1); }
static void die_errno(const char *m)  { fprintf(stderr, "vpart: %s: %s\n", m, strerror(errno)); exit(1); }

static void put16(unsigned char *p, unsigned v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void put32(unsigned char *p, unsigned long v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}
static void put64(unsigned char *p, unsigned long long v) {
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i));
}

/* CRC-32/ISO-HDLC, the same one vgzip uses and the one the UEFI specification
 * names. Built rather than tabulated: the table is 1 KiB of source that says
 * less than the four lines that generate it. */
static unsigned crc32(const unsigned char *p, size_t n) {
    static unsigned tab[256];
    static int built;
    if (!built) {
        for (unsigned i = 0; i < 256; i++) {
            unsigned c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
        built = 1;
    }
    unsigned crc = 0xFFFFFFFFu;
    while (n--) crc = tab[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

static void write_at(int fd, unsigned long long lba, const void *buf, size_t len) {
    if (pwrite(fd, buf, len, (off_t)lba * SECTOR) != (ssize_t)len) die_errno("write");
}

/* A type GUID is written with its first three fields little-endian and the
 * last two as written — the "mixed endian" layout that makes a GPT dump look
 * byte-swapped next to the text form. Getting it wrong produces a partition
 * the firmware simply does not recognise as an ESP, with no error anywhere. */
static void guid(unsigned char out[16], unsigned long d1, unsigned d2, unsigned d3,
                 const unsigned char rest[8]) {
    put32(out, d1); put16(out + 4, d2); put16(out + 6, d3);
    memcpy(out + 8, rest, 8);
}

static const unsigned char ESP_REST[8]   = {0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B};
static const unsigned char LINUX_REST[8] = {0x8E,0x79,0x3D,0x69,0xD8,0x47,0x7D,0xE4};

/* Random, and version 4 as the specification requires: two nibbles say where
 * the bits came from, and a tool that reads them is entitled to believe it. */
static void guid_random(unsigned char out[16]) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, out, 16) != 16) die("cannot read /dev/urandom");
    close(fd);
    out[7] = (unsigned char)((out[7] & 0x0F) | 0x40);   /* version 4  */
    out[8] = (unsigned char)((out[8] & 0x3F) | 0x80);   /* variant 1  */
}

typedef struct { unsigned char type[16]; unsigned long long first, last; const char *name; } part_t;

static void write_gpt(int fd, unsigned long long disk_sectors, part_t *parts, int nparts) {
    unsigned long long last_lba = disk_sectors - 1;
    unsigned long long first_usable = 2 + ARRAY_SECS;              /* 34 */
    unsigned long long last_usable  = last_lba - ARRAY_SECS - 1;

    for (int i = 0; i < nparts; i++) {
        if (parts[i].first < first_usable || parts[i].last > last_usable)
            die("a partition falls outside the usable area");
        if (parts[i].first > parts[i].last) die("a partition ends before it starts");
        if (i && parts[i].first <= parts[i - 1].last) die("partitions overlap");
    }

    /* ---- the entry array ---- */
    unsigned char *array = calloc(1, ENTRIES * ENTRY_SIZE);
    if (!array) die("out of memory");
    for (int i = 0; i < nparts; i++) {
        unsigned char *e = array + (size_t)i * ENTRY_SIZE;
        memcpy(e, parts[i].type, 16);
        guid_random(e + 16);
        put64(e + 32, parts[i].first);
        put64(e + 40, parts[i].last);
        put64(e + 48, 0);                                   /* attributes */
        /* The name is UTF-16LE, 36 characters. ASCII in, two bytes out. */
        for (int k = 0; k < 35 && parts[i].name[k]; k++)
            e[56 + k * 2] = (unsigned char)parts[i].name[k];
    }
    unsigned array_crc = crc32(array, ENTRIES * ENTRY_SIZE);

    /* ---- the two headers ----
     * Identical except for MyLBA, AlternateLBA and where the array lives — and
     * each header's CRC is computed with its own CRC field zeroed, so they
     * differ. Copying the primary's checksum into the backup is the classic
     * way to produce a disk that passes every check until the primary is
     * damaged, which is the one moment the backup is read. */
    unsigned char disk_guid[16];
    guid_random(disk_guid);

    for (int backup = 0; backup < 2; backup++) {
        unsigned char h[SECTOR];
        memset(h, 0, sizeof h);
        memcpy(h, "EFI PART", 8);
        put32(h + 8, 0x00010000UL);                /* revision 1.0            */
        put32(h + 12, 92);                         /* header size             */
        put32(h + 16, 0);                          /* CRC, filled in below    */
        put32(h + 20, 0);                          /* reserved, must be zero  */
        put64(h + 24, backup ? last_lba : 1);      /* MyLBA                   */
        put64(h + 32, backup ? 1 : last_lba);      /* AlternateLBA            */
        put64(h + 40, first_usable);
        put64(h + 48, last_usable);
        memcpy(h + 56, disk_guid, 16);
        put64(h + 72, backup ? last_usable + 1 : 2);   /* where its array is  */
        put32(h + 80, ENTRIES);
        put32(h + 84, ENTRY_SIZE);
        put32(h + 88, array_crc);
        put32(h + 16, crc32(h, 92));

        if (backup) {
            write_at(fd, last_usable + 1, array, ENTRIES * ENTRY_SIZE);
            write_at(fd, last_lba, h, SECTOR);
        } else {
            write_at(fd, 2, array, ENTRIES * ENTRY_SIZE);
            write_at(fd, 1, h, SECTOR);
        }
    }
    free(array);

    /* ---- the protective MBR ----
     * One entry of type 0xEE covering the whole disk, so a tool that reads only
     * MBRs sees a disk that is fully occupied by something it does not
     * understand, rather than an empty one it might offer to partition. The
     * size field is clamped: a disk larger than 2 TiB cannot be described in 32
     * bits, and the convention is to write the largest value rather than wrap. */
    unsigned char mbr[SECTOR];
    memset(mbr, 0, sizeof mbr);
    unsigned char *p = mbr + 446;
    p[0] = 0x00;                                   /* not bootable            */
    p[1] = 0x00; p[2] = 0x02; p[3] = 0x00;         /* CHS of LBA 1, legacy    */
    p[4] = 0xEE;                                   /* GPT protective          */
    p[5] = 0xFF; p[6] = 0xFF; p[7] = 0xFF;         /* CHS end, saturated      */
    put32(p + 8, 1);
    put32(p + 12, disk_sectors - 1 > 0xFFFFFFFFULL ? 0xFFFFFFFFUL
                                                   : (unsigned long)(disk_sectors - 1));
    mbr[510] = 0x55; mbr[511] = 0xAA;
    write_at(fd, 0, mbr, SECTOR);

    if (fsync(fd)) die_errno("fsync");
    if (verbose)
        fprintf(stderr, "vpart: %llu sectors, %d partition(s), usable %llu..%llu\n",
                disk_sectors, nparts, first_usable, last_usable);
}

static unsigned long long file_sectors(int fd, const char *path) {
    struct stat st;
    if (fstat(fd, &st)) die_errno(path);
    if (S_ISREG(st.st_mode)) return (unsigned long long)st.st_size / SECTOR;
    off_t end = lseek(fd, 0, SEEK_END);
    if (end < 0) die_errno(path);
    return (unsigned long long)end / SECTOR;
}

int main(int argc, char **argv) {
    const char *path = NULL;
    unsigned long esp_mb = 0, linux_mb = 0;
    int want_linux = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "--esp") && i + 1 < argc) esp_mb = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--linux")) {
            want_linux = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') linux_mb = strtoul(argv[++i], NULL, 10);
        }
        else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: vpart [-v] DISK --esp MB [--linux [MB]]\n");
            return 1;
        }
        else if (!path) path = argv[i];
        else die("only one disk at a time");
    }
    if (!path || !esp_mb) {
        fprintf(stderr, "usage: vpart [-v] DISK --esp MB [--linux [MB]]\n");
        return 1;
    }

    int fd = open(path, O_RDWR);
    if (fd < 0) die_errno(path);
    unsigned long long sectors = file_sectors(fd, path);
    if (sectors < 2048) die("the disk is too small for a GPT");

    /* Start at 1 MiB, which is what every partitioner does and what alignment
     * on any drive with a 4 KiB physical sector wants. */
    part_t parts[2];
    int n = 0;
    unsigned long long esp_first = 2048;
    unsigned long long esp_last  = esp_first + (unsigned long long)esp_mb * 2048 - 1;
    guid(parts[n].type, 0xC12A7328UL, 0xF81F, 0x11D2, ESP_REST);
    parts[n].first = esp_first; parts[n].last = esp_last; parts[n].name = "EFI System Partition";
    n++;

    if (want_linux) {
        unsigned long long first = esp_last + 1;
        unsigned long long last  = linux_mb ? first + (unsigned long long)linux_mb * 2048 - 1
                                            : sectors - ARRAY_SECS - 2;
        guid(parts[n].type, 0x0FC63DAFUL, 0x8483, 0x4772, LINUX_REST);
        parts[n].first = first; parts[n].last = last; parts[n].name = "VerkOS";
        n++;
    }

    write_gpt(fd, sectors, parts, n);
    close(fd);
    return 0;
}
