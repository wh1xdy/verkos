/* stam — the UEFI handover shim.
 *
 * The second way into the same kernel. handover_dtb.c is used when a loader
 * hands us a device tree in x0; this one is used when firmware starts us as an
 * EFI application and we have to go and find the tree ourselves.
 *
 * Both end in kmain() with a DTB pointer, and nothing above this line knows
 * which of the two happened. That is the whole design: a boot protocol is a
 * file, not a change to the kernel.
 *
 * On aarch64 the firmware already has a device tree and publishes it in the
 * configuration table under EFI_DTB_GUID. On x86_64 nothing does — and that is
 * where vboot will earn its place, by building a tree from the UEFI memory map
 * and the ACPI pointer and installing it under the same GUID, so this file
 * finds it in exactly the same way and the kernel never learns the difference.
 */
#include "../vboot/efi.h"
#include "bootinfo.h"

extern void kmain(void *dtb);

/* The GUID under which a device tree is published. Same on every
 * architecture — that is what makes the x86 plan work. */
static EFI_GUID DTB_GUID =
    { 0xb1b621d5, 0xf19c, 0x41a5, { 0x83,0x0b,0xd9,0x15,0x2c,0x69,0xaa,0xe0 } };

typedef struct { EFI_GUID VendorGuid; void *VendorTable; } config_entry_t;
typedef EFI_STATUS (EFIAPI *get_mmap_t)(UINTN *, void *, UINTN *, UINTN *, UINT32 *);

static EFI_SYSTEM_TABLE *ST;

static int guid_eq(const EFI_GUID *a, const EFI_GUID *b) {
    if (a->Data1 != b->Data1 || a->Data2 != b->Data2 || a->Data3 != b->Data3) return 0;
    for (int i = 0; i < 8; i++) if (a->Data4[i] != b->Data4[i]) return 0;
    return 1;
}

static void say(const CHAR16 *s) { if (ST && ST->ConOut) ST->ConOut->OutputString(ST->ConOut, (CHAR16 *)s); }

/* The memory map is fetched into a fixed buffer rather than an allocated one:
 * allocating changes the map, which invalidates the key ExitBootServices needs,
 * and the retry dance that follows is a well-known source of boot hangs. */
static unsigned char mmap_buf[32 * 1024];

/* The command line arrives two different ways depending on who started us.
 * A device-tree loader writes it into /chosen/bootargs; UEFI passes it as
 * LoadOptions on the loaded image, in UTF-16. vboot uses the latter, which is
 * why stam reported an empty command line the first time our own bootloader
 * started it — the string was there, in a place nothing was reading. */
static char cmdline_buf[512];

static const char *efi_cmdline(EFI_HANDLE image, EFI_BOOT_SERVICES *BS) {
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *li = 0;
    if (EFI_ERROR(BS->HandleProtocol(image, &li_guid, (void **)&li)) || !li) return 0;
    const CHAR16 *w = (const CHAR16 *)li->LoadOptions;
    if (!w || !li->LoadOptionsSize) return 0;
    UINTN n = li->LoadOptionsSize / sizeof(CHAR16);
    UINTN o = 0;
    for (UINTN i = 0; i < n && o < sizeof cmdline_buf - 1; i++) {
        if (!w[i]) break;
        /* Anything outside ASCII cannot appear in a kernel command line, and
         * silently truncating there would hide the rest of it. */
        cmdline_buf[o++] = (w[i] < 0x80) ? (char)w[i] : '?';
    }
    cmdline_buf[o] = 0;
    return o ? cmdline_buf : 0;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab) {
    ST = systab;
    say(L"\r\n  stam: started as an EFI application\r\n");

    /* 1. find the device tree */
    void *dtb = 0;
    config_entry_t *ct = (config_entry_t *)systab->ConfigurationTable;
    for (UINTN i = 0; i < systab->NumberOfTableEntries; i++)
        if (guid_eq(&ct[i].VendorGuid, &DTB_GUID)) { dtb = ct[i].VendorTable; break; }

    if (!dtb) {
        /* Say precisely what is missing. On x86_64 this is the expected state
         * until vboot synthesises a tree, and a vague message there would send
         * someone looking in the wrong place for a long time. */
        say(L"  stam: no device tree in the UEFI configuration table.\r\n");
        say(L"  stam: on aarch64 the firmware should publish one; on x86_64\r\n");
        say(L"  stam: the loader must build one and install it under the same\r\n");
        say(L"  stam: GUID. Refusing to continue rather than guess.\r\n");
        return EFI_LOAD_ERROR;
    }
    say(L"  stam: device tree found, leaving boot services\r\n");

    /* 2. the command line, while boot services still exist to ask for it */
    handover_set_cmdline(efi_cmdline(image, systab->BootServices));

    /* 3. take the memory map and leave. After this call no EFI service may be
     * used — including the console we have been printing on. */
    EFI_BOOT_SERVICES *BS = systab->BootServices;
    get_mmap_t get_mmap = (get_mmap_t)BS->GetMemoryMap;
    UINTN size = sizeof mmap_buf, key = 0, dsize = 0;
    UINT32 dver = 0;
    EFI_STATUS st = get_mmap(&size, mmap_buf, &key, &dsize, &dver);
    if (EFI_ERROR(st)) { say(L"  stam: GetMemoryMap failed\r\n"); return st; }

    st = BS->ExitBootServices(image, key);
    if (EFI_ERROR(st)) {
        /* The map changed between the two calls — the one legitimate reason to
         * retry, and exactly once. */
        size = sizeof mmap_buf;
        if (EFI_ERROR(get_mmap(&size, mmap_buf, &key, &dsize, &dver))) return st;
        st = BS->ExitBootServices(image, key);
        if (EFI_ERROR(st)) return st;
    }

    /* The firmware's map is the only account of what memory is actually free.
     * Without it the kernel can do no better than "everything after my own
     * image", and UEFI loads that image high — which found 58 MiB free on a
     * machine with a gigabyte. Take the largest conventional region. */
    {
        uint64_t best_base = 0, best_size = 0;
        for (UINTN o = 0; dsize && o + dsize <= size; o += dsize) {
            unsigned char *d = mmap_buf + o;
            UINT32 type = (UINT32)d[0] | ((UINT32)d[1] << 8) |
                          ((UINT32)d[2] << 16) | ((UINT32)d[3] << 24);
            if (type != 7) continue;              /* EfiConventionalMemory */
            uint64_t start = 0, pages = 0;
            for (int i = 0; i < 8; i++) start |= (uint64_t)d[8 + i] << (8 * i);
            for (int i = 0; i < 8; i++) pages |= (uint64_t)d[24 + i] << (8 * i);
            if (pages * 4096 > best_size) { best_base = start; best_size = pages * 4096; }
        }
        if (best_size) handover_set_free_region(best_base, best_size);
    }

    /* 4. the machine is ours. From here the only console is the one we find
     * in the device tree for ourselves. */
    kmain(dtb);
    for (;;) __asm__ volatile("wfe");
}
