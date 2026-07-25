#!/bin/bash
# build.sh — build VerkOS' own UEFI bootloader (vboot) and wrap it in a bootable
# GPT+ESP disk for QEMU testing. No gnu-efi: plain gcc + binutils, which VerkOS'
# own toolchain already has.
#
#   build.sh [OUTDIR] [ARCH]      ARCH is aarch64 (default) or x86_64
#
# Both architectures share efi.h, vboot.c and vboot.lds. What differs:
#   aarch64  AAPCS64 calling convention, objcopy target efi-app-aarch64,
#            payload named BOOTAA64.EFI
#   x86_64   Microsoft x64 ABI (EFIAPI expands to ms_abi in efi.h), objcopy
#            target efi-app-x86_64, payload named BOOTX64.EFI, and the compiler
#            must not use the red zone or SSE — firmware calls and interrupts
#            make both unsafe in an EFI application.
#
# Two things are needed to satisfy a strict EDK2 loader, on either architecture:
#   1. the first section must start at RVA 0x1000 with page-sized headers
#      (--file-alignment/--section-alignment 0x1000 + `. = 0x1000` in vboot.lds);
#      with .text at RVA 0 (overlapping the headers) LoadImage says UNSUPPORTED.
#   2. IMAGE_FILE_RELOCS_STRIPPED must be cleared (pe-fixup.py) — objcopy sets it
#      because there is no .reloc, but with ImageBase=0 that means "load at 0,
#      never move me", which the firmware cannot do. vboot is -fpic with zero
#      relocations, so it runs at any address.
# Verified against the distro's systemd-bootaa64.efi, which is laid out the same.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-/tmp/vboot-out}"; mkdir -p "$OUT"
ARCH="${2:-aarch64}"

case "$ARCH" in
    aarch64)
        CC=${CC:-gcc}; OBJCOPY=${OBJCOPY:-objcopy}
        ARCHFLAGS="-mgeneral-regs-only"
        TARGET=efi-app-aarch64
        PAYLOAD=BOOTAA64.EFI
        ;;
    x86_64)
        # cross-build from another host if a toolchain prefix is available
        if command -v x86_64-linux-gnu-gcc >/dev/null && [ "$(uname -m)" != "x86_64" ]; then
            CC=${CC:-x86_64-linux-gnu-gcc}; OBJCOPY=${OBJCOPY:-x86_64-linux-gnu-objcopy}
            LD=${LD:-x86_64-linux-gnu-ld}
        else
            CC=${CC:-gcc}; OBJCOPY=${OBJCOPY:-objcopy}
        fi
        # no red zone (firmware/interrupts clobber it), no SSE/MMX in EFI context
        ARCHFLAGS="-mno-red-zone -mno-mmx -mno-sse"
        TARGET=efi-app-x86_64
        PAYLOAD=BOOTX64.EFI
        ;;
    *) echo "build.sh: unknown arch '$ARCH' (aarch64 or x86_64)" >&2; exit 1 ;;
esac
LD=${LD:-ld}

# 1. compile + link a position-independent freestanding EFI app
"$CC" -ffreestanding -fno-stack-protector -fshort-wchar $ARCHFLAGS \
    -fpic -fno-jump-tables -Wall -I"$HERE" -c "$HERE/vboot.c" -o "$OUT/vboot.o"
"$LD" -nostdlib -znocombreloc -shared -Bsymbolic -T "$HERE/vboot.lds" \
    "$OUT/vboot.o" -o "$OUT/vboot.so"

# 2. convert the ELF to a PE/COFF EFI application
"$OBJCOPY" -j .text -j .data -j .reloc --target "$TARGET" --subsystem=10 \
    --file-alignment 0x1000 --section-alignment 0x1000 \
    "$OUT/vboot.so" "$OUT/$PAYLOAD"
# 3. clear RELOCS_STRIPPED so the firmware may load the (PC-relative) image anywhere
python3 "$HERE/pe-fixup.py" "$OUT/$PAYLOAD"
echo "built $OUT/$PAYLOAD ($ARCH)"

# 4. (test only) wrap in a GPT disk with an ESP that has \EFI\BOOT\<payload>
if command -v mkfs.vfat >/dev/null && command -v parted >/dev/null; then
    dd if=/dev/zero of="$OUT/esp.img" bs=1M count=60 status=none
    mkfs.vfat -n VERKESP "$OUT/esp.img" >/dev/null
    mmd -i "$OUT/esp.img" ::/EFI ::/EFI/BOOT
    mcopy -i "$OUT/esp.img" "$OUT/$PAYLOAD" "::/EFI/BOOT/$PAYLOAD"
    dd if=/dev/zero of="$OUT/disk.img" bs=1M count=64 status=none
    parted -s "$OUT/disk.img" mklabel gpt
    parted -s "$OUT/disk.img" mkpart ESP fat32 1MiB 61MiB
    parted -s "$OUT/disk.img" set 1 esp on
    dd if="$OUT/esp.img" of="$OUT/disk.img" bs=1M seek=1 conv=notrunc status=none
    echo "test disk: $OUT/disk.img"
fi
