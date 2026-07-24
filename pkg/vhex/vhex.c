/* vhex — VerkOS' own hex viewers. Multi-call: xxd and hexdump.
 *
 *   xxd [FILE]                  canonical xxd dump
 *   hexdump [-C] [FILE]         -C = canonical; default = 16-bit words
 * Reads stdin when no FILE. Installed as xxd/hexdump.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void dump_xxd(FILE *f) {
    unsigned char b[16]; long off = 0; size_t n;
    while ((n = fread(b, 1, 16, f)) > 0) {
        printf("%08lx: ", off);
        for (size_t i = 0; i < 16; i++) {
            if (i < n) printf("%02x", b[i]); else printf("  ");
            if (i % 2) putchar(' ');
        }
        printf(" ");
        for (size_t i = 0; i < n; i++) putchar(b[i] >= 32 && b[i] < 127 ? b[i] : '.');
        putchar('\n'); off += n;
    }
}

static void dump_canon(FILE *f) {
    unsigned char b[16]; long off = 0; size_t n;
    while ((n = fread(b, 1, 16, f)) > 0) {
        printf("%08lx  ", off);
        for (size_t i = 0; i < 16; i++) {
            if (i < n) printf("%02x ", b[i]); else printf("   ");
            if (i == 7) putchar(' ');
        }
        printf(" |");
        for (size_t i = 0; i < n; i++) putchar(b[i] >= 32 && b[i] < 127 ? b[i] : '.');
        printf("|\n"); off += n;
    }
    printf("%08lx\n", off);
}

static void dump_words(FILE *f) {
    unsigned char b[16]; long off = 0; size_t n;
    while ((n = fread(b, 1, 16, f)) > 0) {
        printf("%07lx", off);
        for (size_t i = 0; i < n; i += 2) {
            unsigned w = b[i] | (i + 1 < n ? b[i + 1] << 8 : 0);
            printf(" %04x", w);
        }
        putchar('\n'); off += n;
    }
    printf("%07lx\n", off);
}

int main(int argc, char **argv) {
    const char *me = strrchr(argv[0], '/'); me = me ? me + 1 : argv[0];
    int canon = 0; const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-C")) canon = 1;
        else if (argv[i][0] != '-') file = argv[i];
    }
    FILE *f = file ? fopen(file, "rb") : stdin;
    if (!f) { perror(me); return 1; }
    if (!strcmp(me, "xxd")) dump_xxd(f);
    else if (canon) dump_canon(f);
    else dump_words(f);
    if (file) fclose(f);
    return 0;
}
