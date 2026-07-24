/* vboot — VerkOS' own UEFI bootloader (aarch64 + x86_64, no gnu-efi).
 *
 * Milestone 1: a bare EFI application that the firmware loads from
 * \EFI\BOOT\BOOTAA64.EFI and that prints the VerkOS banner. Next milestones
 * load vmlinuz + initramfs off the ESP and hand off via the Linux EFI stub.
 */
#include "efi.h"

static EFI_SYSTEM_TABLE *ST;
static void print(CHAR16 *s) { ST->ConOut->OutputString(ST->ConOut, s); }

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab) {
    (void)image;
    ST = systab;
    if (ST->ConOut->ClearScreen) ST->ConOut->ClearScreen(ST->ConOut);
    print(L"\r\n");
    print(L"  ##   ##   VerkOS vboot 0.1\r\n");
    print(L"   ## ##    our own UEFI bootloader\r\n");
    print(L"    ###     bare aarch64 UEFI, no gnu-efi\r\n");
    print(L"     #\r\n");
    print(L"\r\n");
    print(L"  [ok] firmware handed control to vboot\r\n");
    print(L"  [..] next: load vmlinuz + initramfs from the ESP\r\n");
    print(L"\r\n");
    for (;;) if (ST->BootServices->Stall) ST->BootServices->Stall(1000000);
    return EFI_SUCCESS;
}
