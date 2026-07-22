#!/usr/bin/env bash
# Boot VerkOS/i686 in QEMU via direct kernel boot (no bootloader).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/../config/targets/i686.sh"
OUT="${WORK:-$HERE/../work}/i686/out"

KERNEL="$OUT/vmlinuz"
INITRD="$OUT/initramfs-i686.cpio.gz"
[ -f "$KERNEL" ] || { echo "no kernel at $KERNEL — run: make kernel ARCH=i686" >&2; exit 1; }
[ -f "$INITRD" ] || { echo "no initramfs at $INITRD — run: make image ARCH=i686" >&2; exit 1; }

exec "$QEMU_BIN" \
    -machine "$QEMU_MACHINE" -cpu "$QEMU_CPU" -m 1024 -smp 2 \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -append "console=${QEMU_CONSOLE} rdinit=/init" \
    $QEMU_EXTRA
