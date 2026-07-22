#!/usr/bin/env bash
# Stage 60 — assemble a bootable initramfs from the rootfs.
#
# For development we boot QEMU with the kernel + an initramfs cpio (direct
# kernel boot). A GRUB disk image comes later (see boot/grub/). The initramfs
# is a gzipped cpio of $ROOTFS with a tiny /init if systemd isn't present yet.
source "$(dirname "$0")/lib/common.sh"
mkdirs

build_initramfs() {
    [ -d "$ROOTFS" ] || die "no rootfs at $ROOTFS — build stages 30-40 first"

    # If systemd is installed, boot it as init; otherwise drop a minimal
    # /init that mounts the essentials and execs a shell, so we can still boot
    # and poke around during early development.
    if [ ! -e "$ROOTFS/usr/lib/systemd/systemd" ] && [ ! -e "$ROOTFS/init" ]; then
        warn "systemd not installed yet — writing a minimal fallback /init"
        sudo tee "$ROOTFS/init" >/dev/null <<'INIT'
#!/bin/sh
mount -t proc     proc     /proc
mount -t sysfs    sysfs    /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null
echo
echo "  Welcome to VerkOS (early boot / no init yet)"
echo "  $(cat /etc/verk-release 2>/dev/null || echo VerkOS)"
echo
exec /bin/sh
INIT
        sudo chmod +x "$ROOTFS/init"
    fi

    local img="$OUT_DIR/initramfs-${ARCH}.cpio.gz"
    log "packing $ROOTFS → $img"
    ( cd "$ROOTFS" && sudo find . -print0 \
        | sudo cpio --null -ov --format=newc 2>/dev/null ) \
        | gzip -9 > "$img"
    ok "initramfs → $img ($(du -h "$img" | cut -f1))"
}

run_stage "image-initramfs" build_initramfs

echo
ok "Boot it with:  make run ARCH=$ARCH   (or boot/qemu-$ARCH.sh)"
