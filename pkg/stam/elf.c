/* stam — loading a program that was compiled as a program.
 *
 * Only what is needed to run a static executable: check that the file is for
 * this machine, walk the program headers, and put every PT_LOAD segment where
 * it asks to be. Sections, symbols, relocations and dynamic linking are all
 * absent, and none of them are needed to start a static binary — the section
 * table exists for the linker, not the loader.
 *
 * Writing the segment contents is done straight through the user virtual
 * address, after mapping. That works because there is one address space and
 * user pages are mapped AP=01, which permits EL1 as well as EL0. When the
 * kernel moves to its own half of the map that will have to change, and the
 * change will be obvious: this is the only place that relies on it.
 */
#include "elf.h"
#include "vm.h"
#include "mmu.h"
#include "console.h"

#define PT_LOAD 1
#define PF_X    1

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
int elf_load(uint64_t image, uint64_t size, uint64_t *entry_out) {
    const uint8_t *e = (const uint8_t *)(uintptr_t)image;
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

        kprintf("  [elf] segment %llu: %llu bytes at 0x%llx%s%s\n",
                (unsigned long long)i, (unsigned long long)memsz,
                (unsigned long long)vaddr,
                (flags & PF_X) ? ", executable" : "",
                (memsz > filesz) ? ", partly zero-filled" : "");
        loaded++;
    }
    if (!loaded) { kprintf("  [elf] nothing to load — no PT_LOAD segments\n"); return -1; }

    *entry_out = le64(e + 24);
    return 0;
}
