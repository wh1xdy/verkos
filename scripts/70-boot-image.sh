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

    # A shell, so urverk's emergency prompt is real in early boot — which is
    # exactly when it is needed: no root device, or a root that will not mount.
    # Costs a few MB in an image whose ESP is measured in hundreds.
    add_rescue_shell "$work"
    # An encrypted root has to be opened before there is a root to mount, so
    # cryptsetup has to be here rather than on the filesystem it unlocks.
    add_cryptsetup "$work"

    ( cd "$work" && find . | "$tools/vcpio" -o ) > "$BUILD_DIR/initramfs.cpio" \
        || die "vcpio failed"
    "$tools/vgzip" -c "$BUILD_DIR/initramfs.cpio" > "$out" || die "vgzip failed"
    ok "initramfs → $out ($(du -h "$out" | cut -f1))"
}

# Copy bash and whatever it links into the initramfs, plus verkbox for the
# everyday tools, so a rescue prompt can actually do something.
# cryptsetup and everything it names, into the initramfs. Without this an
# encrypted root cannot be opened at all: urverk prompts, forks, and finds
# nothing to exec.
add_cryptsetup() {
    local work="$1"
    local SUDO=; [ "$(id -u)" = 0 ] || SUDO=sudo
    local cs=""
    for c in "$ROOTFS/usr/sbin/cryptsetup" "$ROOTFS/sbin/cryptsetup" "$ROOTFS/usr/bin/cryptsetup"; do
        [ -x "$c" ] && { cs="$c"; break; }
    done
    [ -n "$cs" ] || { log "no cryptsetup in the rootfs — the initramfs cannot open an encrypted root"; return 0; }

    mkdir -p "$work/usr/sbin" "$work/usr/lib"
    $SUDO ln -sfn usr/sbin "$work/sbin" 2>/dev/null || true
    $SUDO cp "$cs" "$work/usr/sbin/cryptsetup"

    # Follow the whole dependency chain, not just the direct NEEDED entries:
    # libcryptsetup pulls in json-c, devmapper, popt and OpenSSL, and each of
    # those names more. A missing one only shows up as "error while loading
    # shared libraries" at the moment the disk needs unlocking.
    local pending="$work/usr/sbin/cryptsetup" seen="" lib d found
    while [ -n "$pending" ]; do
        local next=""
        for f in $pending; do
            for lib in $(readelf -d "$f" 2>/dev/null | sed -n "s/.*NEEDED.*\[\(.*\)\]/\1/p"); do
                case " $seen " in *" $lib "*) continue ;; esac
                seen="$seen $lib"
                found=""
                for d in "$ROOTFS/usr/lib" "$ROOTFS/lib" "$ROOTFS/usr/lib64"; do
                    [ -e "$d/$lib" ] && { found="$d/$lib"; break; }
                done
                if [ -n "$found" ]; then
                    $SUDO cp -L "$found" "$work/usr/lib/$lib"
                    next="$next $work/usr/lib/$lib"
                else
                    warn "cryptsetup needs $lib, which is not in the rootfs"
                fi
            done
        done
        pending="$next"
    done
    ok "initramfs carries cryptsetup ($(echo $seen | wc -w) libraries)"
    return 0
}

add_rescue_shell() {
    local work="$1"
    local SUDO=; [ "$(id -u)" = 0 ] || SUDO=sudo
    [ -x "$ROOTFS/usr/bin/bash" ] || { warn "no bash in the rootfs — the initramfs gets no rescue shell"; return 0; }

    # Mirror the real root's merged-/usr layout. The dynamic loader searches
    # /usr/lib, and bash's interpreter path points there too, so putting the
    # libraries anywhere else means "error while loading shared libraries".
    mkdir -p "$work/usr/bin" "$work/usr/lib"
    $SUDO ln -sfn usr/bin "$work/bin"
    $SUDO ln -sfn usr/lib "$work/lib"
    $SUDO ln -sfn usr/lib "$work/lib64"

    $SUDO cp "$ROOTFS/usr/bin/bash" "$work/usr/bin/bash"
    $SUDO ln -sf bash "$work/usr/bin/sh"
    [ -x "$ROOTFS/usr/bin/verkbox" ] && $SUDO cp "$ROOTFS/usr/bin/verkbox" "$work/usr/bin/verkbox"

    # the interpreter plus every library bash names
    local libs lib d
    libs=$(readelf -d "$ROOTFS/usr/bin/bash" 2>/dev/null | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p')
    libs="$libs ld-linux-aarch64.so.1 ld-linux-x86-64.so.2"
    for lib in $libs; do
        for d in "$ROOTFS/usr/lib" "$ROOTFS/lib" "$ROOTFS/usr/lib64"; do
            [ -e "$d/$lib" ] && { $SUDO cp -L "$d/$lib" "$work/usr/lib/$lib"; break; }
        done
    done
    # the loop's last test fails on whichever arch's loader is not present, and
    # that must not look like a failed function under set -e
    return 0
}

# Signing is opt-in, because the key cannot live in a public repository and a
# bootloader that trusts a key everyone has is worse than one that trusts none:
# the second is honestly unverified, the first only looks verified.
#
#     vsign keygen verkos.key verkos.pub
#     VERK_SIGN_KEY=$PWD/verkos.key make boot ARCH=aarch64
#
# The public half is generated into the build directory and vboot is compiled
# against it, so the checked-in all-zero header is never edited and a clone
# still builds an unsigned bootloader that boots.
sign_setup() {
    SIGN_PUB=""
    [ -n "${VERK_SIGN_KEY:-}" ] || return 0
    [ -f "$VERK_SIGN_KEY" ] || die "VERK_SIGN_KEY=$VERK_SIGN_KEY does not exist"

    mkdir -p "$BUILD_DIR/sign"
    VSIGN="$BUILD_DIR/sign/vsign"
    cc -O2 -w -o "$VSIGN" "$VERK_ROOT/pkg/vboot/vsign.c" \
        -I"$VERK_ROOT/pkg/vboot" || die "vsign did not build"

    SIGN_PUB="$BUILD_DIR/sign/verkos.pub"
    "$VSIGN" pubof "$VERK_SIGN_KEY" "$SIGN_PUB" >/dev/null 2>&1 || \
        die "could not derive the public key from $VERK_SIGN_KEY"
    "$VSIGN" pubhdr "$SIGN_PUB" > "$BUILD_DIR/sign/vboot-key.h" || \
        die "could not write the public key header"
    export VBOOT_KEY_DIR="$BUILD_DIR/sign"
    ok "signing with $(basename "$VERK_SIGN_KEY")"
}

# Sign what vboot will be asked to verify. Both, not just the kernel: an
# initramfs is where the root is found and the passphrase is read, so an
# unverified one is an unverified system however well the kernel is checked.
sign_payloads() {
    [ -n "${VERK_SIGN_KEY:-}" ] || return 0
    for f in "$1" "$2"; do
        "$VSIGN" sign "$VERK_SIGN_KEY" "$f" >/dev/null || die "could not sign $f"
        "$VSIGN" verify "$SIGN_PUB" "$f" >/dev/null || \
            die "signed $f and then could not verify it"
    done
    ok "signed the kernel and the initramfs, and verified both"
}

build_vboot() {
    sign_setup
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
    need_tool parted parted
    local esp="$BUILD_DIR/esp.img" img="$OUT_DIR/verkos-boot-${ARCH}.img"
    local kernel="$OUT_DIR/vmlinuz" initramfs="$OUT_DIR/initramfs.img"
    [ -f "$kernel" ] || die "no kernel at $kernel — run stage 50 first"
    local cfg; cfg="$(write_config)"

    log "building a ${ESP_MB} MiB EFI system partition"
    dd if=/dev/zero of="$esp" bs=1M count="$ESP_MB" status=none
    # -F 32, for the reason spelled out in 71-iso.sh: left to choose, dosfstools
    # made this FAT16 at the default 160 MiB, and UEFI requires FAT32 for an ESP
    # on a fixed disk.
    # Our own formatter and our own file writer: no dosfstools, no mtools.
    # vmkfs refuses anything but FAT32 rather than choosing by size, so the
    # FAT16 of D-052 cannot come back.
    local tools="$BUILD_DIR/hosttools"; mkdir -p "$tools"
    cc -O2 -o "$tools/vmkfs" "$VERK_ROOT/pkg/vmkfs/vmkfs.c" || die "could not build vmkfs for the host"
    local VMKFS="$tools/vmkfs"
    "$VMKFS" -n VERKESP "$esp"
    "$VMKFS" --mkdir "$esp" /EFI
    "$VMKFS" --mkdir "$esp" /EFI/BOOT
    "$VMKFS" --copy  "$esp" "$BUILD_DIR/vboot/$EFI_NAME" "/EFI/BOOT/$EFI_NAME"
    sign_payloads "$kernel" "$initramfs"
    "$VMKFS" --copy "$esp" "$kernel"    /vmlinuz
    "$VMKFS" --copy "$esp" "$initramfs" /initramfs.img
    "$VMKFS" --copy "$esp" "$cfg"       /vboot.conf
    # signatures, when the payloads have been signed (see pkg/vboot/vsign.c)
    [ -f "$kernel.sig" ]    && "$VMKFS" --copy "$esp" "$kernel.sig"    /vmlinuz.sig
    [ -f "$initramfs.sig" ] && "$VMKFS" --copy "$esp" "$initramfs.sig" /initramfs.img.sig

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
