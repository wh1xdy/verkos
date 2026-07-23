#!/usr/bin/env bash
# Stage 60 — build a bootable root filesystem image from the rootfs.
#
# We boot QEMU with a direct kernel boot (-kernel) plus the rootfs on a virtio
# disk (root=/dev/vda), which scales to a full glibc+systemd system far better
# than stuffing multiple GB into an initramfs loaded entirely into RAM. GRUB +
# a partitioned disk for real hardware comes later (see boot/grub/).
source "$(dirname "$0")/lib/common.sh"
mkdirs

build_ext4_image() {
    [ -d "$ROOTFS" ] || die "no rootfs at $ROOTFS — build stages 30-40 first"
    command -v mkfs.ext4 >/dev/null || die "mkfs.ext4 (e2fsprogs) is required"

    local img="$OUT_DIR/verkos-${ARCH}.ext4"
    # Size the image to the rootfs usage + 25% headroom (min 2 GB), so it also
    # has room to be used/written after boot.
    # Generous free space: rootfs + VERK_IMAGE_HEADROOM_MB (default 4 GiB), so
    # there's room to build/install packages with vpk on the running system.
    local kb size_mb
    kb=$(du -sk "$ROOTFS" | cut -f1)
    size_mb=$(( kb / 1024 + ${VERK_IMAGE_HEADROOM_MB:-4096} ))
    [ "$size_mb" -lt 4096 ] && size_mb=4096

    rm -f "$img"
    log "creating ${size_mb} MiB ext4 image from $ROOTFS"
    # -d populates the fs from a directory; run as root so ownership is preserved.
    mkfs.ext4 -q -F -L VERKROOT -m 1 -d "$ROOTFS" "$img" "${size_mb}M"
    ok "root image → $img ($(du -h "$img" | cut -f1))"
}

run_stage "image-ext4" build_ext4_image

echo
ok "Boot it with:  make run ARCH=$ARCH   (or boot/qemu-$ARCH.sh)"
