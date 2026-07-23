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
BINUTILS_VERSION="2.43.1";   BINUTILS_SHA256="13f74202a3c4c51118b797a39ea4200d3f6cfbe224da6d1d95bb938480132dfd"
GCC_VERSION="14.2.0";        GCC_SHA256="a7b39bc69cbf9e25826c5a60ab26477001f7c08d85cec04bc0e29cabed6f3cc9"
GLIBC_VERSION="2.40";        GLIBC_SHA256="19a890175e9263d748f627993de6f4b1af9cd21e03f080e4bfb3a1fac10205a2"
GMP_VERSION="6.3.0";         GMP_SHA256="a3c2b80201b89e68616f4ad30bc66aee4927c3ce50e33929ca819d5c43538898"
MPFR_VERSION="4.2.1";        MPFR_SHA256="277807353a6726978996945af13e52829e3abd7a9a5b7fb2793894e18f1fcbb2"
MPC_VERSION="1.3.1";         MPC_SHA256="ab642492f5cf882b74aa0cb730cd410a81edcdbec895183ce930e706c1c759b8"

# --- Kernel ----------------------------------------------------------------
LINUX_VERSION="6.12.9";      LINUX_SHA256="87be0360df0931b340d2bac35161a548070fbc3a8c352c49e21e96666c26aeb4"      # 6.12 LTS series

# --- Core userland ---------------------------------------------------------
BASH_VERSION_PKG="5.2.37";   BASH_SHA256="9599b22ecd1d5787ad7d3b7bf0c59f312b3396d1e281175dd1f8a4014da621ff"
COREUTILS_VERSION="9.5";     COREUTILS_SHA256="cd328edeac92f6a665de9f323c93b712af1858bc2e0d88f3f7100469470a1b8a"
UTIL_LINUX_VERSION="2.40.2"; UTIL_LINUX_SHA256="d78b37a66f5922d70edf3bdfb01a6b33d34ed3c3cafd6628203b2a2b67c8e8b3"
M4_VERSION="1.4.19";         M4_SHA256="63aede5c6d33b6d9b13511cd0be2cac046f2e70fd0a07aa9573a04a82783af96"
NCURSES_VERSION="6.5";       NCURSES_SHA256="136d91bc269a9a5785e5f9e980bc76ab57428f604ce3e5a5a90cebc767971cc6"
BISON_VERSION="3.8.2";       BISON_SHA256="9bba0214ccf7f1079c5d59210045227bcf619519840ebfa80cd3849cff5a5bf2"
FLEX_VERSION="2.6.4";        FLEX_SHA256="e87aae032bf07c26f85ac0ed3250998c37621d95f8bd748b31f15b33c45ee995"
GAWK_VERSION="5.3.1";        GAWK_SHA256="694db764812a6236423d4ff40ceb7b6c4c441301b72ad502bb5c27e00cd56f78"
GREP_VERSION="3.11";         GREP_SHA256="1db2aedde89d0dea42b16d9528f894c8d15dae4e190b59aecc78f5a951276eab"
SED_VERSION="4.9";           SED_SHA256="6e226b732e1cd739464ad6862bd1a1aba42d7982922da7a53519631d24975181"
TAR_VERSION="1.35";          TAR_SHA256="4d62ff37342ec7aed748535323930c7cf94acf71c3591882b26a7ea50f3edc16"
GZIP_VERSION="1.13";         GZIP_SHA256="7454eb6935db17c6655576c2e1b0fabefd38b4d0936e0f87f48cd062ce91a057"
XZ_VERSION="5.6.3";          XZ_SHA256="db0590629b6f0fa36e74aea5f9731dc6f8df068ce7b7bafa45301832a5eebc3a"
FILE_VERSION="5.46";         FILE_SHA256="c9cc77c7c560c543135edc555af609d5619dbef011997e988ce40a3d75d86088"
MAKE_VERSION="4.4.1";        MAKE_SHA256="dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3"
FINDUTILS_VERSION="4.10.0";  FINDUTILS_SHA256="1387e0b67ff247d2abde998f90dfbf70c1491391a59ddfecb8ae698789f0a4f5"
DIFFUTILS_VERSION="3.10";    DIFFUTILS_SHA256="90e5e93cc724e4ebe12ede80df1634063c7a855692685919bfe60b556c9bd09e"
PATCH_VERSION="2.7.6";       PATCH_SHA256="ac610bda97abe0d9f6b7c963255a11dcb196c25e337c61f94e4778d632f1d8fd"
BZIP2_VERSION="1.0.8";       BZIP2_SHA256="ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269"
GPERF_VERSION="3.1";         GPERF_SHA256="588546b945bba4b70b6a3a616e80b4ab466e3f33024a352fc2198112cdbb3ae2"
PKGCONF_VERSION="2.3.0";     PKGCONF_SHA256="3a9080ac51d03615e7c1910a0a2a8df08424892b5f13b0628a204d3fcce0ea8b"

# --- Init / system layer ---------------------------------------------------
SYSTEMD_VERSION="256.7";     SYSTEMD_SHA256="896d76ff65c88f5fd9e42f90d152b0579049158a163431dd77cdc57748b1d7b0"
DBUS_VERSION="1.16.0";       DBUS_SHA256="9f8ca5eb51cbe09951aec8624b86c292990ae2428b41b856e2bed17ec65c8849"
UTIL_LINUX_MIN="2.40.2"      # systemd needs a recent util-linux
KMOD_VERSION="33";           KMOD_SHA256="dc768b3155172091f56dc69430b5481f2d76ecd9ccb54ead8c2540dbcf5ea9bc"
EXPAT_VERSION="2.6.4";       EXPAT_SHA256="a695629dae047055b37d50a0ff4776d1d45d0a4c842cf4ccee158441f55ff7ee"
ZLIB_VERSION="1.3.2";        ZLIB_SHA256="bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16"
LIBCAP_VERSION="2.70";       LIBCAP_SHA256="23a6ef8aadaf1e3e875f633bb2d116cfef8952dba7bc7c569b13458e1952b30f"
LIBXCRYPT_VERSION="4.4.36";  LIBXCRYPT_SHA256="e5e1f4caee0a01de2aee26e3138807d6d3ca2b8e67287966d1fefd65e1fd8943"

# --- Python + systemd build tooling ----------------------------------------
# systemd is built with meson/ninja and needs python3 + jinja2. We build libffi,
# Python and ninja from source in the chroot; meson/jinja2/markupsafe come from
# sdists fetched up front (see 10-fetch-sources.sh) and pip-installed offline.
PYTHON_VERSION="3.12.7";     PYTHON_SHA256="24887b92e2afd4a2ac602419ad4b596372f67ac9b077190f459aba390faf5550"
LIBFFI_VERSION="3.4.6";      LIBFFI_SHA256="b0dea9df23c863a7a50e825440f3ebffabd65df1497108e5d437747843895a4e"

# --- Build-only helpers ----------------------------------------------------
MESON_VERSION="1.6.0";       MESON_SHA256=""   # pip sdist, not a tarball — no hash here
NINJA_VERSION="1.12.1";      NINJA_SHA256="821bdff48a3f683bc4bb3b6f0b5fe7b2d647cf65d52aeb63328c91a6c6df285a"
JINJA2_VERSION="3.1.4"
MARKUPSAFE_VERSION="2.1.5"
FLIT_CORE_VERSION="3.9.0"

# ---------------------------------------------------------------------------
# Download URL templates. %V = version. Grouped by upstream host.
# common.sh::source_url maps a package name to one of these.
# ---------------------------------------------------------------------------
# ftpmirror.gnu.org redirects to a nearby mirror. Use it instead of ftp.gnu.org:
# the master server rate-limits bulk downloads and will start refusing partway
# through a full fetch/pin run.
URL_GNU="https://ftpmirror.gnu.org/gnu"
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
# zlib.net's root only hosts the CURRENT release, so a pinned version 404s as
# soon as upstream bumps. fossils/ is the permanent archive of every release.
URL_ZLIB="https://zlib.net/fossils"
URL_FILE="https://astron.com/pub/file"
URL_FLEX="https://github.com/westes/flex/releases/download"
URL_PYTHON="https://www.python.org/ftp/python"
URL_LIBFFI="https://github.com/libffi/libffi/releases/download"
URL_NINJA="https://github.com/ninja-build/ninja/archive/refs/tags"
URL_PKGCONF="https://distfiles.ariadne.space/pkgconf"
URL_LIBXCRYPT="https://github.com/besser82/libxcrypt/releases/download"
