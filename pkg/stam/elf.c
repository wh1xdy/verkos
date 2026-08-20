/* stam — loading a program that was compiled as a program.
 *
 * Only what is needed to run a static executable: check that the file is for
 * this machine, walk the program headers, and put every PT_LOAD segment where
 * it asks to be. Sections, symbols, relocations and dynamic linking are all
 * absent, and none of them are needed to start a static binary — the section
 * table exists for the linker, not the loader.
 *
 * Writing the segment contents is done straight through the user virtual
 * address, after mapping. That works because the kernel loads a program into
 * the address space it is itself running with — TTBR0 has already been
 * switched to the new process — and user pages are mapped AP=01, which permits
 * EL1 as well as EL0. Loading into some *other* process's space would need a
 * temporary mapping instead; nothing does that yet.
 */
#include "elf.h"
#include "vm.h"
#include "mmu.h"
#include "console.h"

#define PT_LOAD 1
#define PT_NOTE 4
#define PF_X    1
#define SAV_NOTE_TYPE 1
#define SAV_MAGIC 0x31564153u          /* "SAV1", little-endian */

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t le64(const uint8_t *p) {
    uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i); return v;
}

/* Every rejection says which check failed. "not a valid executable" sends
 * someone reading the loader; "built for a different machine" sends them to
 * the compiler, which is where the problem is. */
int sav_allows(const sav_manifest_t *m, uint64_t nr) {
    if (!m->present) return 1;
    for (unsigned i = 0; i < m->ncalls; i++) if (m->call[i] == (uint32_t)nr) return 1;
    return 0;
}

/* Read the manifest out of a PT_NOTE, if there is one. Everything is bounded
 * by the sizes the file itself declares and then by the size of the file: a
 * note that says it is longer than the image is a broken image, not a reason
 * to read past the end of it. */
static void read_sav_note(const uint8_t *e, uint64_t size, uint64_t off, uint64_t len,
                          sav_manifest_t *out) {
    if (!out || off + len > size || len < 16) return;
    const uint8_t *n = e + off;
    uint32_t namesz = le32(n), descsz = le32(n + 4), type = le32(n + 8);
    uint64_t name_at = 12, desc_at = 12 + ((namesz + 3) & ~3u);
    if (desc_at + descsz > len) return;
    if (type != SAV_NOTE_TYPE || namesz != 4) return;
    if (!(n[name_at] == 's' && n[name_at+1] == 'a' && n[name_at+2] == 'v' && n[name_at+3] == 0)) return;

    const uint8_t *d = n + desc_at;
    if (descsz < 16 || le32(d) != SAV_MAGIC || le32(d + 4) != 1) return;
    uint32_t ncalls = le32(d + 8), nameslen = le32(d + 12);
    if (16 + (uint64_t)ncalls * 4 + nameslen > descsz) return;
    if (ncalls > SAV_MAX_CALLS) return;

    out->present = 1;
    out->ncalls = ncalls;
    for (uint32_t i = 0; i < ncalls; i++) out->call[i] = le32(d + 16 + i * 4);
    /* The names arrive with a NUL between each; the console wants one line,
     * so they are joined with a space and the trailing one is dropped. */
    uint32_t k = 0;
    for (; k < nameslen && k < SAV_CAPS_BYTES - 1; k++) {
        char ch = (char)d[16 + ncalls * 4 + k];
        out->caps[k] = ch ? ch : ' ';
    }
    while (k && out->caps[k - 1] == ' ') k--;
    out->caps[k] = 0;
}

int elf_load(uint64_t image, uint64_t size, uint64_t *entry_out, sav_manifest_t *manifest) {
    const uint8_t *e = (const uint8_t *)(uintptr_t)image;
    if (manifest) { manifest->present = 0; manifest->ncalls = 0; manifest->caps[0] = 0; }
    if (size < 64) { kprintf("  [elf] too small to be an executable\n"); return -1; }
    if (!(e[0] == 0x7f && e[1] == 'E' && e[2] == 'L' && e[3] == 'F')) {
        kprintf("  [elf] no ELF magic — this is not an executable\n"); return -1; }
    if (e[4] != 2) { kprintf("  [elf] not 64-bit\n"); return -1; }
    if (e[5] != 1) { kprintf("  [elf] not little-endian\n"); return -1; }
    if (le16(e + 18) != 183) {
        kprintf("  [elf] built for machine %llu, not aarch64\n",
                (unsigned long long)le16(e + 18)); return -1; }

    uint64_t phoff = le64(e + 32);
    uint16_t phentsize = le16(e + 54), phnum = le16(e + 56);
    if (!phnum || phoff + (uint64_t)phnum * phentsize > size) {
        kprintf("  [elf] the program header table is outside the file\n"); return -1; }

    unsigned loaded = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *ph = e + phoff + (uint64_t)i * phentsize;
        if (le32(ph) == PT_NOTE) { read_sav_note(e, size, le64(ph + 8), le64(ph + 32), manifest); continue; }
        if (le32(ph) != PT_LOAD) continue;
        uint32_t flags  = le32(ph + 4);
        uint64_t offset = le64(ph + 8);
        uint64_t vaddr  = le64(ph + 16);
        uint64_t filesz = le64(ph + 32);
        uint64_t memsz  = le64(ph + 40);
        if (offset + filesz > size) {
            kprintf("  [elf] segment %llu runs past the end of the file\n",
                    (unsigned long long)i); return -1; }
        if (memsz < filesz) {
            kprintf("  [elf] segment %llu claims less memory than it has data\n",
                    (unsigned long long)i); return -1; }

        uint64_t first = vaddr & ~(PAGE_SIZE - 1);
        uint64_t last  = (vaddr + memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        for (uint64_t va = first; va < last; va += PAGE_SIZE) {
            void *page = alloc_page();          /* zeroed, which is what bss needs */
            if (!page) { kprintf("  [elf] out of memory\n"); return -1; }
            if (map_user_page(va, (uint64_t)(uintptr_t)page, (flags & PF_X) != 0)) {
                kprintf("  [elf] could not map 0x%llx\n", (unsigned long long)va);
                return -1;
            }
        }
        uint8_t *dst = (uint8_t *)(uintptr_t)vaddr;
        const uint8_t *src = e + offset;
        for (uint64_t b = 0; b < filesz; b++) dst[b] = src[b];
        /* The rest of memsz is already zero: alloc_page clears each page, and
         * that is exactly what .bss requires. */

        if (kchatty()) kprintf("  [elf] %llu bytes at 0x%llx%s%s\n",
                (unsigned long long)memsz, (unsigned long long)vaddr,
                (flags & PF_X) ? ", executable" : "",
                (memsz > filesz) ? ", partly zero-filled" : "");
        loaded++;
    }
    if (!loaded) { kprintf("  [elf] nothing to load — no PT_LOAD segments\n"); return -1; }

    if (manifest && manifest->present)
        kprintf("  [sav] a sav program: %s, and %u system call%s it may make\n",
                manifest->caps[0] ? manifest->caps : "no capabilities",
                manifest->ncalls, manifest->ncalls == 1 ? "" : "s");

    *entry_out = le64(e + 24);
    return 0;
}
