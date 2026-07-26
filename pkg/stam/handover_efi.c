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

/* The initrd does not arrive the same way either. A device-tree loader writes
 * its address into /chosen; UEFI has no such convention, so Linux invented one
 * and everybody uses it: the loader installs a LoadFile2 protocol on a device
 * path carrying a known vendor GUID, and whoever wants the file goes and asks.
 * vboot already publishes it that way. Not collecting it was why a kernel
 * started by our own bootloader had a program to run and did not run it. */
static int efi_initrd(EFI_BOOT_SERVICES *BS, uint64_t *start, uint64_t *end) {
    static struct { VENDOR_DEVICE_PATH v; EFI_DEVICE_PATH_PROTOCOL e; }
        __attribute__((packed)) path;
    EFI_GUID lf2_guid = EFI_LOAD_FILE2_PROTOCOL_GUID;
    EFI_GUID vendor   = LINUX_EFI_INITRD_MEDIA_GUID;

    path.v.Header.Type    = MEDIA_DEVICE_PATH;
    path.v.Header.SubType = MEDIA_VENDOR_DP;
    path.v.Header.Length[0] = (UINT8)sizeof(VENDOR_DEVICE_PATH);
    path.v.Header.Length[1] = 0;
    path.v.Guid = vendor;
    path.e.Type    = END_DEVICE_PATH_TYPE;
    path.e.SubType = END_ENTIRE_DEVICE_PATH;
    path.e.Length[0] = (UINT8)sizeof(EFI_DEVICE_PATH_PROTOCOL);
    path.e.Length[1] = 0;

    EFI_DEVICE_PATH_PROTOCOL *dp = (EFI_DEVICE_PATH_PROTOCOL *)&path;
    EFI_HANDLE h = 0;
    if (EFI_ERROR(BS->LocateDevicePath(&lf2_guid, (void **)&dp, &h))) return 0;

    EFI_LOAD_FILE2_PROTOCOL *lf2 = 0;
    if (EFI_ERROR(BS->HandleProtocol(h, &lf2_guid, (void **)&lf2)) || !lf2) return 0;

    /* Asked with no buffer, it reports the size it needs. */
    UINTN size = 0;
    lf2->LoadFile(lf2, dp, 0, &size, 0);
    if (!size) return 0;

    void *buf = 0;
    if (EFI_ERROR(BS->AllocatePool(EfiLoaderData, size, &buf)) || !buf) return 0;
    if (EFI_ERROR(lf2->LoadFile(lf2, dp, 0, &size, buf))) return 0;

    *start = (uint64_t)(uintptr_t)buf;
    *end   = *start + (uint64_t)size;
    return 1;
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

    /* 2. the command line and the initrd, while boot services still exist */
    handover_set_cmdline(efi_cmdline(image, systab->BootServices));
    {
        uint64_t is = 0, ie = 0;
        if (efi_initrd(systab->BootServices, &is, &ie)) {
            handover_set_initrd(is, ie);
            say(L"  stam: initrd collected from the loader\r\n");
        }
    }

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
