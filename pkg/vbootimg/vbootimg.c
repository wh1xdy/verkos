/* vbootimg — write and read the Android boot image format.
 *
 * Qualcomm platforms do not boot from an EFI system partition. The chain is
 * PBL → XBL → ABL, and ABL takes a "boot image": a small header followed by a
 * kernel and an optional ramdisk, each starting on a page boundary. It is the
 * only way onto such a board, which is why VerkOS needs to be able to write
 * one — the Arduino UNO Q is the immediate reason, but every Qualcomm board
 * works the same way.
 *
 * Two commands: `create` makes an image, `show` reads one back. `show` exists
 * so the writer can be checked against a reader that does not share its
 * assumptions, which is the only kind of self-check worth having for a format
 * we cannot yet test on the hardware that consumes it.
 *
 * NOTE ON FLASHING, and it matters: use `fastboot boot image.img`, which loads
 * the image and runs it without writing anything, so a power cycle undoes it.
 * `fastboot flash boot image.img` overwrites the board's boot partition and can
 * leave it unable to start.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#define MAGIC "ANDROID!"
#define HDR_BYTES 1632          /* v0/v1/v2 header, before padding to a page */

static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("vbootimg: ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap); exit(1);
}

static void put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static unsigned char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot read %s", path);
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len < 0) die("cannot size %s", path);
    unsigned char *b = malloc((size_t)len ? (size_t)len : 1);
    if (!b) die("out of memory");
    if (len && fread(b, 1, (size_t)len, f) != (size_t)len) die("short read on %s", path);
    fclose(f);
    *n = (size_t)len;
    return b;
}

static void pad_to(FILE *out, size_t written, size_t page) {
    size_t r = written % page;
    if (!r) return;
    for (size_t i = r; i < page; i++) fputc(0, out);
}

static int cmd_create(int argc, char **argv) {
    const char *kernel = 0, *ramdisk = 0, *outpath = 0, *cmdline = "";
    unsigned long base = 0x80000000ul, page = 4096;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)              outpath = argv[++i];
        else if (!strcmp(argv[i], "--kernel") && i + 1 < argc)   kernel  = argv[++i];
        else if (!strcmp(argv[i], "--ramdisk") && i + 1 < argc)  ramdisk = argv[++i];
        else if (!strcmp(argv[i], "--cmdline") && i + 1 < argc)  cmdline = argv[++i];
        else if (!strcmp(argv[i], "--base") && i + 1 < argc)     base = strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--pagesize") && i + 1 < argc) page = strtoul(argv[++i], 0, 0);
        else die("unknown option '%s'", argv[i]);
    }
    if (!kernel || !outpath) die("usage: vbootimg create -o OUT --kernel K [--ramdisk R] [--cmdline C] [--base ADDR] [--pagesize N]");
    if (page < HDR_BYTES) die("page size %lu is smaller than the header", page);
    if (strlen(cmdline) > 511) die("command line is longer than the 512 bytes the format allows");

    size_t klen = 0, rlen = 0;
    unsigned char *kbuf = slurp(kernel, &klen);
    unsigned char *rbuf = ramdisk ? slurp(ramdisk, &rlen) : 0;
    if (!klen) die("%s is empty", kernel);

    unsigned char hdr[HDR_BYTES];
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, MAGIC, 8);
    put32(hdr + 8,  (uint32_t)klen);
    /* The offsets from the load base are the ones every Android bootloader
     * has used since the format was defined. They are conventions, not part
     * of the header, which is why they are written down here. */
    put32(hdr + 12, (uint32_t)(base + 0x00008000));   /* kernel  */
    put32(hdr + 16, (uint32_t)rlen);
    put32(hdr + 20, (uint32_t)(base + 0x01000000));   /* ramdisk */
    put32(hdr + 24, 0);                               /* second stage: none */
    put32(hdr + 28, (uint32_t)(base + 0x00f00000));
    put32(hdr + 32, (uint32_t)(base + 0x00000100));   /* tags    */
    put32(hdr + 36, (uint32_t)page);
    put32(hdr + 40, 0);                               /* header version 0 */
    put32(hdr + 44, 0);                               /* os version       */
    snprintf((char *)hdr + 48, 16, "VerkOS");
    memcpy(hdr + 64, cmdline, strlen(cmdline));

    FILE *out = fopen(outpath, "wb");
    if (!out) die("cannot write %s", outpath);
    fwrite(hdr, 1, sizeof hdr, out);  pad_to(out, sizeof hdr, page);
    fwrite(kbuf, 1, klen, out);       pad_to(out, klen, page);
    if (rlen) { fwrite(rbuf, 1, rlen, out); pad_to(out, rlen, page); }
    if (fclose(out)) die("could not finish writing %s", outpath);

    printf("vbootimg: %s\n", outpath);
    printf("  kernel   %zu bytes at 0x%lx\n", klen, base + 0x00008000);
    if (rlen) printf("  ramdisk  %zu bytes at 0x%lx\n", rlen, base + 0x01000000);
    printf("  cmdline  %s\n", cmdline[0] ? cmdline : "(empty)");
    printf("\n  Boot it without writing anything:  fastboot boot %s\n", outpath);
    printf("  `fastboot flash boot` overwrites the board and can brick it.\n");
    return 0;
}

static int cmd_show(int argc, char **argv) {
    if (argc != 1) die("usage: vbootimg show IMAGE");
    size_t n = 0;
    unsigned char *b = slurp(argv[0], &n);
    if (n < HDR_BYTES) die("%s is too small to be a boot image", argv[0]);
    if (memcmp(b, MAGIC, 8)) die("%s does not begin with ANDROID!", argv[0]);

    uint32_t klen = get32(b + 8),  kaddr = get32(b + 12);
    uint32_t rlen = get32(b + 16), raddr = get32(b + 20);
    uint32_t page = get32(b + 36), ver   = get32(b + 40);
    if (!page) die("the header claims a page size of zero");

    size_t need = page + ((klen + page - 1) / page) * (size_t)page;
    if (rlen) need += ((rlen + page - 1) / page) * (size_t)page;
    printf("name      %.16s\n", b + 48);
    printf("version   %u\n", ver);
    printf("pagesize  %u\n", page);
    printf("kernel    %u bytes at 0x%x\n", klen, kaddr);
    printf("ramdisk   %u bytes at 0x%x\n", rlen, raddr);
    printf("cmdline   %.512s\n", b + 64);
    printf("size      %zu bytes on disk, %zu expected\n", n, need);
    if (n < need) die("the file is shorter than its own header says it is");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fputs("vbootimg — write and read Android boot images\n\n"
              "  vbootimg create -o OUT --kernel K [--ramdisk R] [--cmdline C]\n"
              "                  [--base ADDR] [--pagesize N]\n"
              "  vbootimg show IMAGE\n", stderr);
        return 1;
    }
    if (!strcmp(argv[1], "create")) return cmd_create(argc - 2, argv + 2);
    if (!strcmp(argv[1], "show"))   return cmd_show(argc - 2, argv + 2);
    die("unknown command '%s'", argv[1]);
    return 1;
}
