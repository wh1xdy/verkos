/* Writing the executable.
 *
 * savc emits the ELF itself: there is no linker under it, and there is
 * nothing in the file it did not decide to put there. Two loadable segments —
 * code with its read-only data, and zeroed memory for the globals — and a
 * note.
 *
 * The note is the part that is not ordinary. It carries the set of system
 * calls this program can possibly make, taken from the capability methods the
 * compiler actually generated. stam reads it when it loads the program and
 * refuses everything else, so `entry fn main(out: Console)` stops being a
 * statement about the source and becomes a fact about the process. A kernel
 * that did not read the note would still run the program correctly; a kernel
 * that does cannot be lied to by a binary that asks for more.
 */
#include "sav.h"

#define BASE_VA   0x400000ull
#define PT_LOAD   1
#define PT_NOTE   4
#define PF_X 1
#define PF_W 2
#define PF_R 4

static void w8(FILE *f, u8 v) { fputc(v, f); }
static void w16(FILE *f, u16 v) { fwrite(&v, 2, 1, f); }
static void w32(FILE *f, u32 v) { fwrite(&v, 4, 1, f); }
static void w64(FILE *f, u64 v) { fwrite(&v, 8, 1, f); }

static void shdr(FILE *f, u32 name, u32 type, u64 flags, u64 addr, u64 off, u64 size, u64 align) {
    w32(f, name); w32(f, type);
    w64(f, flags); w64(f, addr); w64(f, off); w64(f, size);
    w32(f, 0); w32(f, 0);          /* no link, no info: nothing here refers to another section */
    w64(f, align); w64(f, 0);
}

static void phdr(FILE *f, u32 type, u32 flags, u64 off, u64 va, u64 filesz, u64 memsz, u64 align) {
    w32(f, type); w32(f, flags);
    w64(f, off); w64(f, va); w64(f, va);
    w64(f, filesz); w64(f, memsz); w64(f, align);
}

/* The manifest, as the kernel will read it. Fixed fields first so that a
 * reader can find the counts before it has to trust them. */
static size_t build_note(Image *img, u8 **out) {
    size_t names_len = 0;
    for (int i = 0; i < img->ncapnames; i++) names_len += strlen(img->capnames[i]) + 1;
    size_t desc = 16 + (size_t)img->nsyscalls * 4 + names_len;
    size_t desc_pad = (desc + 3) & ~3ull;
    size_t total = 12 + 4 + desc_pad;      /* namesz/descsz/type + "sav\0" + desc */
    u8 *p = xmalloc(total);
    size_t o = 0;
    #define PUT32(v) do { u32 _v = (u32)(v); memcpy(p + o, &_v, 4); o += 4; } while (0)
    PUT32(4);                 /* namesz: "sav\0" */
    PUT32(desc_pad);
    PUT32(1);                 /* note type 1: a sav capability manifest */
    memcpy(p + o, "sav", 4); o += 4;
    PUT32(0x31564153);        /* "SAV1" */
    PUT32(1);                 /* manifest version */
    PUT32(img->nsyscalls);
    PUT32(names_len);
    for (int i = 0; i < img->nsyscalls; i++) PUT32(img->syscalls[i]);
    for (int i = 0; i < img->ncapnames; i++) {
        size_t n = strlen(img->capnames[i]) + 1;
        memcpy(p + o, img->capnames[i], n); o += n;
    }
    while (o < total) p[o++] = 0;
    #undef PUT32
    *out = p;
    return total;
}

void write_elf(Image *img, const char *path) {
    u8 *note; size_t note_size = build_note(img, &note);

    u64 hdr_size  = 64 + 3 * 56;
    u64 text_off  = img->text_vaddr - BASE_VA;          /* where the code starts in the file */
    if (text_off < hdr_size) { fprintf(stderr, "savc: internal: code overlaps the headers\n"); exit(2); }
    u64 text_end  = text_off + img->text.n;
    u64 note_off  = (text_end + 3) & ~3ull;
    u64 seg1_size = note_off + note_size;
    u64 data_off  = (seg1_size + 0xFFF) & ~0xFFFull;
    u64 code_size = img->rodata_off;
    u64 rodata_va = img->text_vaddr + img->rodata_off;
    u64 rodata_sz = img->text.n - img->rodata_off;
    static const char shstr[] = "\0.text\0.rodata\0.note.sav\0.bss\0.shstrtab";
    u64 shstr_off = data_off;                    /* past every loaded byte */
    u64 sh_off    = (shstr_off + sizeof shstr + 7) & ~7ull;

    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "savc: cannot write %s\n", path); exit(2); }

    /* e_ident */
    w8(f, 0x7f); w8(f, 'E'); w8(f, 'L'); w8(f, 'F');
    w8(f, 2);            /* 64-bit */
    w8(f, 1);            /* little-endian */
    w8(f, 1);            /* ELF version */
    w8(f, 0);            /* System V ABI: this program calls no library */
    for (int i = 0; i < 8; i++) w8(f, 0);
    w16(f, 2);           /* ET_EXEC: a fixed address, no interpreter, no relocation */
    w16(f, 183);         /* aarch64 */
    w32(f, 1);
    w64(f, img->entry);
    w64(f, 64);          /* program headers follow the header */
    w64(f, sh_off);      /* a section table, for readers: objdump, readelf, a
                          * person checking that there are no relocations. No
                          * loader needs it, and stam never looks at it. */
    w32(f, 0);
    w16(f, 64); w16(f, 56); w16(f, 3);
    w16(f, 64); w16(f, 6); w16(f, 5);

    phdr(f, PT_LOAD, PF_R | PF_X, 0, BASE_VA, seg1_size, seg1_size, 0x1000);
    phdr(f, PT_LOAD, PF_R | PF_W, data_off, img->data_vaddr, 0, img->bss_size, 0x1000);
    phdr(f, PT_NOTE, PF_R, note_off, BASE_VA + note_off, note_size, note_size, 4);

    while ((u64)ftell(f) < text_off) w8(f, 0);
    fwrite(img->text.p, 1, img->text.n, f);
    while ((u64)ftell(f) < note_off) w8(f, 0);
    fwrite(note, 1, note_size, f);

    while ((u64)ftell(f) < shstr_off) w8(f, 0);
    fwrite(shstr, 1, sizeof shstr, f);
    while ((u64)ftell(f) < sh_off) w8(f, 0);
    shdr(f, 0, 0, 0, 0, 0, 0, 0);                                     /* SHN_UNDEF */
    shdr(f, 1, 1, 0x6, img->text_vaddr, text_off, code_size, 4);      /* .text     AX */
    shdr(f, 7, 1, 0x2, rodata_va, text_off + code_size, rodata_sz, 8);/* .rodata   A  */
    shdr(f, 15, 7, 0x2, BASE_VA + note_off, note_off, note_size, 4);  /* .note.sav A  */
    shdr(f, 25, 8, 0x3, img->data_vaddr, data_off, img->bss_size, 8); /* .bss      WA */
    shdr(f, 30, 3, 0, 0, shstr_off, sizeof shstr, 1);                 /* .shstrtab    */
    fclose(f);

    char cmd[512];
    snprintf(cmd, sizeof cmd, "chmod +x '%s'", path);
    if (system(cmd)) { /* the file is written either way; a mode is not worth failing over */ }
}
