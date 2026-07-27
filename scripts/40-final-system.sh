#!/usr/bin/env bash
# Stage 40 — build the final system natively inside the rootfs.
#
# Precondition: stage 30 finished (rootfs is chroot-ready). This stage does the
# LFS chapter-7/8 work: set up the chroot (virtual filesystems, directory tree,
# essential files/users), stage the sources inside it, then run a native build
# of the real system ending in urverk as PID 1.
#
# The heavy lifting is a build script generated INTO the rootfs and executed via
# chroot (native) or qemu-user chroot (foreign) — exactly how LFS uses /sources.
source "$(dirname "$0")/lib/common.sh"
mkdirs

require_ready() {
    [ -f "$STAMP_DIR/temp-tools-complete" ] || \
        die "stage 30 (temp-tools) not complete — run: make temp ARCH=$ARCH"
    [ -x "$ROOTFS/usr/bin/bash" ] || die "no bash in rootfs; temp-tools incomplete"
}

# --- LFS 7.x: directory tree, essential files, users -----------------------
prepare_tree() {
    step "Creating the directory layout + essential files"
    # Ensure the merged-/usr symlinks (/bin /lib /sbin -> usr/*) are in place and
    # NOT clobbered by a real dir — creating e.g. /lib/firmware directly would
    # turn /lib into a real directory and break the dynamic linker path.
    create_usr_layout
    sudo mkdir -p "$ROOTFS"/{boot,home,mnt,opt,srv}
    sudo mkdir -p "$ROOTFS"/etc/{opt,sysconfig}
    sudo mkdir -p "$ROOTFS"/usr/lib/firmware
    sudo mkdir -p "$ROOTFS"/media/{floppy,cdrom}
    sudo mkdir -p "$ROOTFS"/usr/{,local/}{include,src}
    sudo mkdir -p "$ROOTFS"/usr/lib/locale
    sudo mkdir -p "$ROOTFS"/usr/local/{bin,lib,sbin}
    sudo mkdir -p "$ROOTFS"/usr/{,local/}share/{color,dict,doc,info,locale,man}
    sudo mkdir -p "$ROOTFS"/usr/{,local/}share/{misc,terminfo,zoneinfo}
    sudo mkdir -p "$ROOTFS"/usr/{,local/}share/man/man{1..8}
    sudo mkdir -p "$ROOTFS"/var/{cache,local,log,mail,opt,spool}
    sudo mkdir -p "$ROOTFS"/var/lib/{color,misc,locate}
    sudo ln -sfn /run "$ROOTFS/var/run"       2>/dev/null || true
    sudo ln -sfn /run/lock "$ROOTFS/var/lock" 2>/dev/null || true
    sudo install -d -m 0750 "$ROOTFS/root"
    sudo install -d -m 1777 "$ROOTFS/tmp" "$ROOTFS/var/tmp"

    # Essential files (LFS 7.6). Created on the host, land in the chroot.
    sudo tee "$ROOTFS/etc/passwd" >/dev/null <<'EOF'
root:x:0:0:root:/root:/bin/bash
bin:x:1:1:bin:/dev/null:/usr/bin/false
daemon:x:6:6:Daemon User:/dev/null:/usr/bin/false
messagebus:x:18:18:D-Bus Message Daemon User:/run/dbus:/usr/bin/false
systemd-journal-gateway:x:73:73:systemd Journal Gateway:/:/usr/bin/false
systemd-journal-remote:x:74:74:systemd Journal Remote:/:/usr/bin/false
systemd-journal-upload:x:75:75:systemd Journal Upload:/:/usr/bin/false
systemd-network:x:76:76:systemd Network Management:/:/usr/bin/false
systemd-resolve:x:77:77:systemd Resolver:/:/usr/bin/false
systemd-timesync:x:78:78:systemd Time Synchronization:/:/usr/bin/false
systemd-coredump:x:79:79:systemd Core Dumper:/:/usr/bin/false
uuidd:x:80:80:UUID Generation Daemon User:/dev/null:/usr/bin/false
sshd:x:50:50:sshd PrivSep:/var/lib/sshd:/usr/bin/false
nobody:x:65534:65534:Unprivileged User:/dev/null:/usr/bin/false
EOF
    sudo tee "$ROOTFS/etc/group" >/dev/null <<'EOF'
root:x:0:
bin:x:1:daemon
sys:x:2:
kmem:x:3:
tape:x:4:
tty:x:5:
daemon:x:6:
disk:x:8:
lp:x:7:
dialout:x:10:
audio:x:11:
video:x:12:
utmp:x:13:
cdrom:x:15:
adm:x:16:
messagebus:x:18:
systemd-journal:x:23:
input:x:24:
mail:x:34:
kvm:x:61:
systemd-journal-gateway:x:73:
systemd-journal-remote:x:74:
systemd-journal-upload:x:75:
systemd-network:x:76:
systemd-resolve:x:77:
systemd-timesync:x:78:
systemd-coredump:x:79:
uuidd:x:80:
sshd:x:50:
wheel:x:97:
users:x:999:
nogroup:x:65534:
EOF
    sudo cp "$VERK_ROOT/config/os-release" "$ROOTFS/etc/os-release"
    echo "VerkOS ${VERSION_ID:-0.1}" | sudo tee "$ROOTFS/etc/verk-release" >/dev/null
    echo "verkos" | sudo tee "$ROOTFS/etc/hostname" >/dev/null
    # Log files LFS expects to exist.
    sudo mkdir -p "$ROOTFS/var/log"
    sudo touch "$ROOTFS/var/log"/{btmp,lastlog,faillog,wtmp}
    sudo chmod 600 "$ROOTFS/var/log/btmp" 2>/dev/null || true
    ok "directory tree + passwd/group + branding installed"
}

# --- LFS 7.3: mount the virtual kernel filesystems into the chroot ---------
VFS_MOUNTED=0
mount_vfs() {
    step "Mounting virtual kernel filesystems"
    sudo mkdir -p "$ROOTFS"/{dev,proc,sys,run}
    sudo mount -v --bind /dev  "$ROOTFS/dev"
    sudo mount -vt devpts devpts -o gid=5,mode=0620 "$ROOTFS/dev/pts"
    sudo mount -vt proc  proc  "$ROOTFS/proc"
    sudo mount -vt sysfs sysfs "$ROOTFS/sys"
    sudo mount -vt tmpfs tmpfs "$ROOTFS/run"
    [ -h "$ROOTFS/dev/shm" ] && sudo mkdir -p "$ROOTFS/$(readlink "$ROOTFS/dev/shm")"
    VFS_MOUNTED=1
}
umount_vfs() {
    [ "$VFS_MOUNTED" = 1 ] || return 0
    step "Unmounting virtual kernel filesystems"
    sudo umount -R "$ROOTFS/dev"  2>/dev/null || true
    sudo umount    "$ROOTFS/proc" 2>/dev/null || true
    sudo umount    "$ROOTFS/sys"  2>/dev/null || true
    sudo umount    "$ROOTFS/run"  2>/dev/null || true
    VFS_MOUNTED=0
}
trap umount_vfs EXIT

# --- Stage sources into the chroot -----------------------------------------
stage_sources() {
    step "Staging sources into the rootfs (/sources)"
    sudo mkdir -p "$ROOTFS/sources"
    # Bind-mount so we don't duplicate GBs of tarballs into the rootfs.
    sudo mount --bind "$SRC_DIR" "$ROOTFS/sources" 2>/dev/null \
        || sudo cp -a "$SRC_DIR/." "$ROOTFS/sources/"
    # Make version numbers available to the in-chroot script.
    sudo cp "$VERK_ROOT/config/versions.sh" "$ROOTFS/sources/versions.sh"
    # Stage vpk (the Verk package manager) source + recipes so the in-chroot
    # script can compile and install it against VerkOS' own libs.
    sudo cp "$VERK_ROOT/pkg/vpk/vpk.c" "$ROOTFS/sources/vpk.c" 2>/dev/null || true
    sudo rm -rf "$ROOTFS/sources/vpk-recipes"
    sudo cp -a "$VERK_ROOT/pkg/recipes" "$ROOTFS/sources/vpk-recipes" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/verkbox/verkbox.c" "$ROOTFS/sources/verkbox.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vgetty/vgetty.c" "$ROOTFS/sources/vgetty.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vdhcp/vdhcp.c" "$ROOTFS/sources/vdhcp.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vip/vip.c" "$ROOTFS/sources/vip.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vdig/vdig.c" "$ROOTFS/sources/vdig.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vping/vping.c" "$ROOTFS/sources/vping.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vtrace/vtrace.c" "$ROOTFS/sources/vtrace.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vss/vss.c" "$ROOTFS/sources/vss.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vnetcat/vnetcat.c" "$ROOTFS/sources/vnetcat.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vmon/vmon.c" "$ROOTFS/sources/vmon.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vget/vget.c" "$ROOTFS/sources/vget.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/ved/ved.c" "$ROOTFS/sources/ved.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vwhois/vwhois.c" "$ROOTFS/sources/vwhois.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vsed/vsed.c" "$ROOTFS/sources/vsed.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vfind/vfind.c" "$ROOTFS/sources/vfind.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vxargs/vxargs.c" "$ROOTFS/sources/vxargs.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vdate/vdate.c" "$ROOTFS/sources/vdate.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vstat/vstat.c" "$ROOTFS/sources/vstat.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vsys/vsys.c" "$ROOTFS/sources/vsys.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vhex/vhex.c" "$ROOTFS/sources/vhex.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vtree/vtree.c" "$ROOTFS/sources/vtree.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vpager/vpager.c" "$ROOTFS/sources/vpager.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vfb/vfb.c" "$ROOTFS/sources/vfb.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/verkmod/verkmod.c" "$ROOTFS/sources/verkmod.c" 2>/dev/null || true
    for f in vlogin_core.c vlogin_core.h vlogin.c su.c passwd.c; do
        sudo cp "$VERK_ROOT/pkg/vlogin/$f" "$ROOTFS/sources/$f" 2>/dev/null || true
    done
    sudo cp "$VERK_ROOT/pkg/vmkfs/vmkfs.c"     "$ROOTFS/sources/vmkfs.c"     2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vinstall/vinstall.c" "$ROOTFS/sources/vinstall.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vpart/vpart.c"     "$ROOTFS/sources/vpart.c"     2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/urverk/urverk.c" "$ROOTFS/sources/urverk.c" 2>/dev/null || true
    sudo rm -rf "$ROOTFS/sources/urverk-services"
    sudo cp -a "$VERK_ROOT/pkg/urverk/services" "$ROOTFS/sources/urverk-services" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vgzip/vgzip.c" "$ROOTFS/sources/vgzip.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vzip/vzip.c" "$ROOTFS/sources/vzip.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vtar/vtar.c" "$ROOTFS/sources/vtar.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vcpio/vcpio.c" "$ROOTFS/sources/vcpio.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vawk/vawk.c" "$ROOTFS/sources/vawk.c" 2>/dev/null || true
    sudo cp "$VERK_ROOT/pkg/vbzip2/vbzip2.c" "$ROOTFS/sources/vbzip2.c" 2>/dev/null || true
}
unstage_sources() { sudo umount "$ROOTFS/sources" 2>/dev/null || true; }

# --- Generate the native build script that runs INSIDE the chroot ----------
write_native_build_script() {
    step "Writing the in-chroot native build script"
    # This runs with a native compiler (no --host); it builds the real system.
    # Written faithfully to LFS ch.8. The critical path to a booting system is
    # implemented; extend with the remaining userland
    # packages (FINAL_SYSTEM_SEQUENCE, below) following the same pattern.
    sudo tee "$ROOTFS/root/build-native.sh" >/dev/null <<'NATIVE'
#!/bin/bash
set -euo pipefail
source /sources/versions.sh
cd /sources
export PATH=/usr/bin:/usr/sbin
# The chroot build runs as root; coreutils (and a few others) refuse to configure
# as root unless this is set. Safe here — the whole final system is built as root.
export FORCE_UNSAFE_CONFIGURE=1
# Where pkgconf (built below) should look for .pc files installed by our packages.
export PKG_CONFIG_PATH=/usr/lib/pkgconfig:/usr/share/pkgconfig
JOBS=$(nproc 2>/dev/null || echo 2); export MAKEFLAGS="-j$JOBS"

# Per-package stamps so a re-run (after fixing a later package) skips the ones
# already installed instead of rebuilding the whole chain. Wrap a block as:
#   if need X; then ...build...; mark X; fi
# Per-package build stamps. MUST live inside the rootfs (per-architecture), NOT
# under /sources (which is shared across arches) — otherwise an x86_64 build
# would skip packages already stamped by the aarch64 build and ship a rootfs
# missing systemd et al.
NST=/var/lib/vpk-build-stamps; mkdir -p "$NST"
need(){ [ -f "$NST/$1" ] && { echo ":: $1 already built, skipping"; return 1; } || return 0; }
mark(){ touch "$NST/$1"; }
say(){ printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
# Extract a tarball and echo where it landed. This runs in a command
# substitution, so the cd is local to the subshell — its job is to make the
# result independent of the caller's working directory. Returning a relative
# path used to work only because every package block ended with `cd /sources`;
# the first block after a run of already-built (skipped) ones inherited whatever
# directory the script started in, and its `cd "$d"` failed.
unpack(){ cd /sources; tar -xf "$1"; local d="${1%.tar.*}"; d="${d%.tgz}"; echo "/sources/$d"; }

# 1. glibc — SKIPPED. The cross-toolchain (stage 20) already installed a working
# glibc ${GLIBC_VERSION} into the rootfs from the same source. A native rebuild
# here is LFS hygiene (removes cross-build artifacts) but is NOT required to
# boot, and doing it first would need bison + python built as chroot temporary
# tools (LFS chapter 7) which we don't build yet. Getting to first boot takes
# priority; a proper ch7-temp-tools + ch8-glibc rebuild is a later improvement.
say "glibc ${GLIBC_VERSION} (using cross-installed libc; native rebuild skipped)"
test -f /usr/lib/libc.so.6 || { echo "!! no libc in rootfs — stage 20 incomplete" >&2; exit 1; }
cd /sources

# pkgconf — provides pkg-config, needed by kmod, dbus, systemd. Built early so
# it's on PATH for everything after. Zero dependencies beyond a C compiler.
if need pkgconf; then
say "pkgconf ${PKGCONF_VERSION}"
d=$(unpack pkgconf-${PKGCONF_VERSION}.tar.xz); cd "$d"
./configure --prefix=/usr --disable-static
make; make install
ln -sfv pkgconf /usr/bin/pkg-config
cd /sources
mark pkgconf
fi

# 2-4. ncurses + bash + coreutils (native)
if need core-userland; then
say "ncurses ${NCURSES_VERSION}"
d=$(unpack ncurses-${NCURSES_VERSION}.tar.gz); cd "$d"
./configure --prefix=/usr --mandir=/usr/share/man --with-shared \
    --without-debug --without-normal --with-cxx-shared --enable-pc-files
make; make install
cd /sources

# 3. bash (native)
say "bash ${BASH_VERSION_PKG}"
d=$(unpack bash-${BASH_VERSION_PKG}.tar.gz); cd "$d"
./configure --prefix=/usr --without-bash-malloc --with-installed-readline=no
# Serially, on purpose. bash's rule for pathnames.h generates pathnames.tmp
# and moves it, and with a high -j two paths into that rule race: one moves the
# file while the other still expects it, and the build dies with
# "mv: cannot stat 'pathnames.tmp'". It only appears on machines with many
# cores — it never happened on the 8-core builder and failed immediately on 36.
MAKEFLAGS=-j1 make; make install
ln -sfv bash /usr/bin/sh
cd /sources

# 4. coreutils (native)
say "coreutils ${COREUTILS_VERSION}"
d=$(unpack coreutils-${COREUTILS_VERSION}.tar.xz); cd "$d"
./configure --prefix=/usr --enable-no-install-program=kill,uptime
make; make install
cd /sources
mark core-userland
fi

# 4b. Core userland (LFS ch.8 subset) --------------------------------------
# This rebuilds the full compiler + GNU userland into the final system. None of
# it is needed to BOOT (temp-tools from stage 30 already put sed/grep/gawk/tar/
# coreutils/bash/etc. in the rootfs; a compiler isn't needed at runtime), and it
# is by far the slowest, most failure-prone part. Skip it by default to reach
# first boot fast; set VERK_FULL_USERLAND=1 to build the complete userland.
if [ "${VERK_FULL_USERLAND:-0}" = "1" ] && need full-userland; then
# bzip2 (LFS recipe: shared lib + binaries)
say "bzip2 ${BZIP2_VERSION}"
d=$(unpack bzip2-${BZIP2_VERSION}.tar.gz); cd "$d"
# Clean first, not just between the two builds. unpack reuses an existing
# source directory, so a previous run that got as far as the static build and
# then failed elsewhere leaves non-PIC object files behind — and the shared
# build finds them up to date, skips recompiling, and links them into a shared
# object. x86_64 refuses that outright ("relocation R_X86_64_PC32 ... recompile
# with -fPIC"); aarch64 accepts it, which is why this only ever appeared on the
# second architecture.
make clean 2>/dev/null || true
make -f Makefile-libbz2_so
make clean
make
make PREFIX=/usr install
# Clear the targets first: cp refuses to overwrite an existing symlink, which
# a rerun of this block always hits. (--remove-destination is GNU-only, and cp
# here is verkbox's, which rightly rejects flags it does not implement.)
rm -f /usr/lib/libbz2.so.*
cp -av libbz2.so.* /usr/lib
ln -sfv libbz2.so.${BZIP2_VERSION} /usr/lib/libbz2.so
cp -v bzip2-shared /usr/bin/bzip2
ln -sfv bzip2 /usr/bin/bzcat
ln -sfv bzip2 /usr/bin/bunzip2
rm -f /usr/lib/libbz2.a
cd /sources

# file (native)
say "file ${FILE_VERSION}"
d=$(unpack file-${FILE_VERSION}.tar.gz); cd "$d"
./configure --prefix=/usr; make; make install; cd /sources

# m4 (native)
say "m4 ${M4_VERSION}"
d=$(unpack m4-${M4_VERSION}.tar.xz); cd "$d"
./configure --prefix=/usr; make; make install; cd /sources

# zlib, before anything that asks for a system one.
#
# binutils below is configured --with-system-zlib and gcc links against it, so
# zlib has to exist first. It is otherwise built further down with the other
# small libraries, which was fine for years because every rootfs that reached
# this point already had one from a previous build — and wrong the first time
# the tree was genuinely empty. Guarded on the header rather than a stamp so it
# is a no-op wherever zlib is already installed.
if [ ! -f /usr/include/zlib.h ]; then
    say "zlib ${ZLIB_VERSION} (needed by binutils and gcc)"
    d=$(unpack "zlib-${ZLIB_VERSION}.tar.gz"); cd "$d"
    ./configure --prefix=/usr; make; make install; cd /sources
fi

# binutils (final, native)
say "binutils ${BINUTILS_VERSION} (final)"
d=$(unpack binutils-${BINUTILS_VERSION}.tar.xz); cd "$d"
mkdir -p build && cd build
../configure --prefix=/usr --sysconfdir=/etc --enable-ld=default \
    --enable-plugins --enable-shared --disable-werror --enable-64-bit-bfd \
    --enable-new-dtags --with-system-zlib --enable-default-hash-style=gnu \
    --enable-gprofng=no
# MAKEINFO=true: binutils and gcc build info manuals, which need texinfo.
# VerkOS does not ship it, and the manuals are not worth another package.
make MAKEINFO=true tooldir=/usr; make MAKEINFO=true tooldir=/usr install
rm -fv /usr/lib/lib{bfd,ctf,ctf-nobfd,opcodes,sframe}.a
cd /sources

# gcc (final, native) — gmp/mpfr/mpc bundled in-tree
say "gcc ${GCC_VERSION} (final)"
d=$(unpack gcc-${GCC_VERSION}.tar.xz); cd "$d"
# Clear any graft from an earlier attempt: mv would otherwise move the new
# tree *inside* the old one and gcc would configure against nothing.
rm -rf gmp mpfr mpc
tar -xf ../gmp-${GMP_VERSION}.tar.xz  && mv gmp-${GMP_VERSION}  gmp
tar -xf ../mpfr-${MPFR_VERSION}.tar.xz && mv mpfr-${MPFR_VERSION} mpfr
tar -xf ../mpc-${MPC_VERSION}.tar.gz  && mv mpc-${MPC_VERSION}  mpc
case $(uname -m) in
    x86_64) sed -e '/m64=/s/lib64/lib/' -i.orig gcc/config/i386/t-linux64 ;;
esac
# Start from an empty build directory. Generated headers from a previous
# attempt survive otherwise, and make considers them up to date — a run
# after fixing the generator still compiles against the broken output.
rm -rf build && mkdir build && cd build
# --with-stage1-ldflags=no: configure otherwise links gcc's own build tools
# with -static-libstdc++, and the target libstdc++.a that came from the cross
# toolchain is a stub — it has neither sized delete nor basic_string, so the
# generators fail to link. Building them against the shared library works and
# is what we want anyway, since this build replaces libstdc++ a moment later.
../configure --prefix=/usr LD=ld --with-stage1-ldflags=no --enable-languages=c,c++ \
    --enable-default-pie --enable-default-ssp \
    --disable-multilib --disable-bootstrap --disable-fixincludes \
    --with-system-zlib
make MAKEINFO=true; make MAKEINFO=true install
ln -sfv gcc /usr/bin/cc
cd /sources

# The cross toolchain's libstdc++ is built before glibc exists, so it has no
# thread support at all: _GLIBCXX_HAS_GTHREADS is undefined and std::mutex,
# std::thread and std::condition_variable do not exist. This native rebuild is
# what fixes that, so check it rather than assume — a compiler that silently
# cannot build threaded C++ is worse than one that is missing.
printf '#include <mutex>\n#include <thread>\nint main(){ std::mutex m; std::lock_guard<std::mutex> g(m); return 0; }\n' > /tmp/thrcheck.cpp
if g++ -O2 -o /tmp/thrcheck /tmp/thrcheck.cpp 2>/dev/null; then
    echo ":: libstdc++ has working threads"
else
    echo "!! the native gcc did not give libstdc++ threads — std::mutex is missing" >&2
    exit 1
fi
rm -f /tmp/thrcheck /tmp/thrcheck.cpp

# bison then flex (flex uses bison), then the GNU text/file tools
say "bison ${BISON_VERSION}"
d=$(unpack bison-${BISON_VERSION}.tar.xz); cd "$d"
./configure --prefix=/usr; make; make install; cd /sources
say "flex ${FLEX_VERSION}"
d=$(unpack flex-${FLEX_VERSION}.tar.gz); cd "$d"
./configure --prefix=/usr --disable-static
make; make install
ln -sfv flex /usr/bin/lex
cd /sources

# sed grep gawk diffutils findutils tar gzip make patch (all: configure/make/install)
for spec in "sed:${SED_VERSION}:xz" "grep:${GREP_VERSION}:xz" \
            "gawk:${GAWK_VERSION}:xz" "diffutils:${DIFFUTILS_VERSION}:xz" \
            "findutils:${FINDUTILS_VERSION}:xz" "tar:${TAR_VERSION}:xz" \
            "gzip:${GZIP_VERSION}:xz" "make:${MAKE_VERSION}:gz" \
            "patch:${PATCH_VERSION}:xz"; do
    name=${spec%%:*}; rest=${spec#*:}; ver=${rest%:*}; ext=${rest##*:}
    say "$name $ver"
    d=$(unpack ${name}-${ver}.tar.${ext}); cd "$d"
    ./configure --prefix=/usr
    make; make install
    cd /sources
done
mark full-userland
fi   # end VERK_FULL_USERLAND

# 5. util-linux (systemd needs libmount/libblkid/libuuid)
if need util-linux; then
say "util-linux ${UTIL_LINUX_VERSION}"
d=$(unpack util-linux-${UTIL_LINUX_VERSION}.tar.xz); cd "$d"
./configure --libdir=/usr/lib --runstatedir=/run --disable-chfn-chsh \
    --disable-login --disable-nologin --disable-su --disable-setpriv \
    --disable-runuser --disable-pylibmount --disable-static \
    --disable-liblastlog2 \
    --without-python --without-systemd --without-systemdsystemunitdir \
    ADJTIME_PATH=/var/lib/hwclock/adjtime
make; make install
cd /sources
mark util-linux
fi

# 5b. e2fsprogs — mkfs.ext4 and e2fsck. Needed to install VerkOS onto a disk,
# and it switches on the boot-time filesystem check urverk already implements
# but skips when no fsck is present. libblkid/libuuid/uuidd/fsck come from
# util-linux above, so they are disabled here rather than built twice.
if need e2fsprogs; then
say "e2fsprogs ${E2FSPROGS_VERSION}"
d=$(unpack e2fsprogs-${E2FSPROGS_VERSION}.tar.xz); cd "$d"
mkdir -p build; cd build
../configure --prefix=/usr --sysconfdir=/etc --enable-elf-shlibs \
    --disable-libblkid --disable-libuuid --disable-uuidd --disable-fsck
make
make install
ldconfig
cd /sources
mark e2fsprogs
fi


# 5c. LUKS support: popt, json-c, libdevmapper and cryptsetup. Only needed so
# the installer can create an encrypted root and urverk can open it at boot.
if need popt; then
say "popt ${POPT_VERSION}"
d=$(unpack popt-${POPT_VERSION}.tar.gz); cd "$d"
./configure --prefix=/usr --disable-static
make; make install
ldconfig; cd /sources; mark popt
fi

# cmake is build tooling, like meson/ninja/python: json-c has been cmake-only
# since 0.14, and json-c is what cryptsetup parses LUKS2 headers with. Stage 60
# deletes it from the image again.
if need cmake; then
say "cmake ${CMAKE_VERSION} (build tooling for json-c)"
d=$(unpack cmake-${CMAKE_VERSION}.tar.gz); cd "$d"
./bootstrap --prefix=/usr --parallel=$(nproc 2>/dev/null || echo 2) --no-qt-gui
make; make install
cd /sources; mark cmake
fi

if need json-c; then
say "json-c ${JSONC_VERSION}"
d=$(unpack json-c-${JSONC_VERSION}.tar.gz); cd "$d"
mkdir -p build; cd build
# json-c is cmake-only; configure by hand is not an option, so use the cmake
# we have if there is one, and fall back to the plain autotools files it ships.
cmake -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib \
      -DBUILD_STATIC_LIBS=ON -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release ..
make; make install
ldconfig; cd /sources; mark json-c
fi

# Only libdevmapper is wanted here, not LVM itself: cryptsetup talks to the
# kernel's device mapper through it, and nothing in VerkOS uses logical volumes.
if need libdevmapper; then
say "libdevmapper (from LVM2 ${LVM2_VERSION})"
d=$(unpack LVM2.${LVM2_VERSION}.tgz); cd "$d"
./configure --prefix=/usr --enable-pkgconfig --disable-selinux \
    --disable-udev_sync --with-udev-prefix=/usr --disable-nls || \
    ./configure --prefix=/usr --enable-pkgconfig --disable-selinux --disable-udev_sync
make device-mapper
make install_device-mapper
ldconfig; cd /sources; mark libdevmapper
fi

if need cryptsetup; then
say "cryptsetup ${CRYPTSETUP_VERSION}"
d=$(unpack cryptsetup-${CRYPTSETUP_VERSION}.tar.xz); cd "$d"
# OpenSSL is already built and is the crypto backend; the extra tools are not
# wanted, and neither is the systemd integration that no longer exists here.
./configure --prefix=/usr --disable-ssh-token --disable-asciidoc \
    --with-crypto_backend=openssl --disable-static
make; make install
ldconfig; cd /sources; mark cryptsetup
fi

# 6-7b. zlib, xz, expat, libcap, gperf, libffi — link/build deps of the rest.
# Stamped as a group: one-time builds that must NOT rebuild on a `make system`
# re-run (kmod's libtool chokes re-linking over a dirty tree, and the re-run's
# verkbox tar can't re-extract .xz), so guard the whole block behind one stamp.
if need systemd-deps; then
for pkg in "zlib-${ZLIB_VERSION}.tar.gz" "xz-${XZ_VERSION}.tar.xz"; do
    say "$pkg"; d=$(unpack "$pkg"); cd "$d"
    ./configure --prefix=/usr; make; make install; cd /sources
done
say "expat ${EXPAT_VERSION}"
d=$(unpack expat-${EXPAT_VERSION}.tar.xz); cd "$d"
./configure --prefix=/usr --disable-static; make; make install; cd /sources
say "libcap ${LIBCAP_VERSION}"
d=$(unpack libcap-${LIBCAP_VERSION}.tar.xz); cd "$d"
make prefix=/usr lib=lib; make prefix=/usr lib=lib install; cd /sources
# kmod — REMOVED. It was built as a systemd dependency; with systemd gone
# nothing in VerkOS loads a kernel module: no udev, no modprobe call in any
# script, service or recipe. See the note about /lib/modules below.
# gperf — systemd build dep
say "gperf ${GPERF_VERSION}"
d=$(unpack gperf-${GPERF_VERSION}.tar.gz); cd "$d"
./configure --prefix=/usr; make; make install; cd /sources
# libffi — Python's _ctypes dep. --libdir=/usr/lib: libffi otherwise installs to
# /usr/lib64, off the linker path in our merged-/usr (lib, not lib64) layout, so
# Python's _ctypes fails to import (libffi.so.8 not found). Refresh ld cache after.
say "libffi ${LIBFFI_VERSION}"
d=$(unpack libffi-${LIBFFI_VERSION}.tar.gz); cd "$d"
./configure --prefix=/usr --libdir=/usr/lib --disable-static
make; make install
printf '/usr/lib\n/usr/local/lib\n' > /etc/ld.so.conf
ldconfig
cd /sources
mark systemd-deps
fi

# 7b. Python build toolchain for systemd (meson/ninja/jinja2) ---------------

if need python; then
say "Python ${PYTHON_VERSION}"
d=$(unpack Python-${PYTHON_VERSION}.tar.xz); cd "$d"
./configure --prefix=/usr --enable-shared --with-system-expat \
    --with-ensurepip=yes --without-static-libpython
make; make install
cd /sources
mark python
fi

# ninja and meson used to run unguarded, which made stage 40 impossible to
# resume: stage 60 slims the rootfs and deletes python3/meson/ninja from it, so
# every later run reached this point and died on "python3: command not found".
# Guarded like every other package, they are simply skipped once built — and a
# resume that genuinely needs them now fails with a message that says so.
if need ninja; then
say "ninja ${NINJA_VERSION}"
d=$(unpack ninja-${NINJA_VERSION}.tar.gz); cd "$d"
command -v python3 >/dev/null || {
    echo "!! python3 is gone from this rootfs (stage 60 slims it away)." >&2
    echo "!! Rebuild from a clean rootfs to change anything that needs meson." >&2
    exit 1; }
python3 configure.py --bootstrap
install -vm755 ninja /usr/bin/
cd /sources
mark ninja
fi

if need meson; then
say "meson + jinja2 (offline pip from /sources/pip)"
if [ -d /sources/pip ]; then
    # setuptools+wheel (universal wheels) first, so markupsafe's sdist builds
    # against the target Python 3.12. jinja2 + meson install from wheels.
    pip3 install --no-index --find-links /sources/pip setuptools wheel
    pip3 install --no-index --no-build-isolation --find-links /sources/pip \
        markupsafe jinja2 meson
else
    echo "!! /sources/pip missing — run 'make fetch' on a host with pip first" >&2
    exit 1
fi
command -v meson >/dev/null && command -v ninja >/dev/null \
    || { echo "!! meson/ninja not available after install" >&2; exit 1; }
cd /sources
mark meson
fi

# 8. D-Bus (systemd's IPC bus) — dbus 1.16 uses meson, not autotools.
if need dbus; then
say "dbus ${DBUS_VERSION}"
d=$(unpack dbus-${DBUS_VERSION}.tar.xz); cd "$d"
meson setup build --prefix=/usr --buildtype=release \
    --sysconfdir=/etc --localstatedir=/var \
    -Ddoxygen_docs=disabled -Dxml_docs=disabled -Dsystemd=disabled
ninja -C build; ninja -C build install; cd /sources
mark dbus
fi

# 8.4 perl — needed by libxcrypt's configure (and generally). Stamped: slow.
if need perl; then
say "perl ${PERL_VERSION}"
d=$(unpack perl-${PERL_VERSION}.tar.xz); cd "$d"
sh Configure -des -Dprefix=/usr -Dusethreads
make; make install
cd /sources
mark perl
fi

# 8.5 libxcrypt — provides libcrypt/crypt.h. Modern glibc (2.38+) builds without
# the crypt add-on, and systemd needs the 'crypt' library. LFS uses libxcrypt.
if need libxcrypt; then
say "libxcrypt ${LIBXCRYPT_VERSION}"
d=$(unpack libxcrypt-${LIBXCRYPT_VERSION}.tar.xz); cd "$d"
./configure --prefix=/usr --enable-hashes=strong,glibc \
    --enable-obsolete-api=no --disable-static --disable-failure-tokens
make; make install
ldconfig
cd /sources
mark libxcrypt
fi

# 8.6 vlogin — our own login/su/passwd/useradd, replacing shadow entirely.
# Built here rather than with the other VerkOS tools further down because the
# root password is set later in this same script, and that needs pwconv and
# chpasswd to already exist. Links libxcrypt (8.5) for the hashing itself.
if need vlogin; then
say "vlogin (VerkOS account tools: login/su/passwd/useradd/... replaces shadow)"
# Hardening, and it is not decoration on these three. _FORTIFY_SOURCE turns a
# class of buffer mistakes into a clean abort instead of a corrupted stack;
# stack-protector-strong catches the ones it cannot see statically; PIE plus
# full RELRO and BIND_NOW mean an attacker who does get a write has no fixed
# addresses to aim at and no writable relocation table to redirect. Two of
# these binaries run as root on behalf of someone who is not.
VLOGIN_HARDEN="-O2 -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fPIE -Wall"
VLOGIN_LINK="-pie -Wl,-z,relro,-z,now,-z,noexecstack"

# Three binaries out of one library. su and passwd have a main() each that can
# only ever do one thing; the multi-call tool keeps its argv[0] dispatch and
# does NOT get the setuid bit. See D-051.
gcc $VLOGIN_HARDEN $VLOGIN_LINK -o /usr/bin/vlogin \
    /sources/vlogin.c /sources/vlogin_core.c -I/sources -lcrypt
gcc $VLOGIN_HARDEN $VLOGIN_LINK -o /usr/bin/su \
    /sources/su.c /sources/vlogin_core.c -I/sources -lcrypt
gcc $VLOGIN_HARDEN $VLOGIN_LINK -o /usr/bin/passwd \
    /sources/passwd.c /sources/vlogin_core.c -I/sources -lcrypt
# Names that only need to reach the binary. The account-management tools also
# answer from /usr/sbin, where shadow put them and where scripts look for them.
for t in login chpasswd useradd usermod userdel groupadd groupdel pwconv nologin; do
    ln -sf vlogin "/usr/bin/$t"
done
for t in chpasswd useradd usermod userdel groupadd groupdel pwconv nologin; do
    ln -sf ../bin/vlogin "/usr/sbin/$t"
done
# su and passwd are the only setuid binaries in VerkOS. An ordinary user runs
# them and they must read and write /etc/shadow, which is 0640 root-owned.
#
# They are separately compiled programs, not copies of the multi-call binary.
# A copy would still contain every other tool and choose between them from
# argv[0] — which the caller supplies. See D-051.
for t in su passwd; do
    chown root:root "/usr/bin/$t"
    chmod 4755 "/usr/bin/$t"
done
# Nothing else may carry it, and several packages install one without asking.
# A first inventory of a built rootfs found four beyond ours:
#
#   /usr/bin/mount, /usr/bin/umount     util-linux, so a user can mount
#   /usr/libexec/ssh-keysign            openssh, only for host-based auth
#   /usr/libexec/dbus-daemon-launch-helper   dbus system-bus activation
#
# None of them are wanted here. VerkOS mounts filesystems from urverk and from
# vinstall, both of which already run as root; host-based ssh authentication is
# not configured; and nothing activates services over the system bus. Each is a
# root-privileged program on the disk of every installation, carried for a
# feature nobody asked for — which is the definition of attack surface.
#
# The bit is removed rather than the programs: mount(8) is still useful to
# root, and deleting it would be a surprise. Losing setuid only means an
# ordinary user cannot invoke it, which is the intent.
say "removing the setuid bit from everything except su and passwd"
find / -xdev -perm -4000 -type f 2>/dev/null | while read -r f; do
    case "$f" in
        /usr/bin/su|/usr/bin/passwd) continue ;;
    esac
    echo "  setuid removed: $f"
    chmod u-s "$f"
done
# And then insist on it, because a silent inventory is not an inventory.
left=$(find / -xdev -perm -4000 -type f 2>/dev/null | grep -v "^/usr/bin/su$" | grep -v "^/usr/bin/passwd$" || true)
if [ -n "$left" ]; then
    echo "! setuid binaries that should not exist:" >&2
    echo "$left" >&2
    exit 1
fi
# /bin and /sbin are symlinks into /usr, so /bin/login (what vgetty execs) and
# /sbin/nologin (a shell for accounts that must not be used) already resolve.
cd /sources
mark vlogin
fi

# 8.7 Linux-PAM — pluggable authentication (systemd-logind + login use it).
if need linuxpam; then
say "Linux-PAM ${LINUXPAM_VERSION}"
d=$(unpack Linux-PAM-${LINUXPAM_VERSION}.tar.xz); cd "$d"
./configure --prefix=/usr --sysconfdir=/etc --disable-doc \
    --enable-securedir=/usr/lib/security
make
make install
ldconfig
cd /sources
mark linuxpam
fi

# 9. systemd — PID 1 (needs meson+ninja+python-jinja2 present in the chroot).
#    Built with PAM (logind sessions) now that Linux-PAM is available.
# systemd — REMOVED. VerkOS' own init is urverk (pkg/urverk); nothing in the
# base system links libsystemd any more (shadow is built --disable-logind), so
# systemd is neither built nor installed. /sbin/init is urverk, services live in
# /etc/urverk/services, and urverk-ctl replaces systemctl.
# Point /sbin/init at urverk, VerkOS' own init.
ln -sfv /sbin/urverk /usr/sbin/init
cd /sources

# 10. Networking + SSH -------------------------------------------------------
# OpenSSL (crypto for OpenSSH). install_sw/install_ssldirs skip the slow docs.
if need openssl; then
say "openssl ${OPENSSL_VERSION}"
d=$(unpack openssl-${OPENSSL_VERSION}.tar.gz); cd "$d"
./config --prefix=/usr --openssldir=/etc/ssl --libdir=lib shared zlib-dynamic
make
make install_sw install_ssldirs
ldconfig
cd /sources
mark openssl
fi

# OpenSSH — sshd + ssh client. Needs openssl + zlib.
if need openssh; then
say "openssh ${OPENSSH_VERSION}"
d=$(unpack openssh-${OPENSSH_VERSION}.tar.gz); cd "$d"
./configure --prefix=/usr --sysconfdir=/etc/ssh \
    --with-privsep-path=/var/lib/sshd --with-privsep-user=sshd \
    --with-ssl-dir=/usr --with-zlib
make
install -d -m 0700 /var/lib/sshd
make install
cd /sources
mark openssh
fi

# dhcpcd — the ACTIVE DHCP/DNS client (chosen over systemd-networkd; first step
# toward replacing systemd's networking).
# dhcpcd — REMOVED from the base system. vdhcp is the DHCP client and runs as
# an urverk service; dhcpcd was installed and then never started by anything.
# Still available as a vpk port: `vpk install dhcpcd`.

# 11. Extra userland tools ---------------------------------------------------
# bison + flex — parser/lexer generators (needed by iproute2 and others). Also
# built in the full-userland block, but they're build tools several packages
# need, so build them here too (stamped) even without VERK_FULL_USERLAND.
if need bison; then
say "bison ${BISON_VERSION}"
d=$(unpack bison-${BISON_VERSION}.tar.xz); cd "$d"
./configure --prefix=/usr
make
make install
ln -sfv bison /usr/bin/yacc
cd /sources
mark bison
fi
if need flex; then
say "flex ${FLEX_VERSION}"
d=$(unpack flex-${FLEX_VERSION}.tar.gz); cd "$d"
./configure --prefix=/usr --disable-static
make
make install
ln -sfv flex /usr/bin/lex
cd /sources
mark flex
fi

# iproute2 — ip, ss
# iproute2 — REMOVED from the base system. Its two headline commands were
# already shadowed: `ip` -> vip and `ss`/`netstat` -> vss (see the netln block
# below), so the package only shipped binaries the system renamed out of the
# way. Still available as a vpk port for anyone who wants tc/bridge/netns,
# which vip does not cover: `vpk install iproute2`.

# iputils — ping (meson build; trim deps/docs to just the tools)
if need iputils; then
say "iputils ${IPUTILS_VERSION}"
d=$(unpack iputils-${IPUTILS_VERSION}.tar.gz); cd "$d"
meson setup build --prefix=/usr --buildtype=release \
    -DSKIP_TESTS=true -DBUILD_MANS=false -DBUILD_HTML_MANS=false \
    -DUSE_CAP=false -DUSE_IDN=false -DNO_SETCAP_OR_SUID=true
ninja -C build
ninja -C build install
cd /sources
mark iputils
fi

# curl — HTTP/HTTPS downloads (uses our openssl + zlib)
if need curl; then
say "curl ${CURL_VERSION}"
d=$(unpack curl-${CURL_VERSION}.tar.xz); cd "$d"
./configure --prefix=/usr --with-openssl --with-zlib --without-libpsl \
    --with-ca-path=/etc/ssl/certs --disable-static --enable-optimize
make
make install
cd /sources
mark curl
fi

# procps-ng — removed: our own vmon (ps/top/free/vmstat/uptime) and vsys
# (pgrep/pkill/pidof/…) take over every name it provided, so building it is
# dead weight. The name takeovers live near the end of this script.

# less — removed: replaced by our own vpager (installed as less/more/pg via
# netln, near the end of this script). Last non-native base tool retired.

# nano — simple editor
if need nano; then
say "nano ${NANO_VERSION}"
d=$(unpack nano-${NANO_VERSION}.tar.xz); cd "$d"
./configure --prefix=/usr --sysconfdir=/etc --enable-utf8
make
make install
cd /sources
mark nano
fi

# 12. vpk — the Verk package manager. Self-contained C, compiled against our own
# libcurl/liblzma/zlib. Ships a starter recipe set under /etc/vpk/recipes.
if need vpk; then
say "vpk (Verk package manager)"
gcc -O2 -o /usr/bin/vpk /sources/vpk.c -lcurl -llzma -lz
ln -sfv vpk /usr/bin/verk
mkdir -p /etc/vpk/recipes
cp -a /sources/vpk-recipes/. /etc/vpk/recipes/ 2>/dev/null || true
rm -f /etc/vpk/recipes/README.md
cd /sources
mark vpk
fi

# 12b. verkbox — VerkOS' own multi-call core utilities (BusyBox-style). Our first
# native userland tool. Build the binary here; its applets take over from the GNU
# coreutils names at the very end of this script (so the build itself keeps using
# GNU's tools, and only the shipped image runs ours).
if need verkbox; then
say "verkbox (VerkOS core utilities)"
gcc -O2 -o /usr/bin/verkbox /sources/verkbox.c
cd /sources
mark verkbox
fi

# 12c. vgetty — VerkOS' own getty (replaces agetty for the console login).
# A leaf process systemd execs on the console; hands off to /bin/login. First
# step of making our init userland ours (see serial-getty autologin drop-in).
if need vgetty; then
say "vgetty (VerkOS getty)"
gcc -O2 -o /usr/bin/vgetty /sources/vgetty.c
cd /sources
mark vgetty
fi

# 12d. vdhcp — VerkOS' own minimal DHCPv4 client (replaces dhcpcd). Raw
# AF_PACKET handshake so it can lease before an address exists; applies the
# lease via ioctls and writes /etc/resolv.conf. Second step of making our
# networking ours (see vdhcp.service below, which supersedes dhcpcd).
if need vdhcp; then
say "vdhcp (VerkOS DHCP client)"
gcc -O2 -o /usr/sbin/vdhcp /sources/vdhcp.c
cd /sources
mark vdhcp
fi

# 12e. vip — VerkOS' own network config tool (an ip/ifconfig in one binary).
# Shows/configures interfaces, addresses, and routes via AF_INET ioctls; pairs
# with vdhcp. Shipped as its own command (iproute2's `ip` stays for the bits vip
# doesn't cover yet).
if need vip; then
say "vip (VerkOS ip/ifconfig)"
gcc -O2 -o /usr/bin/vip /sources/vip.c
cd /sources
mark vip
fi

# 12f. vdig — VerkOS' own DNS lookup tool (a dig/host/nslookup). Builds DNS
# queries and parses responses itself over UDP.
if need vdig; then
say "vdig (VerkOS DNS lookup)"
gcc -O2 -o /usr/bin/vdig /sources/vdig.c
cd /sources
mark vdig
fi

# 12g/h/i. vping/vtrace/vss — the rest of the network toolbox: ICMP ping, ICMP
# traceroute, and an ss/netstat socket lister. Raw sockets + /proc parsing.
if need vping; then
say "vping (VerkOS ping)"
gcc -O2 -o /usr/bin/vping /sources/vping.c
cd /sources
mark vping
fi
if need vtrace; then
say "vtrace (VerkOS traceroute)"
gcc -O2 -o /usr/bin/vtrace /sources/vtrace.c
cd /sources
mark vtrace
fi
if need vss; then
say "vss (VerkOS ss/netstat)"
gcc -O2 -o /usr/bin/vss /sources/vss.c
cd /sources
mark vss
fi

# 12j/k/l. vnetcat (nc), vmon (procps: free/uptime/vmstat/ps/top, multi-call),
# and vget (curl/wget over HTTP + HTTPS via OpenSSL).
if need vnetcat; then
say "vnetcat (VerkOS netcat)"
gcc -O2 -o /usr/bin/vnetcat /sources/vnetcat.c
cd /sources
mark vnetcat
fi
if need vmon; then
say "vmon (VerkOS process/system monitors)"
gcc -O2 -o /usr/bin/vmon /sources/vmon.c
cd /sources
mark vmon
fi
if need vget; then
say "vget (VerkOS HTTP/HTTPS fetcher)"
gcc -O2 -o /usr/bin/vget /sources/vget.c -lssl -lcrypto
cd /sources
mark vget
fi

# 12m-q. ved (vi editor), vwhois, vsed, vfind, vxargs. vsed is a full sed engine
# (hold space, N/D/P, branching, y, a/i/c, ...) and is build-verified — hello
# builds from source with it as `sed` — so it takes over the name (below).
if need ved; then
say "ved (VerkOS vi)"
gcc -O2 -o /usr/bin/ved /sources/ved.c
cd /sources; mark ved
fi
if need vwhois; then
say "vwhois (VerkOS whois)"
gcc -O2 -o /usr/bin/vwhois /sources/vwhois.c
cd /sources; mark vwhois
fi
if need vsed; then
say "vsed (VerkOS sed)"
gcc -O2 -o /usr/bin/vsed /sources/vsed.c
cd /sources; mark vsed
fi
if need vfind; then
say "vfind (VerkOS find)"
gcc -O2 -o /usr/bin/vfind /sources/vfind.c
cd /sources; mark vfind
fi
if need vxargs; then
say "vxargs (VerkOS xargs)"
gcc -O2 -o /usr/bin/vxargs /sources/vxargs.c
cd /sources; mark vxargs
fi
if need vdate; then
say "vdate (VerkOS date)"
gcc -O2 -o /usr/bin/vdate /sources/vdate.c
cd /sources; mark vdate
fi
if need vstat; then
say "vstat (VerkOS stat)"
gcc -O2 -o /usr/bin/vstat /sources/vstat.c
cd /sources; mark vstat
fi
if need vsys; then
say "vsys (VerkOS df/dmesg/pgrep/pkill/killall/pidof/watch)"
gcc -O2 -o /usr/bin/vsys /sources/vsys.c
cd /sources; mark vsys
fi
if need vhex; then
say "vhex (VerkOS hexdump/xxd)"
gcc -O2 -o /usr/bin/vhex /sources/vhex.c
cd /sources; mark vhex
fi
if need vtree; then
say "vtree (VerkOS tree)"
gcc -O2 -o /usr/bin/vtree /sources/vtree.c
cd /sources; mark vtree
fi
if need vpager; then
say "vpager (VerkOS pager — our own less/more/pg)"
gcc -O2 -o /usr/bin/vpager /sources/vpager.c
cd /sources; mark vpager
fi
if need vfb; then
say "vfb (VerkOS framebuffer splash — first GUI)"
gcc -O2 -o /usr/bin/vfb /sources/vfb.c
cd /sources; mark vfb
fi
# verkmod — our kernel module tools. kmod went with systemd; without a loader the
# modules the kernel build installs are unreachable.
# vmkfs.vfat — our own FAT32 formatter, so an EFI system partition can be
# created without dosfstools. See docs/DECISIONS.md D-014.
if need vmkfs; then
say "vmkfs (VerkOS FAT32 formatter)"
gcc -O2 -o /usr/bin/vmkfs.vfat /sources/vmkfs.c
for t in mkfs.vfat mkfs.fat mkfs.msdos; do ln -sf vmkfs.vfat "/usr/bin/$t"; done
for t in mkfs.vfat mkfs.fat mkfs.msdos vmkfs.vfat; do ln -sf ../bin/vmkfs.vfat "/usr/sbin/$t"; done
cd /sources; mark vmkfs
fi

# vpart — our own GPT writer. The installer needs a partition table before it
# needs anything else, and sfdisk was the last third-party tool in that path
# (D-066). Built before vinstall because vinstall refuses to run without it.
if need vpart; then
say "vpart (VerkOS GPT writer)"
gcc -O2 -o /usr/bin/vpart /sources/vpart.c
ln -sf ../bin/vpart /usr/sbin/vpart
cd /sources; mark vpart
fi

# vinstall — the installer: GPT + ESP + ext4 root + vboot, onto a real disk.
if need vinstall; then
say "vinstall (VerkOS installer)"
gcc -O2 -o /usr/bin/vinstall /sources/vinstall.c
ln -sf ../bin/vinstall /usr/sbin/vinstall
cd /sources; mark vinstall
fi

if need verkmod; then
say "verkmod (VerkOS module tools: insmod/rmmod/lsmod/modinfo/modprobe/depmod)"
gcc -O2 -o /usr/bin/verkmod /sources/verkmod.c
cd /sources; mark verkmod
fi
# urverk — VerkOS' own init (the systemd replacement we are growing). Static so
# the same binary works as the initramfs /init. Installed alongside systemd, not
# instead of it: boot it with init=/sbin/urverk (vboot has a menu entry).
if need urverk; then
say "urverk (VerkOS init — stage 1 initramfs + stage 2 PID 1)"
gcc -O2 -static -o /sbin/urverk /sources/urverk.c
# same binary as the control client: `urverk-ctl status`, `urverk-ctl restart sshd`
ln -sf /sbin/urverk /usr/bin/urverk-ctl
ln -sf /sbin/urverk /usr/bin/urverk
mkdir -p /etc/urverk/services
cp -a /sources/urverk-services/. /etc/urverk/services/ 2>/dev/null || true
cd /sources; mark urverk
fi
if need vgzip; then
say "vgzip (VerkOS gzip/gunzip/zcat — own DEFLATE)"
gcc -O2 -o /usr/bin/vgzip /sources/vgzip.c
cd /sources; mark vgzip
fi
if need vzip; then
say "vzip (VerkOS zip/unzip — reuses DEFLATE)"
gcc -O2 -o /usr/bin/vzip /sources/vzip.c
cd /sources; mark vzip
fi
if need vtar; then
say "vtar (VerkOS tar — ustar)"
gcc -O2 -o /usr/bin/vtar /sources/vtar.c
cd /sources; mark vtar
fi
if need vcpio; then
say "vcpio (VerkOS cpio — newc)"
gcc -O2 -o /usr/bin/vcpio /sources/vcpio.c
cd /sources; mark vcpio
fi
if need vawk; then
say "vawk (VerkOS awk — own interpreter)"
gcc -O2 -o /usr/bin/vawk /sources/vawk.c -lm
cd /sources; mark vawk
fi
if need vbzip2; then
say "vbzip2 (VerkOS bzip2/bunzip2/bzcat — own BWT codec)"
gcc -O2 -o /usr/bin/vbzip2 /sources/vbzip2.c
cd /sources; mark vbzip2
fi

# --- System configuration (LFS ch.9 essentials) ---------------------------
say "System configuration files"
cat > /etc/fstab <<'FSTAB'
# file-system   mount-point   type    options              dump  fsck
tmpfs           /run          tmpfs   defaults             0     0
proc            /proc         proc    nosuid,noexec,nodev  0     0
sysfs           /sys          sysfs   nosuid,noexec,nodev  0     0
devpts          /dev/pts      devpts  gid=5,mode=620       0     0
FSTAB

# Initial DNS fallback (vdhcp overwrites this at runtime with DHCP-provided
# servers). The "generated by vdhcp" marker tells vdhcp this file is safe to
# replace; if an admin later writes their own resolv.conf without the marker,
# vdhcp leaves it alone. rm -f first: /etc/resolv.conf may be a dangling symlink
# from a previous networkd config, which would make the redirect fail.
rm -f /etc/resolv.conf
cat > /etc/resolv.conf <<'RC'
# generated by vdhcp
nameserver 1.1.1.1
nameserver 9.9.9.9
RC

ln -sfv /usr/share/zoneinfo/UTC /etc/localtime

# CA certificate bundle (Mozilla, via curl.se) so HTTPS verification works for
# vpk, curl and anything using OpenSSL's default paths.
mkdir -p /etc/ssl/certs
if [ -f /sources/cacert.pem ]; then
    cp /sources/cacert.pem /etc/ssl/certs/ca-certificates.crt
    ln -sfv ca-certificates.crt /etc/ssl/cert.pem
fi

cat > /etc/ld.so.conf <<'LD'
/usr/local/lib
/opt/lib
LD
ldconfig || true

# PAM configuration (Linux-PAM). Minimal functional stack: pam_unix against
# /etc/shadow, so login/logind sessions authenticate properly. Unknown services
# fall through to 'other' which denies.
mkdir -p /etc/pam.d
cat > /etc/pam.d/system-auth     <<'P'
auth      required    pam_unix.so
P
cat > /etc/pam.d/system-account  <<'P'
account   required    pam_unix.so
P
cat > /etc/pam.d/system-session  <<'P'
session   required    pam_unix.so
P
cat > /etc/pam.d/system-password <<'P'
password  required    pam_unix.so sha512 shadow
P
cat > /etc/pam.d/login <<'P'
auth      include     system-auth
account   include     system-account
session   include     system-session
password  include     system-password
P
cat > /etc/pam.d/urverk-user <<'P'
account   include     system-account
session   include     system-session
P
cat > /etc/pam.d/other <<'P'
auth      required    pam_warn.so
auth      required    pam_deny.so
account   required    pam_warn.so
account   required    pam_deny.so
password  required    pam_warn.so
password  required    pam_deny.so
session   required    pam_warn.so
session   required    pam_deny.so
P

# Minimal shell environment.
cat > /etc/profile <<'PROF'
export PATH=/usr/local/bin:/usr/bin:/usr/local/sbin:/usr/sbin
export PS1='\u@\h:\w\$ '
export LANG=C.UTF-8
export SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt
# Greet interactive logins with verkfetch (VerkOS' neofetch).
if [ -x /usr/bin/verkfetch ]; then
    case "$-" in *i*) verkfetch ;; esac
fi
PROF

# Machine id: kept, it is a generic /etc/machine-id that other tools read.
[ -s /etc/machine-id ] || echo uninitialized > /etc/machine-id

# Users/passwords: create /etc/shadow from /etc/passwd, then set a known root
# password. DEV DEFAULT: root / "verkos" — CHANGE BEFORE ANY REAL USE.
sed -i 's|^root::|root:x:|' /etc/passwd    # undo the earlier empty-password hack
if [ -x /usr/bin/vlogin ]; then
    /usr/bin/vlogin pwconv
    echo "root:verkos" | /usr/bin/vlogin chpasswd
else
    # Without an account tool there is no way to write a hash, and leaving
    # root:x: with no shadow entry would lock the system out entirely.
    sed -i 's|^root:x:|root::|' /etc/passwd
fi

# Serial-console autologin: drop straight to a root shell on boot (handy for dev
# and demos). Applies to any serial-getty@ instance regardless of tty name.
# Console autologin is urverk's console service (vgetty --autologin root).

# D-Bus system bus. Our dbus is built WITHOUT systemd integration (that would
# pull in glib), so it CANNOT do socket activation: --address=systemd: and
# --systemd-activation require systemd support and make dbus-daemon exit with
# "compiled without systemd support". Instead run it as a plain service that
# creates and listens on /run/dbus/system_bus_socket itself (address comes from
# the built-in system.conf). systemd-logind & friends connect there.
mkdir -p /usr/lib/tmpfiles.d

# systemd ships dbus policy files (e.g. oom1) referencing service users that our
# minimal base lacks, making dbus warn "Unknown username systemd-oom". Create the
# ones referenced so the config loads cleanly.
for u in systemd-oom:996 systemd-network:995 systemd-resolve:994; do
    name="${u%%:*}"; uid="${u##*:}"
    grep -q "^$name:" /etc/passwd || \
        echo "$name:x:$uid:$uid:$name:/:/usr/bin/false" >> /etc/passwd
    grep -q "^$name:" /etc/group || echo "$name:x:$uid:" >> /etc/group
done


# --- Networking: vdhcp is ACTIVE (our own DHCP client). dhcpcd and
# --- systemd-networkd are both built but left disabled as swappable options —
# --- concrete steps toward replacing systemd's networking with our own.
# (/etc/resolv.conf already created above; vdhcp overwrites it at runtime.)

# /etc/vdhcp.conf — vdhcp reads this for the interface list and tunables. With
# no 'interfaces=' line it defaults to eth0; list several to run one worker per
# NIC. All keys are optional.
cat > /etc/vdhcp.conf <<'VC'
# VerkOS DHCP client configuration. See pkg/vdhcp/vdhcp.c for the full option set.
# interfaces=eth0 eth1     # NICs to lease on (default: eth0)
# hostname=verkos          # hostname advertised to the server (default: system hostname)
# lease=86400              # requested lease time in seconds (option 51)
# vendor=VerkOS            # vendor class identifier (option 60)
# fqdn=verkos.local        # FQDN for dynamic DNS (option 81)
# hook=/etc/vdhcp/hook     # script run on BOUND/RENEW/REBIND/RELEASE/EXPIRE
# request=26,42,119,121    # extra option codes to request
# metric=0                 # default-route metric (higher = lower priority)
# resolv=yes               # manage /etc/resolv.conf (no = leave it to the admin/hook)
# rapid=no                 # try RFC 4039 rapid commit (2-message lease)
# inform=no                # DHCPINFORM mode: fetch options for a static address
VC

# vdhcp.service — VerkOS' own DHCP client, the active one. Config-driven: the
# interface list (and tunables) come from /etc/vdhcp.conf.
# vdhcp, sshd and the console login are urverk services (built in, and in
# /etc/urverk/services) — no unit files needed.

# --- Register the base system in vpk's database ----------------------------
# So `vpk list` shows the whole OS and base packages are protected from removal
# (base=yes, no file manifest). Versions come from the sourced versions.sh.
reg() { mkdir -p /var/lib/vpk/db/"$1"; printf 'name=%s\nversion=%s\nbase=yes\n' "$1" "$2" > /var/lib/vpk/db/"$1"/meta; }
reg linux "$LINUX_VERSION";        reg glibc "$GLIBC_VERSION"
reg binutils "$BINUTILS_VERSION";  reg gcc "$GCC_VERSION"
reg ncurses "$NCURSES_VERSION";    reg bash "$BASH_VERSION_PKG"
reg coreutils "$COREUTILS_VERSION";reg bzip2 "$BZIP2_VERSION"
reg file "$FILE_VERSION";          reg m4 "$M4_VERSION"
reg bison "$BISON_VERSION";        reg flex "$FLEX_VERSION"
reg sed "$SED_VERSION";            reg grep "$GREP_VERSION"
reg gawk "$GAWK_VERSION";          reg diffutils "$DIFFUTILS_VERSION"
reg findutils "$FINDUTILS_VERSION";reg tar "$TAR_VERSION"
reg gzip "$GZIP_VERSION";          reg make "$MAKE_VERSION"
reg patch "$PATCH_VERSION";        reg util-linux "$UTIL_LINUX_VERSION"
reg zlib "$ZLIB_VERSION";          reg xz "$XZ_VERSION"
reg expat "$EXPAT_VERSION";        reg libcap "$LIBCAP_VERSION"
reg gperf "$GPERF_VERSION"
reg pkgconf "$PKGCONF_VERSION";    reg libffi "$LIBFFI_VERSION"
reg python "$PYTHON_VERSION";      reg ninja "$NINJA_VERSION"
reg perl "$PERL_VERSION";          reg libxcrypt "$LIBXCRYPT_VERSION"
reg vlogin 0.1;                    reg linux-pam "$LINUXPAM_VERSION"
reg e2fsprogs "$E2FSPROGS_VERSION"
reg cryptsetup "$CRYPTSETUP_VERSION"; reg popt "$POPT_VERSION"
reg json-c "$JSONC_VERSION";          reg lvm2 "$LVM2_VERSION"
reg dbus "$DBUS_VERSION";          reg urverk 0.1
reg openssl "$OPENSSL_VERSION";    reg openssh "$OPENSSH_VERSION"

reg iputils "$IPUTILS_VERSION";    reg curl "$CURL_VERSION"
reg nano "$NANO_VERSION"

# verkbox applets take over from GNU coreutils for these tool names. Done last, so
# the build above used GNU's tools while the shipped image runs VerkOS' own.
if [ -x /usr/bin/verkbox ]; then
    say "verkbox (VerkOS core utilities)"
    # verkbox ships as our own userland. Per-tool takeover: a tool only shadows the
    # GNU coreutils once its applet is flag-complete AND differentially byte-exact vs
    # GNU across ALL its common flags (an earlier wholesale takeover regressed e.g.
    # `ls -l` with "unknown option"). `ls` now qualifies — verified byte-exact for
    # -l/-a/-A/-1/-R/-r/-t/-S/-d/-h/-F and --color — so it takes over. The other
    # applets ship (callable as `verkbox <applet>`) but stay GNU-backed until each
    # clears the same bar: flag-complete AND differentially byte-exact vs GNU across
    # all common flags (incl. the ones autotools/configure use, like `ls -i`).
    # Every applet verkbox ships is flag-complete, differentially byte-exact vs
    # GNU, AND verified build-safe: hello + tree build AND install from source with
    # verkbox shadowing GNU (incl. the cp -a / mkdir -p that `make install` leans
    # on). So the whole set takes over /usr/bin. GNU's coreutils binaries remain
    # installed; only these names are symlinked to verkbox.
    for t in $(/usr/bin/verkbox --list | sed -n 's/^applets: *//p'); do
        ln -sf verkbox /usr/bin/"$t"
    done
    reg verkbox 0.1; reg vgetty 0.1; reg vdhcp 0.1; reg vip 0.1; reg vdig 0.1
    reg vping 0.1; reg vtrace 0.1; reg vss 0.1
    reg vnetcat 0.1; reg vmon 0.1; reg vget 0.1
    reg ved 0.1; reg vwhois 0.1; reg vsed 0.1; reg vfind 0.1; reg vxargs 0.1
    reg vdate 0.1; reg vstat 0.1
    reg vsys 0.1; reg vhex 0.1; reg vtree 0.1; reg vgzip 0.1
    reg vzip 0.1; reg vtar 0.1; reg vcpio 0.1; reg vawk 0.1; reg vbzip2 0.1
    reg vpager 0.1; reg vfb 0.1; reg urverk 0.1; reg verkmod 0.1
    reg vmkfs 0.1;  reg vinstall 0.1
fi

# The network tools take over the FAMILIAR command names, the same way verkbox
# owns `ls`/`cp` (not `vls`/`vcp`): the vX binary is the real file, and the
# everyday name (ip, ping, dig, ...) is a symlink to it, so muscle memory just
# works. Any pre-existing real binary is kept as <name>.orig to revert to.
if [ -x /usr/bin/vip ]; then
    netln() {   # netln FAMILIAR-NAME  OUR-TOOL
        local p; p=$(command -v "$1" 2>/dev/null || true)
        [ -n "$p" ] && [ ! -L "$p" ] && mv "$p" "$p.orig"
        ln -sf "/usr/bin/$2" "${p:-/usr/bin/$1}"
    }
    netln ip vip
    netln ping vping
    netln traceroute vtrace
    netln ss vss
    netln netstat vss
    netln dig vdig
    netln host vdig
    netln nslookup vdig
    netln nc vnetcat
    netln netcat vnetcat
    netln curl vget
    netln wget vget
    # vmon is multi-call (dispatches on argv[0]) — link each monitor name to it.
    for m in free uptime vmstat ps top; do netln "$m" vmon; done
    # vi/vim (no editor but nano before), whois (was missing), and find/xargs
    # (build-verified). NOT sed: our vsed is a subset that breaks autoconf, so
    # GNU sed stays and ours is available as `vsed`.
    netln vi ved
    netln vim ved
    netln whois vwhois
    netln find vfind
    netln xargs vxargs
    netln sed vsed        # full engine, build-verified (hello configure passes)
    netln date vdate      # build-verified
    netln stat vstat      # build-verified
    for t in df dmesg pgrep pkill killall pidof watch; do netln "$t" vsys; done
    netln hexdump vhex
    netln xxd vhex
    netln tree vtree
    # module tools: nothing else provides these now that kmod is gone
    for m in insmod rmmod lsmod modinfo modprobe depmod; do netln "$m" verkmod; done
    # our own pager (raw-terminal less/more): paging, regex search, pan, multi-file.
    netln less vpager
    netln more vpager
    netln pg vpager
    # gzip/gunzip/zcat: our own DEFLATE, build-verified (decompresses source
    # tarballs; hello builds through it). Interoperates with standard gzip both ways.
    netln gzip vgzip
    netln gunzip vgzip
    netln zcat vgzip
    # archives: zip/unzip/cpio were missing; tar is build-verified (GNU reads our
    # tars and our tar/cpio round-trip). Our vzip interops with standard zip.
    netln zip vzip
    netln unzip vzip
    netln cpio vcpio
    netln tar vtar
    netln awk vawk       # own interpreter, build-verified (hello configure passes)
    # bzip2/bunzip2/bzcat were missing; our BWT codec interops with real bzip2
    # both ways (compress+decompress verified vs bzip2 and python bz2).
    netln bzip2 vbzip2
    netln bunzip2 vbzip2
    netln bzcat vbzip2
fi

# Every package is installed by now, so the unpacked trees under /sources are
# scratch — and they are not small: gcc's build directory alone reached 46 GB on
# a 79 GB volume, which is what stopped this build the first time round. The
# tarballs stay; anything here can be unpacked again. Only on success, so a
# failed build still has its tree to look at. VERK_KEEP_SRC=1 keeps them.
if [ -z "${VERK_KEEP_SRC:-}" ]; then
    freed=0
    for d in /sources/*/; do
        n=${d%/}
        case "${n##*/}" in pip|vpk-recipes) continue ;; esac
        [ -d "$n" ] || continue
        sz=$(du -sm "$n" 2>/dev/null | cut -f1)
        rm -rf "$n" 2>/dev/null && freed=$((freed + ${sz:-0}))
    done
    [ "$freed" -gt 0 ] && echo "==> reclaimed ${freed} MB of unpacked sources (tarballs kept)"
fi

echo
echo "==> Native build complete: full core userland + urverk as /usr/sbin/init"
NATIVE
    sudo chmod +x "$ROOTFS/root/build-native.sh"
}

# The authoritative full package order for the final system (checklist).
# The native script above implements the critical path; the rest of userland is
# layered on using the identical in-chroot pattern.
FINAL_SYSTEM_SEQUENCE=(
    glibc zlib bzip2 xz ncurses bash coreutils
    sed grep gawk diffutils findutils tar gzip make patch m4 bison flex
    binutils gcc file gperf expat libcap util-linux dbus urverk
)

require_ready
prepare_tree
mount_vfs
stage_sources
write_native_build_script

step "Entering chroot to build the native system (this is the long one)"
if is_native; then
    log "native chroot"
else
    ensure_binfmt
    log "foreign chroot via ${QEMU_USER}-static"
fi

# systemd's build tools (meson/ninja/jinja2) are built inside the chroot by the
# native script: libffi+Python+ninja from source, meson/jinja2 via offline pip
# from the sdists that 'make fetch' staged under sources/pip/. Verify they exist.
if [ ! -d "$SRC_DIR/pip" ]; then
    warn "sources/pip/ is missing — the python build tools weren't staged."
    warn "Run 'make fetch' on a host with pip before the final-system build."
fi

if enter_rootfs '/root/build-native.sh'; then
    stamp_set "final-system-complete"
    # Install verkfetch (VerkOS' neofetch). It's a repo script, not a fetched
    # package, so copy it in from the host side; /etc/profile runs it on login.
    if [ -f "$VERK_ROOT/tools/verkfetch" ]; then
        sudo install -m 0755 "$VERK_ROOT/tools/verkfetch" "$ROOTFS/usr/bin/verkfetch"
        ok "installed verkfetch → /usr/bin/verkfetch"
    fi
    ok "Final system built — urverk is /usr/sbin/init"
else
    warn "Native build stopped before completion — inspect the log above."
    warn "Re-run 'make system ARCH=$ARCH' to resume (completed packages are installed)."
    NATIVE_INCOMPLETE=1
fi

unstage_sources
umount_vfs
echo
# Stop here rather than let 'make all' go on to build a kernel and an image out
# of a rootfs that is missing packages: an image that is quietly incomplete is
# worse than no image, because it looks finished.
if [ "${NATIVE_INCOMPLETE:-0}" = 1 ]; then
    die "the final system is incomplete — not continuing to the kernel and image"
fi
ok "Stage 40 finished for $ARCH"
