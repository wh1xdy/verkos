#!/usr/bin/env bash
# Boot VerkOS/aarch64 in QEMU via direct kernel boot on the 'virt' board.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/../config/targets/aarch64.sh"
OUT="${WORK:-$HERE/../work}/aarch64/out"

KERNEL="$OUT/vmlinuz"        # arm64 'Image' installed as vmlinuz by stage 50
INITRD="$OUT/initramfs-aarch64.cpio.gz"
[ -f "$KERNEL" ] || { echo "no kernel at $KERNEL — run: make kernel ARCH=aarch64" >&2; exit 1; }
[ -f "$INITRD" ] || { echo "no initramfs at $INITRD — run: make image ARCH=aarch64" >&2; exit 1; }

# 'virt' + cortex-a72 is a clean, well-supported dev target. No firmware needed
# for direct kernel boot.
exec "$QEMU_BIN" \
    -machine "$QEMU_MACHINE" -cpu "$QEMU_CPU" -m 1024 -smp 2 \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -append "console=${QEMU_CONSOLE} rdinit=/init" \
    $QEMU_EXTRA
