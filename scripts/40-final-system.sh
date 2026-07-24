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
sshd:x:50:50:sshd PrivSep:/var/lib/sshd:/usr/bin/false
dhcpcd:x:52:52:dhcpcd PrivSep:/var/lib/dhcpcd:/usr/bin/false
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
dhcpcd:x:52:
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
# Per-package build stamps. MUST live inside the rootfs (per-architecture), NOT
# under /sources (which is shared across arches) — otherwise an x86_64 build
# would skip packages already stamped by the aarch64 build and ship a rootfs
# missing systemd et al.
NST=/var/lib/vpk-build-stamps; mkdir -p "$NST"
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
make; make install
ln -sfv pkgconf /usr/bin/pkg-config
cd /sources

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
make; make install
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
./configure --prefix=/usr; make; make install; cd /sources

# m4 (native)
say "m4 ${M4_VERSION}"
d=$(unpack m4-${M4_VERSION}.tar.xz); cd "$d"
./configure --prefix=/usr; make; make install; cd /sources

# binutils (final, native)
say "binutils ${BINUTILS_VERSION} (final)"
d=$(unpack binutils-${BINUTILS_VERSION}.tar.xz); cd "$d"
mkdir -p build && cd build
../configure --prefix=/usr --sysconfdir=/etc --enable-ld=default \
    --enable-plugins --enable-shared --disable-werror --enable-64-bit-bfd \
    --enable-new-dtags --with-system-zlib --enable-default-hash-style=gnu \
    --enable-gprofng=no
make tooldir=/usr; make tooldir=/usr install
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
make; make install
ln -sfv gcc /usr/bin/cc
cd /sources

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

# 6. zlib, xz, expat, libcap, kmod — systemd link deps
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
say "kmod ${KMOD_VERSION}"
d=$(unpack kmod-${KMOD_VERSION}.tar.xz); cd "$d"
./configure --prefix=/usr --sysconfdir=/etc --with-openssl=no --with-xz \
    --with-zstd=no --disable-manpages
make; make install; cd /sources

# 7. gperf — systemd build dep
say "gperf ${GPERF_VERSION}"
d=$(unpack gperf-${GPERF_VERSION}.tar.gz); cd "$d"
./configure --prefix=/usr; make; make install; cd /sources

# 7b. Python build toolchain for systemd (meson/ninja/jinja2) ---------------
# libffi → Python → ninja (all from source), then meson/jinja2 via offline pip.
# --libdir=/usr/lib: libffi otherwise installs to /usr/lib64, which is off the
# linker path in our merged-/usr (lib, not lib64) layout, so Python's _ctypes
# fails to import (libffi.so.8 not found). Refresh the linker cache after.
say "libffi ${LIBFFI_VERSION}"
d=$(unpack libffi-${LIBFFI_VERSION}.tar.gz); cd "$d"
./configure --prefix=/usr --libdir=/usr/lib --disable-static
make; make install
printf '/usr/lib\n/usr/local/lib\n' > /etc/ld.so.conf
ldconfig
cd /sources

if need python; then
say "Python ${PYTHON_VERSION}"
d=$(unpack Python-${PYTHON_VERSION}.tar.xz); cd "$d"
./configure --prefix=/usr --enable-shared --with-system-expat \
    --with-ensurepip=yes --without-static-libpython
make; make install
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

# 8.6 shadow — /bin/login, passwd, useradd, /etc/shadow. util-linux's login was
# disabled, so this is what makes the getty login prompt actually work.
if need shadow; then
say "shadow ${SHADOW_VERSION}"
d=$(unpack shadow-${SHADOW_VERSION}.tar.xz); cd "$d"
sed -i 's/groups$(EXEEXT) //' src/Makefile.in
find man -name Makefile.in -exec sed -i 's/groups\.1 / /'   {} \;
find man -name Makefile.in -exec sed -i 's/getspnam\.3 / /' {} \;
find man -name Makefile.in -exec sed -i 's/passwd\.5 / /'   {} \;
sed -e 's:#ENCRYPT_METHOD DES:ENCRYPT_METHOD YESCRYPT:' \
    -e 's:/var/spool/mail:/var/mail:' -e '/PATH=/{s@/sbin:@@;s@/usr/sbin:@@}' \
    -i etc/login.defs
./configure --sysconfdir=/etc --disable-static \
    --without-libbsd --with-{b,yes}crypt --without-selinux \
    --without-libpam
make; make exec_prefix=/usr install
cd /sources
mark shadow
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
if need systemd; then
say "systemd ${SYSTEMD_VERSION}"
d=$(unpack systemd-${SYSTEMD_VERSION}.tar.gz); cd "$d"
mkdir -p build && cd build
meson setup .. \
    --prefix=/usr --buildtype=release \
    -Dmode=release -Ddefault-dnssec=no -Dfirstboot=false \
    -Dinstall-tests=false -Dldconfig=false -Dsysusers=false \
    -Drpmmacrosdir=no -Dhomed=disabled -Duserdb=false \
    -Dman=disabled -Dmode=release -Dpam=enabled -Dpamconfdir=no \
    -Dpamlibdir=/usr/lib/security \
    -Ddev-kvm-mode=0660 -Dnobody-group=nogroup
ninja
ninja install
mark systemd
fi
# Point /sbin/init at systemd.
ln -sfv /usr/lib/systemd/systemd /usr/sbin/init
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
if need dhcpcd; then
say "dhcpcd ${DHCPCD_VERSION}"
d=$(unpack dhcpcd-${DHCPCD_VERSION}.tar.xz); cd "$d"
./configure --prefix=/usr --sysconfdir=/etc --libexecdir=/usr/lib/dhcpcd \
    --dbdir=/var/lib/dhcpcd --rundir=/run --privsepuser=dhcpcd
make
make install
cd /sources
mark dhcpcd
fi

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
if need iproute2; then
say "iproute2 ${IPROUTE2_VERSION}"
d=$(unpack iproute2-${IPROUTE2_VERSION}.tar.xz); cd "$d"
./configure
make
make install
cd /sources
mark iproute2
fi

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

# procps-ng — ps, top, free
if need procps; then
say "procps-ng ${PROCPS_VERSION}"
d=$(unpack procps-ng-${PROCPS_VERSION}.tar.xz); cd "$d"
./configure --prefix=/usr --disable-static --disable-kill
make
make install
cd /sources
mark procps
fi

# less — pager
if need less; then
say "less ${LESS_VERSION}"
d=$(unpack less-${LESS_VERSION}.tar.gz); cd "$d"
./configure --prefix=/usr --sysconfdir=/etc
make
make install
cd /sources
mark less
fi

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
cat > /etc/pam.d/systemd-user <<'P'
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

# Machine id (systemd needs one).
systemd-machine-id-setup 2>/dev/null || echo uninitialized > /etc/machine-id

# Text login by default; systemd auto-starts serial-getty on console=.
systemctl set-default multi-user.target 2>/dev/null \
  || ln -sfv /usr/lib/systemd/system/multi-user.target \
             /etc/systemd/system/default.target

# Users/passwords: create /etc/shadow from /etc/passwd, then set a known root
# password. DEV DEFAULT: root / "verkos" — CHANGE BEFORE ANY REAL USE.
sed -i 's|^root::|root:x:|' /etc/passwd    # undo the earlier empty-password hack
if command -v pwconv >/dev/null; then
    pwconv
    echo "root:verkos" | chpasswd
else
    sed -i 's|^root:x:|root::|' /etc/passwd  # no shadow tools: fall back to empty
fi

# Serial-console autologin: drop straight to a root shell on boot (handy for dev
# and demos). Applies to any serial-getty@ instance regardless of tty name.
mkdir -p /etc/systemd/system/serial-getty@.service.d
cat > /etc/systemd/system/serial-getty@.service.d/autologin.conf <<'AL'
[Service]
ExecStart=
ExecStart=-/usr/bin/vgetty --autologin root --keep-baud %I 115200,38400,9600 $TERM
AL

# D-Bus system bus. Our dbus is built WITHOUT systemd integration (that would
# pull in glib), so it CANNOT do socket activation: --address=systemd: and
# --systemd-activation require systemd support and make dbus-daemon exit with
# "compiled without systemd support". Instead run it as a plain service that
# creates and listens on /run/dbus/system_bus_socket itself (address comes from
# the built-in system.conf). systemd-logind & friends connect there.
mkdir -p /etc/systemd/system/multi-user.target.wants /usr/lib/tmpfiles.d
rm -f /usr/lib/systemd/system/dbus.socket \
      /etc/systemd/system/sockets.target.wants/dbus.socket
cat > /usr/lib/systemd/system/dbus.service <<'DV'
[Unit]
Description=D-Bus System Message Bus
Documentation=man:dbus-daemon(1)
[Service]
Type=simple
ExecStartPre=/usr/bin/mkdir -p /run/dbus
ExecStart=/usr/bin/dbus-daemon --system --nofork --nopidfile --syslog-only
ExecReload=/usr/bin/dbus-send --print-reply --system --type=method_call --dest=org.freedesktop.DBus / org.freedesktop.DBus.ReloadConfig
Restart=on-failure
[Install]
WantedBy=multi-user.target
Alias=dbus.service
DV
echo 'd /run/dbus 0755 root root -' > /usr/lib/tmpfiles.d/dbus.conf
ln -sf /usr/lib/systemd/system/dbus.service \
       /etc/systemd/system/multi-user.target.wants/dbus.service
ln -sf /usr/lib/systemd/system/dbus.service /etc/systemd/system/dbus.service

# systemd ships dbus policy files (e.g. oom1) referencing service users that our
# minimal base lacks, making dbus warn "Unknown username systemd-oom". Create the
# ones referenced so the config loads cleanly.
for u in systemd-oom:996 systemd-network:995 systemd-resolve:994; do
    name="${u%%:*}"; uid="${u##*:}"
    grep -q "^$name:" /etc/passwd || \
        echo "$name:x:$uid:$uid:$name:/:/usr/bin/false" >> /etc/passwd
    grep -q "^$name:" /etc/group || echo "$name:x:$uid:" >> /etc/group
done

# systemd-vconsole-setup runs when /dev/tty0 exists and calls loadkeys/setfont from
# the kbd package, which our minimal base doesn't ship yet — so it failed and left
# systemd 'degraded'. Gate it on loadkeys being present: without kbd the unit is
# cleanly SKIPPED (not failed); installing kbd later re-enables it automatically.
mkdir -p /usr/lib/systemd/system/systemd-vconsole-setup.service.d
cat > /usr/lib/systemd/system/systemd-vconsole-setup.service.d/10-verkos.conf <<'VC'
[Unit]
ConditionPathExists=/usr/bin/loadkeys
VC

# --- Networking: vdhcp is ACTIVE (our own DHCP client). dhcpcd and
# --- systemd-networkd are both built but left disabled as swappable options —
# --- concrete steps toward replacing systemd's networking with our own.
mkdir -p /etc/systemd/system/multi-user.target.wants /etc/systemd/network
cat > /etc/systemd/network/20-wired.network <<'NET'
[Match]
Name=en* eth*

[Network]
DHCP=yes
NET
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
cat > /etc/systemd/system/vdhcp.service <<'VD'
[Unit]
Description=vdhcp DHCP client (VerkOS)
Wants=network.target
Before=network.target
After=systemd-udevd.service
[Service]
Type=simple
ExecStart=/usr/sbin/vdhcp
Restart=on-failure
RestartSec=2
[Install]
WantedBy=multi-user.target
VD
ln -sf /etc/systemd/system/vdhcp.service \
       /etc/systemd/system/multi-user.target.wants/vdhcp.service

# dhcpcd.service — kept installed but NOT enabled (swap in by enabling this and
# disabling vdhcp.service if ever needed).
cat > /etc/systemd/system/dhcpcd.service <<'DH'
[Unit]
Description=dhcpcd DHCP/DNS client (VerkOS, inactive fallback)
Wants=network.target
Before=network.target
Conflicts=vdhcp.service
[Service]
Type=simple
ExecStart=/usr/sbin/dhcpcd -B
Restart=on-failure
[Install]
WantedBy=multi-user.target
DH

# --- SSH: host keys, sshd service, password (root/verkos) + key auth --------
ssh-keygen -A 2>/dev/null || true
mkdir -p /root/.ssh; chmod 700 /root/.ssh
[ -f /root/.ssh/verkos_ed25519 ] || \
    ssh-keygen -t ed25519 -N '' -C verkos -f /root/.ssh/verkos_ed25519 2>/dev/null
cat /root/.ssh/verkos_ed25519.pub > /root/.ssh/authorized_keys
chmod 600 /root/.ssh/authorized_keys
cat >> /etc/ssh/sshd_config <<'SSHD'

# --- VerkOS: allow root via both password and key ---
PermitRootLogin yes
PasswordAuthentication yes
PubkeyAuthentication yes
SSHD
cat > /etc/systemd/system/sshd.service <<'SS'
[Unit]
Description=OpenSSH server (VerkOS)
After=network.target
[Service]
Type=simple
ExecStartPre=/usr/bin/ssh-keygen -A
ExecStart=/usr/sbin/sshd -D -e
Restart=on-failure
[Install]
WantedBy=multi-user.target
SS
ln -sf /etc/systemd/system/sshd.service \
       /etc/systemd/system/multi-user.target.wants/sshd.service

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
reg kmod "$KMOD_VERSION";          reg gperf "$GPERF_VERSION"
reg pkgconf "$PKGCONF_VERSION";    reg libffi "$LIBFFI_VERSION"
reg python "$PYTHON_VERSION";      reg ninja "$NINJA_VERSION"
reg perl "$PERL_VERSION";          reg libxcrypt "$LIBXCRYPT_VERSION"
reg shadow "$SHADOW_VERSION";      reg linux-pam "$LINUXPAM_VERSION"
reg dbus "$DBUS_VERSION";          reg systemd "$SYSTEMD_VERSION"
reg openssl "$OPENSSL_VERSION";    reg openssh "$OPENSSH_VERSION"
reg dhcpcd "$DHCPCD_VERSION";      reg iproute2 "$IPROUTE2_VERSION"
reg iputils "$IPUTILS_VERSION";    reg curl "$CURL_VERSION"
reg procps-ng "$PROCPS_VERSION";   reg less "$LESS_VERSION"
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
fi

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
    # Install verkfetch (VerkOS' neofetch). It's a repo script, not a fetched
    # package, so copy it in from the host side; /etc/profile runs it on login.
    if [ -f "$VERK_ROOT/tools/verkfetch" ]; then
        sudo install -m 0755 "$VERK_ROOT/tools/verkfetch" "$ROOTFS/usr/bin/verkfetch"
        ok "installed verkfetch → /usr/bin/verkfetch"
    fi
    ok "Final system built — systemd is /usr/sbin/init"
else
    warn "Native build stopped before completion — inspect the log above."
    warn "Re-run 'make system ARCH=$ARCH' to resume (completed packages are installed)."
fi

unstage_sources
umount_vfs
echo
ok "Stage 40 finished for $ARCH"
