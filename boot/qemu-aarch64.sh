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

# User-mode networking on the virt board's virtio-mmio bus (virtio-net-device,
# not -pci, so headless QEMU needs no option ROM). Guest gets 10.0.2.15 via DHCP.
# virtio-gpu-device gives the guest a KMS GPU -> /dev/fb0 (VerkOS' GUI draws to it);
# virtio-keyboard/tablet feed evdev for a future compositor. Set VERK_QMP=<path>
# for a QMP control socket (used to `screendump` the framebuffer headlessly).
GPU_ARGS=(-device virtio-gpu-device -device virtio-keyboard-device -device virtio-tablet-device)
QMP_ARGS=()
[ -n "${VERK_QMP:-}" ] && QMP_ARGS=(-qmp "unix:${VERK_QMP},server,nowait")
exec "$QEMU_BIN" \
    -machine "$QEMU_MACHINE" -cpu "$QEMU_CPU" -m 2048 -smp 2 \
    -kernel "$KERNEL" \
    -append "root=/dev/vda rw console=${QEMU_CONSOLE} init=/usr/lib/systemd/systemd" \
    -drive file="$DISK",format=raw,if=virtio \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 -device virtio-net-device,netdev=net0 \
    "${GPU_ARGS[@]}" "${QMP_ARGS[@]}" \
    $QEMU_EXTRA
