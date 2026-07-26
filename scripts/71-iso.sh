#!/usr/bin/env bash
# Stage 71 — assemble a bootable live ISO.
#
# The medium carries two files and nothing else:
#
#   /efi.img      a FAT32 EFI system partition holding vboot, the kernel and
#                 the initramfs — the same one stage 70 builds, with a boot
#                 configuration that points at the live root instead of a disk
#   /rootfs.img   the ext4 system image stage 60 already produced
#
# The EFI image is registered as an El Torito no-emulation entry, so the
# firmware boots vboot out of it exactly as it would from a disk's ESP. Nothing
# in vboot changes.
#
# Because the system ships as one opaque image rather than as a directory tree,
# the ISO needs no Rock Ridge and no Joliet: two uppercase 8.3 names in one
# directory. That is what makes pkg/viso small enough to be ours (D-022).
#
# At boot, urverk mounts the ISO by its volume label, loop-mounts rootfs.img
# out of it, and stacks a tmpfs overlay so the live system is writable without
# anything reaching the medium.
source "$(dirname "$0")/lib/common.sh"
mkdirs

ISO_LABEL="${VERK_ISO_LABEL:-VERKLIVE}"

# The same per-architecture names stage 70 uses; kept here rather than sourced
# so this stage can run on its own.
case "$ARCH" in
    aarch64) EFI_NAME=BOOTAA64.EFI; CONSOLE=ttyAMA0 ;;
    x86_64)  EFI_NAME=BOOTX64.EFI;  CONSOLE=ttyS0   ;;
    *)       die "no EFI boot file name known for $ARCH" ;;
esac

need_tool() { command -v "$1" >/dev/null || die "$1 is required for the ISO (install $2)"; }

# The live boot configuration. Deliberately not pkg/vboot/vboot.conf, whose
# entries name a root partition that a live medium does not have.
write_live_config() {
    local cfg="$BUILD_DIR/vboot-live.conf"
    cat > "$cfg" <<EOF
# VerkOS live medium — written by stage 71.
#
# root= names the ISO itself, found by its volume label; the real system is the
# image on it, which urverk loop-mounts. rd.overlay puts a tmpfs on top so the
# live system can be written to without touching the medium.
timeout 5
default 0

entry VerkOS ${VERK_VERSION:-0.1} (live)
  kernel  \\vmlinuz
  initrd  \\initramfs.img
  cmdline root=LABEL=$ISO_LABEL rootfstype=iso9660 rd.image=/rootfs.img rd.overlay rd.esp=/efi.img rw console=$CONSOLE init=/sbin/urverk

entry VerkOS ${VERK_VERSION:-0.1} (live, copied into RAM)
  kernel  \\vmlinuz
  initrd  \\initramfs.img
  cmdline root=LABEL=$ISO_LABEL rootfstype=iso9660 rd.image=/rootfs.img rd.toram rd.overlay rd.esp=/efi.img rw console=$CONSOLE init=/sbin/urverk

entry VerkOS ${VERK_VERSION:-0.1} (live, verbose)
  kernel  \\vmlinuz
  initrd  \\initramfs.img
  cmdline root=LABEL=$ISO_LABEL rootfstype=iso9660 rd.image=/rootfs.img rd.overlay rd.esp=/efi.img rw console=$CONSOLE init=/sbin/urverk urverk.debug

entry VerkOS ${VERK_VERSION:-0.1} (rescue shell)
  kernel  \\vmlinuz
  initrd  \\initramfs.img
  cmdline root=LABEL=$ISO_LABEL rootfstype=iso9660 rd.image=/rootfs.img rd.overlay rd.esp=/efi.img rw console=$CONSOLE init=/sbin/urverk urverk.rescue
EOF
    echo "$cfg"
}

build_iso() {
    need_tool mkfs.vfat dosfstools
    need_tool mcopy mtools

    local kernel="$OUT_DIR/vmlinuz"
    local initramfs="$OUT_DIR/initramfs.img"
    local rootimg="$BUILD_DIR/live-rootfs.img"
    local vboot_efi="$BUILD_DIR/vboot/$EFI_NAME"

    [ -f "$kernel" ]    || die "no kernel at $kernel — run: make kernel ARCH=$ARCH"
    [ -f "$initramfs" ] || die "no initramfs at $initramfs — run: make boot ARCH=$ARCH"
    [ -d "$ROOTFS" ]    || die "no rootfs at $ROOTFS — run: make system ARCH=$ARCH"
    [ -f "$vboot_efi" ] || die "no vboot at $vboot_efi — run: make boot ARCH=$ARCH"

    # viso is ours, built for the host: the ISO is assembled here, not inside
    # the target system.
    local tools="$BUILD_DIR/hosttools"
    mkdir -p "$tools"
    cc -O2 -o "$tools/viso" "$VERK_ROOT/pkg/viso/viso.c" || die "could not build viso for the host"

    local cfg; cfg="$(write_live_config)"
    local esp="$BUILD_DIR/efi.img"
    local staged="$BUILD_DIR/iso"
    rm -rf "$staged"; mkdir -p "$staged"

    # --- the EFI boot image -------------------------------------------------
    # Sized to what it actually holds, with a little slack. FAT32 needs about
    # 33 MB to exist at all, which vmkfs enforces.
    # stat -L, not stat: $OUT_DIR/vmlinuz is a symlink to the versioned kernel,
    # and measuring the link gives 14 bytes rather than 45 MB.
    local need_mb
    need_mb=$(( ( $(stat -Lc%s "$kernel") + $(stat -Lc%s "$initramfs") ) / 1048576 + 32 ))
    [ "$need_mb" -lt 48 ] && need_mb=48
    log "EFI boot image: ${need_mb} MiB"
    dd if=/dev/zero of="$esp" bs=1M count="$need_mb" status=none
    # -F 32 is not optional. Without it dosfstools picks the type from the
    # cluster count and chose FAT16 at every size this has ever been built at,
    # while everything around it — this comment, the 33 MB floor, the docs —
    # said FAT32. UEFI requires FAT32 on fixed media and permits FAT12/16 only
    # on removable; AAVMF and OVMF accept FAT16 anyway, which is the only
    # reason it has always booted. That is D-023's shape: something we got away
    # with because we had only met one firmware.
    mkfs.vfat -F 32 -n VERKESP "$esp" >/dev/null
    mmd -i "$esp" ::/EFI ::/EFI/BOOT
    mcopy -i "$esp" "$vboot_efi" "::/EFI/BOOT/$EFI_NAME"
    mcopy -i "$esp" "$(readlink -f "$kernel")" ::/vmlinuz
    mcopy -i "$esp" "$initramfs" ::/initramfs.img
    mcopy -i "$esp" "$cfg"       ::/vboot.conf

    # --- the system image ---------------------------------------------------
    # Built here rather than reused from stage 60: that image is sized for a
    # writable installed system, with a quarter of headroom and a journal it
    # will never use here. A live image is read-only for its whole life, so it
    # gets neither — which took 1098 MB down to about what the files occupy,
    # and removes the needs_recovery problem at the source rather than working
    # around it at every boot.
    mount | grep -q " ${ROOTFS}/" && die "something is mounted inside $ROOTFS — unmount it first"
    local content_mb
    content_mb=$(du -sxm "$ROOTFS" | cut -f1)
    local img_mb=$(( content_mb + content_mb / 20 + 32 ))     # 5% and a little
    log "system image: ${content_mb} MB of files, ${img_mb} MB image (no journal)"
    rm -f "$rootimg"
    truncate -s "${img_mb}M" "$rootimg"
    # a different label from the ISO volume, so the two are never confused
    mkfs.ext4 -q -F -m 0 -O ^has_journal -L VERKROOT -d "$ROOTFS" "$rootimg" \
        || die "could not build the live system image"

    # --- what goes on the ISO ----------------------------------------------
    # The names matter: viso maps them to uppercase 8.3, and the kernel maps
    # them back to lower case, so rd.image=/rootfs.img resolves. Hard-link
    # rather than copy — the root image is over a gigabyte.
    ln -f "$esp" "$staged/efi.img" 2>/dev/null || cp "$esp" "$staged/efi.img"
    ln -f "$rootimg" "$staged/rootfs.img" 2>/dev/null || cp "$rootimg" "$staged/rootfs.img"
    if command -v e2fsck >/dev/null; then
        e2fsck -fp "$staged/rootfs.img" >/dev/null 2>&1
        case $? in
            0|1) ok "system image checked and clean" ;;
            *)   warn "e2fsck reported trouble with the system image" ;;
        esac
    fi

    local iso="$OUT_DIR/verkos-live-${ARCH}.iso"
    "$tools/viso" -v -V "$ISO_LABEL" -e "$staged/efi.img" -o "$iso" \
        "$staged/efi.img" "$staged/rootfs.img" || die "viso failed"

    # --- check it with something that is not ours ---------------------------
    if command -v xorriso >/dev/null; then
        if xorriso -indev "$iso" -report_el_torito plain 2>&1 | grep -q "El Torito boot img"; then
            ok "xorriso recognises the boot record"
        else
            warn "xorriso does not see an El Torito boot record — this will not boot"
        fi
    else
        log "xorriso not installed; skipping the independent check"
    fi

    ok "live ISO → $iso ($(du -h "$iso" | cut -f1))"
    log "boot it with: make run-iso ARCH=$ARCH"
}

run_stage "iso-${ARCH}" build_iso
