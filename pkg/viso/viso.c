/* viso — VerkOS' own ISO 9660 writer, for making a bootable live medium.
 *
 * Deliberately tiny. The live ISO carries three flat files and nothing else:
 * the EFI boot image, the root filesystem image, and the boot catalogue. That
 * removes everything that makes ISO 9660 writers large and error-prone —
 * Rock Ridge, Joliet, multi-level directories, multi-extent files — and leaves
 * a structure smaller than the FAT32 we already write in pkg/vmkfs.
 *
 *   viso [-V VOLID] [-e ESP.IMG] [-v] -o OUT.iso FILE...
 *
 * The one place a mistake here is silent is the El Torito validation entry's
 * checksum: get it wrong and firmware declines to boot with no diagnostic at
 * all. So the judge is deliberately not us — scripts/test.sh checks what this
 * writes with xorriso, the way vmkfs is checked with fsck.fat.
 *
 * Layout (2048-byte sectors):
 *   0-15   system area, zeroed
 *   16     primary volume descriptor
 *   17     boot record volume descriptor (El Torito)
 *   18     volume descriptor set terminator
 *   19     El Torito boot catalogue
 *   20     L path table
 *   21     M path table
 *   22     root directory
 *   23+    file extents, each starting on a sector boundary
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>

#define SECTOR      2048
#define SYSTEM_AREA 16
#define PVD_LBA     16
#define BRVD_LBA    17
#define TERM_LBA    18
#define CATALOG_LBA 19
#define LPATH_LBA   20
#define MPATH_LBA   21
#define ROOT_LBA    22
#define FIRST_FILE  23
#define MAX_FILES   16

static int verbose;

static void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("viso: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

/* ISO 9660 stores most numbers twice, once in each byte order, so a reader of
 * either persuasion can use the field it understands. */
static void le16(unsigned char *p, unsigned v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void be16(unsigned char *p, unsigned v) { p[0] = (v >> 8) & 0xff; p[1] = v & 0xff; }
static void le32(unsigned char *p, unsigned long v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}
static void be32(unsigned char *p, unsigned long v) {
    p[0] = (v >> 24) & 0xff; p[1] = (v >> 16) & 0xff; p[2] = (v >> 8) & 0xff; p[3] = v & 0xff;
}
static void both16(unsigned char *p, unsigned v)      { le16(p, v); be16(p + 2, v); }
static void both32(unsigned char *p, unsigned long v) { le32(p, v); be32(p + 4, v); }

/* d-characters: A-Z, 0-9 and underscore. Anything else becomes an underscore
 * rather than being passed through, because a reader is entitled to reject it. */
static void dchars(unsigned char *dst, size_t n, const char *src) {
    memset(dst, ' ', n);
    for (size_t i = 0; i < n && src[i]; i++) {
        int c = toupper((unsigned char)src[i]);
        dst[i] = (char)((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? c : '_');
    }
}

/* "YYYYMMDDHHMMSScc" plus a timezone byte, which is how a volume descriptor
 * spells a date. */
static void dec_datetime(unsigned char *p, time_t t) {
    struct tm tm;
    gmtime_r(&t, &tm);
    char b[17];
    snprintf(b, sizeof b, "%04d%02d%02d%02d%02d%02d00",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    memcpy(p, b, 16);
    p[16] = 0;                      /* GMT */
}
/* directory records use a shorter, binary form */
static void dir_datetime(unsigned char *p, time_t t) {
    struct tm tm;
    gmtime_r(&t, &tm);
    p[0] = (unsigned char)(tm.tm_year);
    p[1] = (unsigned char)(tm.tm_mon + 1);
    p[2] = (unsigned char)tm.tm_mday;
    p[3] = (unsigned char)tm.tm_hour;
    p[4] = (unsigned char)tm.tm_min;
    p[5] = (unsigned char)tm.tm_sec;
    p[6] = 0;
}

struct entry {
    char path[512];
    char name[32];              /* the 8.3-ish name as it appears on the ISO */
    unsigned long lba, size, sectors;
    int is_boot;                /* this one is the El Torito image */
};

static unsigned long sectors_for(unsigned long bytes) {
    return (bytes + SECTOR - 1) / SECTOR;
}

static void write_at(int fd, unsigned long lba, const void *buf, size_t len) {
    if (pwrite(fd, buf, len, (off_t)lba * SECTOR) != (ssize_t)len)
        die("write at LBA %lu: %s", lba, strerror(errno));
}

/* A directory record. ISO 9660 names carry a ";1" version suffix; the two
 * special records for "." and ".." use a single 0x00 / 0x01 byte instead. */
static size_t dir_record(unsigned char *p, unsigned long lba, unsigned long size,
                         const char *name, int is_dir, time_t when) {
    size_t namelen = name ? strlen(name) : 1;
    size_t len = 33 + namelen;
    if (len & 1) len++;                     /* records are even-length */
    memset(p, 0, len);
    p[0] = (unsigned char)len;
    p[1] = 0;                               /* no extended attributes */
    both32(p + 2, lba);
    both32(p + 10, size);
    dir_datetime(p + 18, when);
    p[25] = is_dir ? 0x02 : 0x00;           /* file flags: bit 1 = directory */
    p[26] = 0;                              /* not interleaved */
    p[27] = 0;
    both16(p + 28, 1);                      /* volume sequence number */
    p[32] = (unsigned char)namelen;
    if (name) memcpy(p + 33, name, namelen);
    else p[33] = 0;
    return len;
}

int main(int argc, char **argv) {
    const char *out = NULL, *volid = "VERKLIVE", *esp = NULL;
    struct entry files[MAX_FILES];
    int nfiles = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-V") && i + 1 < argc) volid = argv[++i];
        else if (!strcmp(argv[i], "-e") && i + 1 < argc) esp = argv[++i];
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "usage: viso [-V VOLID] [-e ESP.IMG] [-v] -o OUT.iso FILE...\n");
            return 1;
        }
        else {
            if (nfiles >= MAX_FILES) die("too many files (max %d)", MAX_FILES);
            snprintf(files[nfiles].path, sizeof files[nfiles].path, "%s", argv[i]);
            nfiles++;
        }
    }
    if (!out) die("no output given (-o OUT.iso)");
    if (!nfiles) die("no files to put on the image");

    /* The boot image has to be one of the files, so the catalogue can point at
     * an extent that is really there. */
    int boot_index = -1;
    if (esp) {
        for (int i = 0; i < nfiles; i++) {
            if (!strcmp(files[i].path, esp)) { boot_index = i; break; }
        }
        if (boot_index < 0) {
            if (nfiles >= MAX_FILES) die("too many files (max %d)", MAX_FILES);
            snprintf(files[nfiles].path, sizeof files[nfiles].path, "%s", esp);
            boot_index = nfiles++;
        }
    }

    time_t now = time(NULL);

    /* Assign names, sizes and extents. */
    unsigned long lba = FIRST_FILE;
    for (int i = 0; i < nfiles; i++) {
        struct stat st;
        if (stat(files[i].path, &st)) die("%s: %s", files[i].path, strerror(errno));
        if (!S_ISREG(st.st_mode)) die("%s is not a regular file", files[i].path);
        const char *base = strrchr(files[i].path, '/');
        base = base ? base + 1 : files[i].path;

        /* Uppercase 8.3 with the version suffix, which is what a reader without
         * Rock Ridge expects — and what the kernel maps back to lower case. */
        char stem[64] = "", ext[16] = "";
        const char *dot = strrchr(base, '.');
        size_t slen = dot ? (size_t)(dot - base) : strlen(base);
        if (slen > 8) slen = 8;
        for (size_t k = 0; k < slen; k++) stem[k] = (char)toupper((unsigned char)base[k]);
        if (dot) for (int k = 0; k < 3 && dot[1 + k]; k++) ext[k] = (char)toupper((unsigned char)dot[1 + k]);
        snprintf(files[i].name, sizeof files[i].name, "%s.%s;1", stem, ext[0] ? ext : "");

        files[i].size = (unsigned long)st.st_size;
        files[i].sectors = sectors_for(files[i].size);
        files[i].lba = lba;
        files[i].is_boot = (i == boot_index);
        lba += files[i].sectors;
        if (verbose)
            fprintf(stderr, "  %-14s LBA %-7lu %lu sectors (%lu bytes)%s\n",
                    files[i].name, files[i].lba, files[i].sectors, files[i].size,
                    files[i].is_boot ? "  [boot image]" : "");
    }
    unsigned long total_sectors = lba;

    int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) die("%s: %s", out, strerror(errno));

    /* ---- system area: 16 zeroed sectors ---- */
    unsigned char zero[SECTOR];
    memset(zero, 0, sizeof zero);
    for (unsigned long s = 0; s < SYSTEM_AREA; s++) write_at(fd, s, zero, SECTOR);

    /* ---- the root directory, one sector: ".", ".." and the files ---- */
    unsigned char root[SECTOR];
    memset(root, 0, sizeof root);
    size_t off = 0;
    off += dir_record(root + off, ROOT_LBA, SECTOR, NULL, 1, now);          /* "." */
    {   /* ".." — same record with a 0x01 name */
        size_t l = dir_record(root + off, ROOT_LBA, SECTOR, NULL, 1, now);
        root[off + 33] = 1;
        off += l;
    }
    for (int i = 0; i < nfiles; i++)
        off += dir_record(root + off, files[i].lba, files[i].size, files[i].name, 0, now);
    if (off > SECTOR) die("the root directory does not fit in one sector");
    write_at(fd, ROOT_LBA, root, SECTOR);

    /* ---- path tables: one entry, the root ---- */
    unsigned char lp[SECTOR], mp[SECTOR];
    memset(lp, 0, sizeof lp); memset(mp, 0, sizeof mp);
    lp[0] = 1;  lp[1] = 0;  le32(lp + 2, ROOT_LBA);  le16(lp + 6, 1);  lp[8] = 0; lp[9] = 0;
    mp[0] = 1;  mp[1] = 0;  be32(mp + 2, ROOT_LBA);  be16(mp + 6, 1);  mp[8] = 0; mp[9] = 0;
    unsigned long path_table_size = 10;
    write_at(fd, LPATH_LBA, lp, SECTOR);
    write_at(fd, MPATH_LBA, mp, SECTOR);

    /* ---- primary volume descriptor ---- */
    unsigned char pvd[SECTOR];
    memset(pvd, 0, sizeof pvd);
    pvd[0] = 1;                                   /* primary */
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;                                   /* version */
    memset(pvd + 8, ' ', 32);                     /* system identifier */
    dchars(pvd + 40, 32, volid);                  /* volume identifier */
    both32(pvd + 80, total_sectors);
    both16(pvd + 120, 1);                         /* volume set size */
    both16(pvd + 124, 1);                         /* volume sequence number */
    both16(pvd + 128, SECTOR);                    /* logical block size */
    both32(pvd + 132, path_table_size);
    le32(pvd + 140, LPATH_LBA);
    le32(pvd + 144, 0);                           /* no optional L table */
    be32(pvd + 148, MPATH_LBA);
    be32(pvd + 152, 0);                           /* no optional M table */
    dir_record(pvd + 156, ROOT_LBA, SECTOR, NULL, 1, now);   /* root directory record */
    memset(pvd + 190, ' ', 128 + 128 + 128 + 37 + 37 + 37);  /* the identifier fields */
    dec_datetime(pvd + 813, now);                 /* volume creation */
    dec_datetime(pvd + 830, now);                 /* modification */
    memset(pvd + 847, '0', 16); pvd[863] = 0;     /* expiration: none */
    memset(pvd + 864, '0', 16); pvd[880] = 0;     /* effective: none */
    pvd[881] = 1;                                 /* file structure version */
    write_at(fd, PVD_LBA, pvd, SECTOR);

    /* ---- boot record volume descriptor + El Torito catalogue ---- */
    if (esp) {
        unsigned char brvd[SECTOR];
        memset(brvd, 0, sizeof brvd);
        brvd[0] = 0;                               /* boot record */
        memcpy(brvd + 1, "CD001", 5);
        brvd[6] = 1;
        memcpy(brvd + 7, "EL TORITO SPECIFICATION", 23);
        le32(brvd + 71, CATALOG_LBA);
        write_at(fd, BRVD_LBA, brvd, SECTOR);

        unsigned char cat[SECTOR];
        memset(cat, 0, sizeof cat);
        /* validation entry */
        cat[0] = 0x01;                             /* header id */
        cat[1] = 0xEF;                             /* platform: UEFI */
        memcpy(cat + 4, "VerkOS", 6);              /* id string */
        cat[30] = 0x55; cat[31] = 0xAA;            /* key bytes */
        /* The checksum makes all sixteen little-endian 16-bit words of this
         * entry sum to zero. Firmware that dislikes it simply does not boot,
         * and says nothing, which is why test.sh has xorriso check it. */
        {
            unsigned sum = 0;
            for (int k = 0; k < 32; k += 2) sum += (unsigned)cat[k] | ((unsigned)cat[k + 1] << 8);
            le16(cat + 28, (unsigned)((0x10000 - (sum & 0xffff)) & 0xffff));
        }
        /* default (initial/default) entry */
        unsigned long bsize = files[boot_index].size;
        unsigned long b512 = (bsize + 511) / 512;
        cat[32] = 0x88;                            /* bootable */
        cat[33] = 0x00;                            /* no emulation */
        le16(cat + 34, 0);                         /* load segment: firmware default */
        cat[36] = 0;                               /* system type: unused for EFI */
        /* The sector count is 16 bits and an ESP of any useful size does not
         * fit. Firmware ignores it for no-emulation EFI entries and reads the
         * whole file, but implementations disagree on what to write — so match
         * xorriso, whose ISOs boot on the widest range of firmware: it writes
         * 0 rather than a truncated count. (Checked against an ISO xorriso
         * built for the same input.) */
        le16(cat + 38, b512 > 0xffff ? 0 : (unsigned)b512);
        le32(cat + 40, files[boot_index].lba);
        write_at(fd, CATALOG_LBA, cat, SECTOR);
        if (verbose)
            fprintf(stderr, "  boot catalogue at LBA %d, image at LBA %lu (%lu x 512-byte sectors)\n",
                    CATALOG_LBA, files[boot_index].lba, b512);
    } else {
        /* Without a boot image the descriptor slot still has to be something a
         * reader can skip over. */
        write_at(fd, BRVD_LBA, zero, SECTOR);
    }

    /* ---- volume descriptor set terminator ---- */
    unsigned char term[SECTOR];
    memset(term, 0, sizeof term);
    term[0] = 0xFF;
    memcpy(term + 1, "CD001", 5);
    term[6] = 1;
    write_at(fd, TERM_LBA, term, SECTOR);

    /* ---- the file contents ---- */
    for (int i = 0; i < nfiles; i++) {
        int in = open(files[i].path, O_RDONLY);
        if (in < 0) die("%s: %s", files[i].path, strerror(errno));
        static unsigned char buf[SECTOR * 32];
        off_t pos = (off_t)files[i].lba * SECTOR;
        ssize_t n;
        while ((n = read(in, buf, sizeof buf)) > 0) {
            if (pwrite(fd, buf, (size_t)n, pos) != n) die("writing %s: %s", files[i].name, strerror(errno));
            pos += n;
        }
        if (n < 0) die("reading %s: %s", files[i].path, strerror(errno));
        close(in);
    }
    /* Make the file exactly total_sectors long, padding whatever the last
     * extent did not fill. */
    if (ftruncate(fd, (off_t)total_sectors * SECTOR)) die("ftruncate: %s", strerror(errno));

    if (fsync(fd)) die("fsync: %s", strerror(errno));
    close(fd);

    if (verbose)
        fprintf(stderr, "viso: %s — %lu sectors (%lu MiB), volume %s\n",
                out, total_sectors, total_sectors * SECTOR / (1024 * 1024), volid);
    return 0;
}
