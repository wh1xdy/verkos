#!/usr/bin/env bash
# Boot VerkOS/aarch64 in QEMU the way a real machine boots it: UEFI firmware
# starts vboot from the EFI System Partition, vboot loads the kernel and
# initramfs, urverk takes over as PID 1. No -kernel, no external bootloader.
#
# Set VERK_DIRECT=1 to fall back to direct kernel boot (handy when the boot
# image is not built yet, or to bisect a bootloader problem).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/../config/targets/aarch64.sh"
OUT="${WORK:-$HERE/../work}/aarch64/out"

DISK="$OUT/verkos-aarch64.ext4"
BOOTIMG="$OUT/verkos-boot-aarch64.img"
KERNEL="$OUT/vmlinuz"
[ -f "$DISK" ] || { echo "no rootfs at $DISK — run: make image ARCH=aarch64" >&2; exit 1; }

# virtio-gpu gives the guest a KMS device (/dev/fb0, which vfb draws to) and
# vboot a GOP to render its menu on; the input devices feed evdev.
GPU_ARGS=(-device virtio-gpu-pci -device virtio-keyboard-pci -device virtio-tablet-pci)
QMP_ARGS=()
[ -n "${VERK_QMP:-}" ] && QMP_ARGS=(-qmp "unix:${VERK_QMP},server,nowait")
# romfile= : this QEMU has no efi-virtio.rom, and the guest does not need one
NET_ARGS=(-netdev user,id=net0,hostfwd=tcp::2222-:22
          -device virtio-net-pci,netdev=net0,romfile=)

if [ "${VERK_DIRECT:-0}" != "1" ] && [ -f "$BOOTIMG" ]; then
    # Locate the aarch64 UEFI firmware and give it a writable variable store, so
    # vboot can remember the last entry it booted.
    FW=""
    for c in /usr/share/qemu-efi-aarch64/QEMU_EFI.fd /usr/share/AAVMF/AAVMF_CODE.fd \
             /usr/share/edk2/aarch64/QEMU_EFI.fd; do
        [ -f "$c" ] && { FW="$c"; break; }
    done
    [ -n "$FW" ] || { echo "no aarch64 UEFI firmware found (install qemu-efi-aarch64)" >&2; exit 1; }
    VARS="${WORK:-$HERE/../work}/aarch64/efi-vars.fd"
    CODE="${WORK:-$HERE/../work}/aarch64/efi-code.fd"
    # the firmware volumes must be exactly 64 MiB for the virt board's pflash
    [ -f "$CODE" ] || { dd if=/dev/zero of="$CODE" bs=1M count=64 status=none
                        dd if="$FW" of="$CODE" conv=notrunc status=none; }
    [ -f "$VARS" ] || dd if=/dev/zero of="$VARS" bs=1M count=64 status=none

    exec "$QEMU_BIN" \
        -machine "$QEMU_MACHINE" -cpu "$QEMU_CPU" -m 2048 -smp 2 \
        -drive if=pflash,format=raw,file="$CODE",readonly=on \
        -drive if=pflash,format=raw,file="$VARS" \
        -drive if=none,id=rootdisk,file="$DISK",format=raw \
        -device virtio-blk-pci,drive=rootdisk \
        -drive if=none,id=espdisk,file="$BOOTIMG",format=raw \
        -device virtio-blk-pci,drive=espdisk,bootindex=0 \
        "${NET_ARGS[@]}" "${GPU_ARGS[@]}" "${QMP_ARGS[@]}" \
        $QEMU_EXTRA
fi

# Fallback: direct kernel boot (no bootloader, no initramfs).
[ -f "$KERNEL" ] || { echo "no kernel at $KERNEL — run: make kernel ARCH=aarch64" >&2; exit 1; }
exec "$QEMU_BIN" \
    -machine "$QEMU_MACHINE" -cpu "$QEMU_CPU" -m 2048 -smp 2 \
    -kernel "$KERNEL" \
    -append "root=/dev/vda rw console=${QEMU_CONSOLE} init=/sbin/urverk" \
    -drive file="$DISK",format=raw,if=virtio \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 -device virtio-net-device,netdev=net0 \
    "${GPU_ARGS[@]}" "${QMP_ARGS[@]}" \
    $QEMU_EXTRA
