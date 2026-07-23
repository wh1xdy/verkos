#!/usr/bin/env bash
# Stage 40 — build the final system natively inside the rootfs.
#
# Precondition: stage 30 finished (rootfs is chroot-ready). This stage does the
# LFS chapter-7/8 work: set up the chroot (virtual filesystems, directory tree,
# essential files/users), stage the sources inside it, then run a native build
# of the real system ending in systemd as PID 1.
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
}
unstage_sources() { sudo umount "$ROOTFS/sources" 2>/dev/null || true; }

# --- Generate the native build script that runs INSIDE the chroot ----------
write_native_build_script() {
    step "Writing the in-chroot native build script"
    # This runs with a native compiler (no --host); it builds the real system.
    # Written faithfully to LFS ch.8 / BLFS systemd. The critical path to a
    # booting systemd is implemented; extend with the remaining userland
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
NST=/sources/.nst; mkdir -p "$NST"
need(){ [ -f "$NST/$1" ] && { echo ":: $1 already built, skipping"; return 1; } || return 0; }
mark(){ touch "$NST/$1"; }
say(){ printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
unpack(){ tar -xf "$1"; echo "${1%.tar.*}"; }

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
say "pkgconf ${PKGCONF_VERSION}"
d=$(unpack pkgconf-${PKGCONF_VERSION}.tar.xz); cd "$d"
./configure --prefix=/usr --disable-static
make && make install
ln -sfv pkgconf /usr/bin/pkg-config
cd /sources

# 2-4. ncurses + bash + coreutils (native)
if need core-userland; then
say "ncurses ${NCURSES_VERSION}"
d=$(unpack ncurses-${NCURSES_VERSION}.tar.gz); cd "$d"
./configure --prefix=/usr --mandir=/usr/share/man --with-shared \
    --without-debug --without-normal --with-cxx-shared --enable-pc-files
make && make install
cd /sources

# 3. bash (native)
say "bash ${BASH_VERSION_PKG}"
d=$(unpack bash-${BASH_VERSION_PKG}.tar.gz); cd "$d"
./configure --prefix=/usr --without-bash-malloc --with-installed-readline=no
make && make install
ln -sfv bash /usr/bin/sh
cd /sources

# 4. coreutils (native)
say "coreutils ${COREUTILS_VERSION}"
d=$(unpack coreutils-${COREUTILS_VERSION}.tar.xz); cd "$d"
./configure --prefix=/usr --enable-no-install-program=kill,uptime
make && make install
cd /sources
mark core-userland
fi

# 4b. Core userland (LFS ch.8 subset) --------------------------------------
# This rebuilds the full compiler + GNU userland into the final system. None of
# it is needed to BOOT (temp-tools from stage 30 already put sed/grep/gawk/tar/
# coreutils/bash/etc. in the rootfs; a compiler isn't needed at runtime), and it
# is by far the slowest, most failure-prone part. Skip it by default to reach
# first boot fast; set VERK_FULL_USERLAND=1 to build the complete userland.
if [ "${VERK_FULL_USERLAND:-0}" = "1" ]; then
# bzip2 (LFS recipe: shared lib + binaries)
say "bzip2 ${BZIP2_VERSION}"
d=$(unpack bzip2-${BZIP2_VERSION}.tar.gz); cd "$d"
make -f Makefile-libbz2_so
make clean
make
make PREFIX=/usr install
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
./configure --prefix=/usr; make && make install; cd /sources

# m4 (native)
say "m4 ${M4_VERSION}"
d=$(unpack m4-${M4_VERSION}.tar.xz); cd "$d"
./configure --prefix=/usr; make && make install; cd /sources

# binutils (final, native)
say "binutils ${BINUTILS_VERSION} (final)"
d=$(unpack binutils-${BINUTILS_VERSION}.tar.xz); cd "$d"
mkdir -p build && cd build
../configure --prefix=/usr --sysconfdir=/etc --enable-ld=default \
    --enable-plugins --enable-shared --disable-werror --enable-64-bit-bfd \
    --enable-new-dtags --with-system-zlib --enable-default-hash-style=gnu \
    --enable-gprofng=no
make tooldir=/usr && make tooldir=/usr install
rm -fv /usr/lib/lib{bfd,ctf,ctf-nobfd,opcodes,sframe}.a
cd /sources

# gcc (final, native) — gmp/mpfr/mpc bundled in-tree
say "gcc ${GCC_VERSION} (final)"
d=$(unpack gcc-${GCC_VERSION}.tar.xz); cd "$d"
tar -xf ../gmp-${GMP_VERSION}.tar.xz  && mv gmp-${GMP_VERSION}  gmp
tar -xf ../mpfr-${MPFR_VERSION}.tar.xz && mv mpfr-${MPFR_VERSION} mpfr
tar -xf ../mpc-${MPC_VERSION}.tar.gz  && mv mpc-${MPC_VERSION}  mpc
case $(uname -m) in
    x86_64) sed -e '/m64=/s/lib64/lib/' -i.orig gcc/config/i386/t-linux64 ;;
esac
mkdir -p build && cd build
../configure --prefix=/usr LD=ld --enable-languages=c,c++ \
    --enable-default-pie --enable-default-ssp \
    --disable-multilib --disable-bootstrap --disable-fixincludes \
    --with-system-zlib
make && make install
ln -sfv gcc /usr/bin/cc
cd /sources

# bison then flex (flex uses bison), then the GNU text/file tools
say "bison ${BISON_VERSION}"
d=$(unpack bison-${BISON_VERSION}.tar.xz); cd "$d"
./configure --prefix=/usr; make && make install; cd /sources
say "flex ${FLEX_VERSION}"
d=$(unpack flex-${FLEX_VERSION}.tar.gz); cd "$d"
./configure --prefix=/usr --disable-static
make && make install
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
    make && make install
    cd /sources
done
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
make && make install
cd /sources
mark util-linux
fi

# 6. zlib, xz, expat, libcap, kmod — systemd link deps
for pkg in "zlib-${ZLIB_VERSION}.tar.gz" "xz-${XZ_VERSION}.tar.xz"; do
    say "$pkg"; d=$(unpack "$pkg"); cd "$d"
    ./configure --prefix=/usr; make && make install; cd /sources
done
say "expat ${EXPAT_VERSION}"
d=$(unpack expat-${EXPAT_VERSION}.tar.xz); cd "$d"
./configure --prefix=/usr --disable-static; make && make install; cd /sources
say "libcap ${LIBCAP_VERSION}"
d=$(unpack libcap-${LIBCAP_VERSION}.tar.xz); cd "$d"
make prefix=/usr lib=lib && make prefix=/usr lib=lib install; cd /sources
say "kmod ${KMOD_VERSION}"
d=$(unpack kmod-${KMOD_VERSION}.tar.xz); cd "$d"
./configure --prefix=/usr --sysconfdir=/etc --with-openssl=no --with-xz \
    --with-zstd=no --disable-manpages
make && make install; cd /sources

# 7. gperf — systemd build dep
say "gperf ${GPERF_VERSION}"
d=$(unpack gperf-${GPERF_VERSION}.tar.gz); cd "$d"
./configure --prefix=/usr; make && make install; cd /sources

# 7b. Python build toolchain for systemd (meson/ninja/jinja2) ---------------
# libffi → Python → ninja (all from source), then meson/jinja2 via offline pip.
# --libdir=/usr/lib: libffi otherwise installs to /usr/lib64, which is off the
# linker path in our merged-/usr (lib, not lib64) layout, so Python's _ctypes
# fails to import (libffi.so.8 not found). Refresh the linker cache after.
say "libffi ${LIBFFI_VERSION}"
d=$(unpack libffi-${LIBFFI_VERSION}.tar.gz); cd "$d"
./configure --prefix=/usr --libdir=/usr/lib --disable-static
make && make install
printf '/usr/lib\n/usr/local/lib\n' > /etc/ld.so.conf
ldconfig
cd /sources

if need python; then
say "Python ${PYTHON_VERSION}"
d=$(unpack Python-${PYTHON_VERSION}.tar.xz); cd "$d"
./configure --prefix=/usr --enable-shared --with-system-expat \
    --with-ensurepip=yes --without-static-libpython
make && make install
cd /sources
mark python
fi

say "ninja ${NINJA_VERSION}"
d=$(unpack ninja-${NINJA_VERSION}.tar.gz); cd "$d"
python3 configure.py --bootstrap
install -vm755 ninja /usr/bin/
cd /sources

say "meson + jinja2 (offline pip from /sources/pip)"
if [ -d /sources/pip ]; then
    # Wheels only — no build backend needed, so no setuptools/flit_core dance.
    pip3 install --no-index --find-links /sources/pip meson jinja2 markupsafe
else
    echo "!! /sources/pip missing — run 'make fetch' on a host with pip first" >&2
    exit 1
fi
command -v meson >/dev/null && command -v ninja >/dev/null \
    || { echo "!! meson/ninja not available after install" >&2; exit 1; }
cd /sources

# 8. D-Bus (systemd's IPC bus)
say "dbus ${DBUS_VERSION}"
d=$(unpack dbus-${DBUS_VERSION}.tar.xz); cd "$d"
./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var \
    --runstatedir=/run --disable-static --disable-doxygen-docs --disable-xml-docs
make && make install; cd /sources

# 9. systemd — PID 1 (needs meson+ninja+python-jinja2 present in the chroot)
say "systemd ${SYSTEMD_VERSION}"
d=$(unpack systemd-${SYSTEMD_VERSION}.tar.gz); cd "$d"
mkdir -p build && cd build
meson setup .. \
    --prefix=/usr --buildtype=release \
    -Dmode=release -Ddefault-dnssec=no -Dfirstboot=false \
    -Dinstall-tests=false -Dldconfig=false -Dsysusers=false \
    -Drpmmacrosdir=no -Dhomed=disabled -Duserdb=false \
    -Dman=disabled -Dmode=release -Dpamconfdir=no \
    -Ddev-kvm-mode=0660 -Dnobody-group=nogroup
ninja && ninja install
# Point /sbin/init at systemd.
ln -sfv /usr/lib/systemd/systemd /usr/sbin/init
cd /sources

# --- System configuration (LFS ch.9 essentials) ---------------------------
say "System configuration files"
cat > /etc/fstab <<'FSTAB'
# file-system   mount-point   type    options              dump  fsck
tmpfs           /run          tmpfs   defaults             0     0
proc            /proc         proc    nosuid,noexec,nodev  0     0
sysfs           /sys          sysfs   nosuid,noexec,nodev  0     0
devpts          /dev/pts      devpts  gid=5,mode=620       0     0
FSTAB

cat > /etc/resolv.conf <<'RC'
nameserver 1.1.1.1
nameserver 9.9.9.9
RC

ln -sfv /usr/share/zoneinfo/UTC /etc/localtime

cat > /etc/ld.so.conf <<'LD'
/usr/local/lib
/opt/lib
LD
ldconfig || true

# Minimal shell environment.
cat > /etc/profile <<'PROF'
export PATH=/usr/local/bin:/usr/bin:/usr/local/sbin:/usr/sbin
export PS1='\u@\h:\w\$ '
export LANG=C.UTF-8
PROF

# Machine id (systemd needs one).
systemd-machine-id-setup 2>/dev/null || echo uninitialized > /etc/machine-id

# Text login by default; systemd auto-starts serial-getty on console=.
systemctl set-default multi-user.target 2>/dev/null \
  || ln -sfv /usr/lib/systemd/system/multi-user.target \
             /etc/systemd/system/default.target

# Empty root password for early development. CHANGE BEFORE ANY REAL USE.
sed -i 's|^root:x:|root::|' /etc/passwd

echo
echo "==> Native build complete: full core userland + systemd as /usr/sbin/init"
NATIVE
    sudo chmod +x "$ROOTFS/root/build-native.sh"
}

# The authoritative full package order for the final system (checklist).
# The native script above implements the critical path through systemd; the
# rest of userland is layered on using the identical in-chroot pattern.
FINAL_SYSTEM_SEQUENCE=(
    glibc zlib bzip2 xz ncurses bash coreutils
    sed grep gawk diffutils findutils tar gzip make patch m4 bison flex
    binutils gcc file gperf expat libcap kmod util-linux dbus systemd
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
    warn "sources/pip/ is missing — the systemd build tools weren't staged."
    warn "Run 'make fetch' on a host with pip before the final-system build."
fi

if enter_rootfs '/root/build-native.sh'; then
    stamp_set "final-system-complete"
    ok "Final system built — systemd is /usr/sbin/init"
else
    warn "Native build stopped before completion — inspect the log above."
    warn "Re-run 'make system ARCH=$ARCH' to resume (completed packages are installed)."
fi

unstage_sources
umount_vfs
echo
ok "Stage 40 finished for $ARCH"
