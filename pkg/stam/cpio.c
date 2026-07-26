/* stam — finding a file in the initrd.
 *
 * The initrd is a cpio archive in the "newc" format, which is what every Linux
 * initramfs is and what VerkOS' own vcpio already writes. That choice is the
 * same one as ELF and the device tree: adopt the format everyone speaks, own
 * the implementation behind it.
 *
 * This is not a filesystem. There is no directory structure, nothing can be
 * written, and finding a file is a linear walk from the beginning. It is
 * exactly enough for a kernel to find the first program it should run, which is
 * all an initramfs has ever been for.
 */
#include "cpio.h"
#include "console.h"

#define HDR 110                  /* the newc header, in ASCII hex */

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Every field is eight ASCII hex digits. A field that is not is a corrupt
 * archive, and reading on would walk off into memory that is not ours. */
static int field(const unsigned char *p, uint64_t *out) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        int d = hexval((char)p[i]);
        if (d < 0) return -1;
        v = (v << 4) | (uint64_t)d;
    }
    *out = v;
    return 0;
}

static uint64_t round4(uint64_t n) { return (n + 3) & ~3ull; }

static int same(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Names in an archive are usually relative ("init", "bin/sh") and a program
 * asking for one will write it with a leading slash. Accept either. */
static const char *skip_slash(const char *s) { return s[0] == '/' ? s + 1 : s; }

int cpio_find(uint64_t image, uint64_t size, const char *want,
              uint64_t *addr_out, uint64_t *size_out) {
    const unsigned char *base = (const unsigned char *)(uintptr_t)image;
    uint64_t off = 0;
    want = skip_slash(want);

    while (off + HDR <= size) {
        const unsigned char *h = base + off;
        if (h[0] != '0' || h[1] != '7' || h[2] != '0' || h[3] != '7' ||
            h[4] != '0' || h[5] != '1') return -1;      /* not newc */

        uint64_t filesize = 0, namesize = 0;
        if (field(h + 54, &filesize) || field(h + 94, &namesize)) return -1;
        if (!namesize || off + HDR + namesize > size) return -1;

        const char *name = (const char *)(h + HDR);
        uint64_t data = off + round4(HDR + namesize);
        if (data + filesize > size) return -1;

        if (same(name, "TRAILER!!!")) return -1;        /* end of archive */
        if (same(skip_slash(name), want)) {
            *addr_out = image + data;
            *size_out = filesize;
            return 0;
        }
        off = data + round4(filesize);
    }
    return -1;
}

/* List what is in there. Used once at boot: a kernel that cannot find /init
 * should say what it *did* find, or the next hour is spent guessing at the
 * name rather than at the problem. */
void cpio_list(uint64_t image, uint64_t size) {
    const unsigned char *base = (const unsigned char *)(uintptr_t)image;
    uint64_t off = 0;
    unsigned n = 0;
    while (off + HDR <= size) {
        const unsigned char *h = base + off;
        if (h[0] != '0' || h[1] != '7') break;
        uint64_t filesize = 0, namesize = 0;
        if (field(h + 54, &filesize) || field(h + 94, &namesize)) break;
        if (!namesize || off + HDR + namesize > size) break;
        const char *name = (const char *)(h + HDR);
        if (same(name, "TRAILER!!!")) break;
        kprintf("            %s (%llu bytes)\n", name, (unsigned long long)filesize);
        n++;
        off = off + round4(HDR + namesize) + round4(filesize);
    }
    if (!n) kprintf("            (nothing readable)\n");
}
