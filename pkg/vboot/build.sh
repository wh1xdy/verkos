#!/bin/bash
# build.sh — build VerkOS' own UEFI bootloader (vboot) for aarch64 and wrap it
# in a bootable GPT+ESP disk for QEMU/AAVMF testing. No gnu-efi: plain gcc +
# binutils (objcopy --target efi-app-aarch64), which VerkOS' own toolchain has.
#
# STATUS (2026-07-24): WORKING. AAVMF auto-boots \EFI\BOOT\BOOTAA64.EFI and
# vboot prints its banner. Two things were needed to satisfy the strict EDK2
# loader, both handled below:
#   1. the first section must start at RVA 0x1000 with page-sized headers
#      (--file-alignment/--section-alignment 0x1000 + `. = 0x1000` in vboot.lds);
#      with .text at RVA 0 (overlapping the headers) LoadImage says UNSUPPORTED.
#   2. IMAGE_FILE_RELOCS_STRIPPED must be cleared (pe-fixup.py) — objcopy sets it
#      because there is no .reloc, but with ImageBase=0 that means "load at 0,
#      never move me", which the firmware cannot do. vboot is -fpic with zero
#      relocations, so it runs at any address.
# Verified by comparing against the distro's systemd-bootaa64.efi, which has the
# identical layout (.text @ 0x1000, SizeOfHeaders=4096, no .reloc, flag clear).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-/tmp/vboot-out}"; mkdir -p "$OUT"

# 1. compile + link a position-independent freestanding EFI app
gcc -ffreestanding -fno-stack-protector -fshort-wchar -mgeneral-regs-only \
    -fpic -fno-jump-tables -Wall -I"$HERE" -c "$HERE/vboot.c" -o "$OUT/vboot.o"
ld -nostdlib -znocombreloc -shared -Bsymbolic -T "$HERE/vboot.lds" \
    "$OUT/vboot.o" -o "$OUT/vboot.so"

# 2. convert the ELF to a PE/COFF EFI application (aarch64 uses AAPCS, no ms_abi).
#    Page-sized alignment keeps the headers in their own page ahead of .text.
objcopy -j .text -j .data -j .reloc --target efi-app-aarch64 --subsystem=10 \
    --file-alignment 0x1000 --section-alignment 0x1000 \
    "$OUT/vboot.so" "$OUT/BOOTAA64.EFI"
# 3. clear RELOCS_STRIPPED so the firmware may load the (PC-relative) image anywhere
python3 "$HERE/pe-fixup.py" "$OUT/BOOTAA64.EFI"
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
