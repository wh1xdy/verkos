#!/usr/bin/env bash
# Boot VerkOS/aarch64 in QEMU: direct kernel boot with the rootfs on a virtio
# disk (root=/dev/vda), systemd as PID 1, on the 'virt' board. Serial console.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/../config/targets/aarch64.sh"
OUT="${WORK:-$HERE/../work}/aarch64/out"

KERNEL="$OUT/vmlinuz"        # arm64 'Image' installed as vmlinuz by stage 50
DISK="$OUT/verkos-aarch64.ext4"
[ -f "$KERNEL" ] || { echo "no kernel at $KERNEL — run: make kernel ARCH=aarch64" >&2; exit 1; }
[ -f "$DISK" ]   || { echo "no disk at $DISK — run: make image ARCH=aarch64" >&2; exit 1; }

# -nic none: the 'virt' board's default virtio-net wants a ROM file that headless
# QEMU builds may lack, and networking isn't needed to boot. Add it back later.
exec "$QEMU_BIN" \
    -machine "$QEMU_MACHINE" -cpu "$QEMU_CPU" -m 2048 -smp 2 \
    -kernel "$KERNEL" \
    -append "root=/dev/vda rw console=${QEMU_CONSOLE} init=/usr/lib/systemd/systemd" \
    -drive file="$DISK",format=raw,if=virtio \
    -nic none $QEMU_EXTRA
