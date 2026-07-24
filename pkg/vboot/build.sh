#!/bin/bash
# build.sh — build VerkOS' own UEFI bootloader (vboot) for aarch64 and wrap it
# in a bootable GPT+ESP disk for QEMU/AAVMF testing. No gnu-efi: plain gcc +
# binutils (objcopy --target efi-app-aarch64), which VerkOS' own toolchain has.
#
# STATUS (2026-07-24): the EFI app COMPILES to a valid PE32+ EFI application
# (MZ/PE headers correct, subsystem=10, machine=aarch64, 16 data dirs, ZERO
# relocations — fully PC-relative), the AAVMF firmware boots, and the GPT+ESP is
# recognized (FS0 maps, startup.nsh finds \EFI\BOOT\BOOTAA64.EFI). REMAINING:
# EDK2's LoadImage returns EFI_UNSUPPORTED for our objcopy-produced PE despite a
# textbook-valid header. Next step: diff against a gnu-efi crt0-built PE to find
# the discrepancy (likely a section-flag / dummy-.reloc detail EDK2 insists on),
# then bake the fix into the linker script so no gnu-efi dependency is needed.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-/tmp/vboot-out}"; mkdir -p "$OUT"

# 1. compile + link a position-independent freestanding EFI app
gcc -ffreestanding -fno-stack-protector -fshort-wchar -mgeneral-regs-only \
    -fpic -fno-jump-tables -Wall -I"$HERE" -c "$HERE/vboot.c" -o "$OUT/vboot.o"
ld -nostdlib -znocombreloc -shared -Bsymbolic -T "$HERE/vboot.lds" \
    "$OUT/vboot.o" -o "$OUT/vboot.so"

# 2. convert the ELF to a PE/COFF EFI application (aarch64 uses AAPCS, no ms_abi)
objcopy -j .text -j .rodata -j .data --target efi-app-aarch64 --subsystem=10 \
    "$OUT/vboot.so" "$OUT/BOOTAA64.EFI"
echo "built $OUT/BOOTAA64.EFI"

# 3. (test only) wrap in a GPT disk with an ESP that has \EFI\BOOT\BOOTAA64.EFI
if command -v mkfs.vfat >/dev/null && command -v parted >/dev/null; then
    dd if=/dev/zero of="$OUT/esp.img" bs=1M count=60 status=none
    mkfs.vfat -n VERKESP "$OUT/esp.img" >/dev/null
    mmd -i "$OUT/esp.img" ::/EFI ::/EFI/BOOT
    mcopy -i "$OUT/esp.img" "$OUT/BOOTAA64.EFI" ::/EFI/BOOT/BOOTAA64.EFI
    dd if=/dev/zero of="$OUT/disk.img" bs=1M count=64 status=none
    parted -s "$OUT/disk.img" mklabel gpt
    parted -s "$OUT/disk.img" mkpart ESP fat32 1MiB 61MiB
    parted -s "$OUT/disk.img" set 1 esp on
    dd if="$OUT/esp.img" of="$OUT/disk.img" bs=1M seek=1 conv=notrunc status=none
    echo "test disk: $OUT/disk.img  (boot: qemu-system-aarch64 -M virt -cpu cortex-a72"
    echo "  -drive if=pflash,format=raw,file=QEMU_EFI-padded.fd,readonly=on"
    echo "  -drive if=pflash,format=raw,file=vars.fd"
    echo "  -device virtio-blk-pci,drive=hd0 -drive if=none,id=hd0,file=$OUT/disk.img)"
fi
