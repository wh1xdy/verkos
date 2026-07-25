#!/usr/bin/env bash
# Stage 70 — assemble the bootable UEFI disk: vboot, an initramfs, and an ESP.
#
# Stage 60 produces the root filesystem; this produces the thing a machine
# actually starts from, so that a plain `make all` gives something bootable
# rather than something that still needs QEMU's -kernel.
#
# Output: $OUT_DIR/verkos-boot-$ARCH.img — a GPT disk whose EFI System Partition
# holds \EFI\BOOT\BOOT{AA64,X64}.EFI (vboot), \vmlinuz, \initramfs.img and
# \vboot.conf. Boot it with the firmware; no -kernel, no bootloader from anyone
# else.
source "$(dirname "$0")/lib/common.sh"
mkdirs

ESP_MB="${VERK_ESP_MB:-160}"      # kernel + initramfs + room for a second kernel

case "$ARCH" in
    aarch64) EFI_NAME=BOOTAA64.EFI; CONSOLE=ttyAMA0 ;;
    x86_64)  EFI_NAME=BOOTX64.EFI;  CONSOLE=ttyS0   ;;
    *)       die "stage 70 does not know how to boot $ARCH" ;;
esac

need_tool() { command -v "$1" >/dev/null || die "$1 is required for the boot image (install $2)"; }

build_initramfs() {
    local work="$BUILD_DIR/initramfs" out="$OUT_DIR/initramfs.img"
    local SUDO=; [ "$(id -u)" = 0 ] || SUDO=sudo

    [ -x "$ROOTFS/sbin/urverk" ] || die "no urverk in the rootfs — run stage 40 first"

    # Build VerkOS' own cpio and gzip for the *host*, so the initramfs is made
    # with our tools even when the target binaries are a foreign architecture.
    local tools="$BUILD_DIR/hosttools"
    mkdir -p "$tools"
    cc -O2 -o "$tools/vcpio" "$VERK_ROOT/pkg/vcpio/vcpio.c" 2>/dev/null \
        || die "could not build vcpio for the host"
    cc -O2 -o "$tools/vgzip" "$VERK_ROOT/pkg/vgzip/vgzip.c" 2>/dev/null \
        || die "could not build vgzip for the host"

    $SUDO rm -rf "$work"
    mkdir -p "$work"/{proc,sys,dev,run,newroot}
    # urverk is one binary in two stages: as /init it does early userspace, then
    # switch_roots into the same binary as PID 1 on the real root.
    $SUDO cp "$ROOTFS/sbin/urverk" "$work/init"
    $SUDO chmod 0755 "$work/init"

    ( cd "$work" && find . | "$tools/vcpio" -o ) > "$BUILD_DIR/initramfs.cpio" \
        || die "vcpio failed"
    "$tools/vgzip" -c "$BUILD_DIR/initramfs.cpio" > "$out" || die "vgzip failed"
    ok "initramfs → $out ($(du -h "$out" | cut -f1))"
}

build_vboot() {
    bash "$VERK_ROOT/pkg/vboot/build.sh" "$BUILD_DIR/vboot" "$ARCH" >/dev/null \
        || die "vboot build failed"
    [ -f "$BUILD_DIR/vboot/$EFI_NAME" ] || die "vboot produced no $EFI_NAME"
    ok "vboot → $EFI_NAME"
}

# A config that matches this build: the console the kernel was told to use, and
# urverk as init. Anything the user put in pkg/vboot/vboot.conf wins.
write_config() {
    local cfg="$BUILD_DIR/vboot.conf"
    if [ -f "$VERK_ROOT/pkg/vboot/vboot.conf" ]; then
        sed "s/ttyAMA0/$CONSOLE/g" "$VERK_ROOT/pkg/vboot/vboot.conf" > "$cfg"
    else
        cat > "$cfg" <<EOF
timeout 5
default 0

entry VerkOS ${VERK_VERSION:-0.1}
  kernel  \\vmlinuz
  initrd  \\initramfs.img
  cmdline root=/dev/vda rw console=$CONSOLE init=/sbin/urverk
EOF
    fi
    echo "$cfg"
}

build_boot_disk() {
    need_tool mkfs.vfat dosfstools
    need_tool mcopy mtools
    need_tool parted parted
    local esp="$BUILD_DIR/esp.img" img="$OUT_DIR/verkos-boot-${ARCH}.img"
    local kernel="$OUT_DIR/vmlinuz" initramfs="$OUT_DIR/initramfs.img"
    [ -f "$kernel" ] || die "no kernel at $kernel — run stage 50 first"
    local cfg; cfg="$(write_config)"

    log "building a ${ESP_MB} MiB EFI system partition"
    dd if=/dev/zero of="$esp" bs=1M count="$ESP_MB" status=none
    mkfs.vfat -n VERKESP "$esp" >/dev/null
    mmd -i "$esp" ::/EFI ::/EFI/BOOT
    mcopy -i "$esp" "$BUILD_DIR/vboot/$EFI_NAME" "::/EFI/BOOT/$EFI_NAME"
    mcopy -i "$esp" "$kernel"    ::/vmlinuz
    mcopy -i "$esp" "$initramfs" ::/initramfs.img
    mcopy -i "$esp" "$cfg"       ::/vboot.conf
    # signatures, when the payloads have been signed (see pkg/vboot/vsign.c)
    [ -f "$kernel.sig" ]    && mcopy -i "$esp" "$kernel.sig"    ::/vmlinuz.sig
    [ -f "$initramfs.sig" ] && mcopy -i "$esp" "$initramfs.sig" ::/initramfs.img.sig

    # GPT with a single ESP; the firmware needs the partition table to find it
    local total=$((ESP_MB + 4))
    dd if=/dev/zero of="$img" bs=1M count="$total" status=none
    parted -s "$img" mklabel gpt
    parted -s "$img" mkpart ESP fat32 1MiB "$((ESP_MB + 1))MiB"
    parted -s "$img" set 1 esp on
    dd if="$esp" of="$img" bs=1M seek=1 conv=notrunc status=none
    ok "boot disk → $img ($(du -h "$img" | cut -f1))"
}

# Deliberately not stamped: these are cheap, and a boot image that silently
# lags the kernel or urverk it is supposed to start is worse than rebuilding.
build_vboot
build_initramfs
build_boot_disk

echo
ok "Boot it with:  make run ARCH=$ARCH"
