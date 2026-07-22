# VerkOS — pinned upstream package versions
# ---------------------------------------------------------------------------
# Single source of truth for every version number in the build. Nothing else
# should hard-code a version. To bump a package: change its version and SHA-256
# here, then `make fetch`.
#
# SHA-256 policy: if a *_SHA256 is empty, `make fetch` downloads the tarball,
# records the computed hash into config/versions.lock, and WARNS. Once you've
# verified a hash against upstream, paste it here to make the build refuse any
# tarball that doesn't match. We never *build* unverified source without at
# least a recorded lock entry.
#
# Versions below target a coherent, buildable set (LFS-9.x/10.x lineage,
# modernised). Adjust freely — the build reads these, not magic numbers.
# ---------------------------------------------------------------------------

# --- Toolchain -------------------------------------------------------------
BINUTILS_VERSION="2.43.1";   BINUTILS_SHA256=""
GCC_VERSION="14.2.0";        GCC_SHA256=""
GLIBC_VERSION="2.40";        GLIBC_SHA256=""
GMP_VERSION="6.3.0";         GMP_SHA256=""
MPFR_VERSION="4.2.1";        MPFR_SHA256=""
MPC_VERSION="1.3.1";         MPC_SHA256=""

# --- Kernel ----------------------------------------------------------------
LINUX_VERSION="6.12.9";      LINUX_SHA256=""      # 6.12 LTS series

# --- Core userland ---------------------------------------------------------
BASH_VERSION_PKG="5.2.37";   BASH_SHA256=""
COREUTILS_VERSION="9.5";     COREUTILS_SHA256=""
UTIL_LINUX_VERSION="2.40.2"; UTIL_LINUX_SHA256=""
M4_VERSION="1.4.19";         M4_SHA256=""
NCURSES_VERSION="6.5";       NCURSES_SHA256=""
BISON_VERSION="3.8.2";       BISON_SHA256=""
FLEX_VERSION="2.6.4";        FLEX_SHA256=""
GAWK_VERSION="5.3.1";        GAWK_SHA256=""
GREP_VERSION="3.11";         GREP_SHA256=""
SED_VERSION="4.9";           SED_SHA256=""
TAR_VERSION="1.35";          TAR_SHA256=""
GZIP_VERSION="1.13";         GZIP_SHA256=""
XZ_VERSION="5.6.3";          XZ_SHA256=""
FILE_VERSION="5.46";         FILE_SHA256=""
MAKE_VERSION="4.4.1";        MAKE_SHA256=""
FINDUTILS_VERSION="4.10.0";  FINDUTILS_SHA256=""
DIFFUTILS_VERSION="3.10";    DIFFUTILS_SHA256=""
PATCH_VERSION="2.7.6";       PATCH_SHA256=""
BZIP2_VERSION="1.0.8";       BZIP2_SHA256=""
GPERF_VERSION="3.1";         GPERF_SHA256=""

# --- Init / system layer ---------------------------------------------------
SYSTEMD_VERSION="256.7";     SYSTEMD_SHA256=""
DBUS_VERSION="1.16.0";       DBUS_SHA256=""
UTIL_LINUX_MIN="2.40.2"      # systemd needs a recent util-linux
KMOD_VERSION="33";           KMOD_SHA256=""
EXPAT_VERSION="2.6.4";       EXPAT_SHA256=""
ZLIB_VERSION="1.3.1";        ZLIB_SHA256=""
LIBCAP_VERSION="2.70";       LIBCAP_SHA256=""

# --- Build-only helpers ----------------------------------------------------
MESON_VERSION="1.6.0";       MESON_SHA256=""
NINJA_VERSION="1.12.1";      NINJA_SHA256=""

# ---------------------------------------------------------------------------
# Download URL templates. %V = version. Grouped by upstream host.
# common.sh::source_url maps a package name to one of these.
# ---------------------------------------------------------------------------
URL_GNU="https://ftp.gnu.org/gnu"
URL_KERNEL="https://cdn.kernel.org/pub/linux/kernel"
URL_SYSTEMD="https://github.com/systemd/systemd/archive/refs/tags"
URL_NCURSES="https://invisible-mirror.net/archives/ncurses"
URL_XZ="https://github.com/tukaani-project/xz/releases/download"
URL_SOURCEWARE="https://sourceware.org/pub"
URL_DBUS="https://dbus.freedesktop.org/releases/dbus"
URL_KMOD="https://mirrors.edge.kernel.org/pub/linux/utils/kernel/kmod"
URL_UTILLINUX="https://mirrors.edge.kernel.org/pub/linux/utils/util-linux"
URL_LIBCAP="https://mirrors.edge.kernel.org/pub/linux/libs/security/linux-privs/libcap2"
URL_EXPAT="https://github.com/libexpat/libexpat/releases/download"
URL_ZLIB="https://zlib.net"
