/* vboot — VerkOS' own UEFI bootloader.
 *
 * The firmware loads us from \EFI\BOOT\BOOTAA64.EFI. We then read the kernel
 * (\vmlinuz) off the same volume we were loaded from, hand it the VerkOS
 * command line, and start it. Linux is built with CONFIG_EFI_STUB, so vmlinuz
 * is itself a valid EFI application — LoadImage/StartImage is the whole
 * handoff, no arch-specific boot protocol to reimplement.
 *
 * No gnu-efi, no GRUB: hand-written EFI headers (efi.h) + plain gcc/binutils.
 * See build.sh for the two PE quirks a strict EDK2 loader requires.
 */
#include "efi.h"

#define KERNEL_PATH  L"\\vmlinuz"
#define VBOOT_CMDLINE \
    L"root=/dev/vda rw console=ttyAMA0 init=/usr/lib/systemd/systemd"

static EFI_SYSTEM_TABLE  *ST;
static EFI_BOOT_SERVICES *BS;

static void print(CHAR16 *s) { ST->ConOut->OutputString(ST->ConOut, s); }

/* print a UINTN in decimal (no libc) */
static void print_num(UINTN v) {
    CHAR16 buf[24]; int i = 23;
    buf[i--] = 0;
    if (!v) buf[i--] = L'0';
    while (v && i >= 0) { buf[i--] = L'0' + (v % 10); v /= 10; }
    print(&buf[i + 1]);
}
/* print an EFI_STATUS as 0x... so failures are identifiable */
static void print_status(EFI_STATUS s) {
    CHAR16 buf[20]; int i = 18;
    buf[19] = 0;
    if (!s) buf[i--] = L'0';
    while (s && i >= 1) { UINTN d = s & 0xf; buf[i--] = d < 10 ? L'0' + d : L'a' + (d - 10); s >>= 4; }
    buf[i--] = L'x'; buf[i] = L'0';
    print(&buf[i]);
}
static void fail(CHAR16 *what, EFI_STATUS st) {
    print(L"  [!!] "); print(what); print(L" failed: "); print_status(st); print(L"\r\n");
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab) {
    EFI_GUID li_guid  = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID fs_guid  = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_STATUS st;

    ST = systab; BS = systab->BootServices;
    if (ST->ConOut->ClearScreen) ST->ConOut->ClearScreen(ST->ConOut);
    print(L"\r\n");
    print(L"  ##   ##   VerkOS vboot 0.1\r\n");
    print(L"   ## ##    our own UEFI bootloader\r\n");
    print(L"    ###\r\n");
    print(L"     #\r\n\r\n");

    /* 1. find the volume we were loaded from */
    EFI_LOADED_IMAGE_PROTOCOL *li = 0;
    st = BS->HandleProtocol(image, &li_guid, (void **)&li);
    if (EFI_ERROR(st)) { fail(L"HandleProtocol(LoadedImage)", st); goto hang; }

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
    st = BS->HandleProtocol(li->DeviceHandle, &fs_guid, (void **)&fs);
    if (EFI_ERROR(st)) { fail(L"HandleProtocol(SimpleFileSystem)", st); goto hang; }

    EFI_FILE_PROTOCOL *root = 0, *kf = 0;
    st = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(st)) { fail(L"OpenVolume", st); goto hang; }

    /* 2. open + size the kernel (seek to EOF instead of pulling in FILE_INFO) */
    print(L"  [..] opening "); print(KERNEL_PATH); print(L"\r\n");
    st = root->Open(root, &kf, KERNEL_PATH, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st)) { fail(L"Open(vmlinuz)", st); goto hang; }

    UINT64 size = 0;
    if (EFI_ERROR((st = kf->SetPosition(kf, 0xFFFFFFFFFFFFFFFFULL))) ||
        EFI_ERROR((st = kf->GetPosition(kf, &size))) ||
        EFI_ERROR((st = kf->SetPosition(kf, 0)))) { fail(L"seek", st); goto hang; }

    /* 3. read it into memory */
    void *buf = 0;
    st = BS->AllocatePool(EfiLoaderData, (UINTN)size, &buf);
    if (EFI_ERROR(st)) { fail(L"AllocatePool", st); goto hang; }
    UINTN want = (UINTN)size;
    st = kf->Read(kf, &want, buf);
    if (EFI_ERROR(st)) { fail(L"Read", st); goto hang; }
    kf->Close(kf); root->Close(root);
    print(L"  [ok] kernel loaded, "); print_num(want); print(L" bytes\r\n");

    /* 4. let the firmware load the kernel image (it is a PE via CONFIG_EFI_STUB) */
    EFI_HANDLE kimg = 0;
    st = BS->LoadImage(0, image, 0, buf, want, &kimg);
    if (EFI_ERROR(st)) { fail(L"LoadImage(kernel)", st); goto hang; }

    /* 5. hand the kernel its command line via LoadOptions */
    EFI_LOADED_IMAGE_PROTOCOL *kli = 0;
    st = BS->HandleProtocol(kimg, &li_guid, (void **)&kli);
    if (EFI_ERROR(st)) { fail(L"HandleProtocol(kernel LoadedImage)", st); goto hang; }
    CHAR16 *cmdline = VBOOT_CMDLINE;
    UINTN clen = 0; while (cmdline[clen]) clen++;
    kli->LoadOptions     = cmdline;
    kli->LoadOptionsSize = (UINT32)((clen + 1) * sizeof(CHAR16));

    print(L"  [ok] cmdline: "); print(cmdline); print(L"\r\n");
    print(L"  [->] starting VerkOS\r\n\r\n");

    /* 6. go. On success this never returns. */
    st = BS->StartImage(kimg, 0, 0);
    fail(L"StartImage", st);

hang:
    print(L"\r\n  vboot halted.\r\n");
    for (;;) if (BS->Stall) BS->Stall(1000000);
    return st;
}
