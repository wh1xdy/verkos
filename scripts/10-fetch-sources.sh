#!/usr/bin/env bash
# Stage 10 — download and verify every upstream source tarball.
# Sources are shared across architectures (work/sources/).
source "$(dirname "$0")/lib/common.sh"
mkdirs

step "Fetching sources into $SRC_DIR"

# Each line: fetch_one "<tarball>" "<sha256 var>"
# Tarball names are derived from config/versions.sh.
fetch_one "binutils-${BINUTILS_VERSION}.tar.xz"       "$BINUTILS_SHA256"
fetch_one "gcc-${GCC_VERSION}.tar.xz"                 "$GCC_SHA256"
fetch_one "gmp-${GMP_VERSION}.tar.xz"                 "$GMP_SHA256"
fetch_one "mpfr-${MPFR_VERSION}.tar.xz"               "$MPFR_SHA256"
fetch_one "mpc-${MPC_VERSION}.tar.gz"                 "$MPC_SHA256"
fetch_one "glibc-${GLIBC_VERSION}.tar.xz"             "$GLIBC_SHA256"
fetch_one "linux-${LINUX_VERSION}.tar.xz"             "$LINUX_SHA256"

fetch_one "m4-${M4_VERSION}.tar.xz"                   "$M4_SHA256"
fetch_one "ncurses-${NCURSES_VERSION}.tar.gz"         "$NCURSES_SHA256"
fetch_one "bash-${BASH_VERSION_PKG}.tar.gz"           "$BASH_SHA256"
fetch_one "coreutils-${COREUTILS_VERSION}.tar.xz"     "$COREUTILS_SHA256"
fetch_one "bison-${BISON_VERSION}.tar.xz"             "$BISON_SHA256"
fetch_one "gawk-${GAWK_VERSION}.tar.xz"               "$GAWK_SHA256"
fetch_one "grep-${GREP_VERSION}.tar.xz"               "$GREP_SHA256"
fetch_one "sed-${SED_VERSION}.tar.xz"                 "$SED_SHA256"
fetch_one "tar-${TAR_VERSION}.tar.xz"                 "$TAR_SHA256"
fetch_one "gzip-${GZIP_VERSION}.tar.xz"               "$GZIP_SHA256"
fetch_one "xz-${XZ_VERSION}.tar.xz"                   "$XZ_SHA256"
fetch_one "make-${MAKE_VERSION}.tar.gz"               "$MAKE_SHA256"
fetch_one "findutils-${FINDUTILS_VERSION}.tar.xz"     "$FINDUTILS_SHA256"
fetch_one "diffutils-${DIFFUTILS_VERSION}.tar.xz"     "$DIFFUTILS_SHA256"
fetch_one "patch-${PATCH_VERSION}.tar.xz"             "$PATCH_SHA256"
fetch_one "flex-${FLEX_VERSION}.tar.gz"               "$FLEX_SHA256"
fetch_one "file-${FILE_VERSION}.tar.gz"               "$FILE_SHA256"
fetch_one "bzip2-${BZIP2_VERSION}.tar.gz"             "$BZIP2_SHA256"
fetch_one "zlib-${ZLIB_VERSION}.tar.gz"               "$ZLIB_SHA256"
fetch_one "gperf-${GPERF_VERSION}.tar.gz"             "$GPERF_SHA256"
fetch_one "pkgconf-${PKGCONF_VERSION}.tar.xz"         "$PKGCONF_SHA256"

# System layer (built natively in the chroot; downloaded here).
fetch_one "expat-${EXPAT_VERSION}.tar.xz"             "$EXPAT_SHA256"
fetch_one "libcap-${LIBCAP_VERSION}.tar.xz"           "$LIBCAP_SHA256"
fetch_one "libxcrypt-${LIBXCRYPT_VERSION}.tar.xz"     "$LIBXCRYPT_SHA256"
fetch_one "perl-${PERL_VERSION}.tar.xz"               "$PERL_SHA256"
fetch_one "shadow-${SHADOW_VERSION}.tar.xz"           "$SHADOW_SHA256"
fetch_one "dhcpcd-${DHCPCD_VERSION}.tar.xz"           "$DHCPCD_SHA256"
fetch_one "openssl-${OPENSSL_VERSION}.tar.gz"         "$OPENSSL_SHA256"
fetch_one "openssh-${OPENSSH_VERSION}.tar.gz"         "$OPENSSH_SHA256"
fetch_one "kmod-${KMOD_VERSION}.tar.xz"               "$KMOD_SHA256"
fetch_one "util-linux-${UTIL_LINUX_VERSION}.tar.xz"   "$UTIL_LINUX_SHA256"
fetch_one "dbus-${DBUS_VERSION}.tar.xz"               "$DBUS_SHA256"

# Python + systemd build tooling (built/installed in the chroot).
fetch_one "libffi-${LIBFFI_VERSION}.tar.gz"           "$LIBFFI_SHA256"
fetch_one "Python-${PYTHON_VERSION}.tar.xz"           "$PYTHON_SHA256"
fetch_one "ninja-${NINJA_VERSION}.tar.gz"             "$NINJA_SHA256"

fetch_one "systemd-${SYSTEMD_VERSION}.tar.gz"         "$SYSTEMD_SHA256"

# meson/jinja2/markupsafe/flit_core as sdists for an OFFLINE pip install inside
# the chroot (Python there has no TLS). Fetched here on the host, which does.
# Stored under sources/pip/ so the chroot build can --find-links them.
if command -v pip3 >/dev/null 2>&1 || command -v pip >/dev/null 2>&1; then
    PIP="$(command -v pip3 || command -v pip)"
    step "Fetching Python build tools (sdists) for offline chroot install"
    mkdir -p "$SRC_DIR/pip"
    # jinja2 + meson are pure-Python (py3-none-any wheels) — grab them as wheels,
    # along with setuptools + wheel (also universal) to build markupsafe. But
    # markupsafe has a C extension: its wheels are Python-version+arch specific,
    # and this host's pip (Python 3.11) would fetch a cp311 wheel that won't
    # match the target's Python 3.12. So fetch markupsafe as an sdist and build
    # it against the target Python in the chroot.
    "$PIP" download --dest "$SRC_DIR/pip" \
        "setuptools" "wheel" \
        "jinja2==${JINJA2_VERSION}" \
        "meson==${MESON_VERSION}" \
        && "$PIP" download --no-binary :all: --dest "$SRC_DIR/pip" \
        "markupsafe==${MARKUPSAFE_VERSION}" \
        && ok "Python build tools staged in $SRC_DIR/pip" \
        || warn "pip download failed — systemd's meson/jinja2 must be provided another way"
else
    warn "No host pip found — meson/jinja2/markupsafe won't be staged for the"
    warn "chroot. Install pip and re-run 'make fetch', or provide them manually."
fi

echo
ok "All sources present in $SRC_DIR"
# NB: a bare `[ -f X ] && warn` as the final line makes the script exit 1 when the
# test is false (the all-pinned success case), which fails `make fetch`. Guard it.
if [ -f "$LOCKFILE" ]; then
    warn "Some packages had no pinned hash — see $LOCKFILE and paste values into config/versions.sh"
fi
exit 0
