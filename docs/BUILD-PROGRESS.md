# Build progress — first real aarch64 build

Live status of the first end-to-end VerkOS build, run on an Apple Silicon Mac
(M5 Pro) inside a native arm64 Linux VM. Target: **aarch64**.

## Environment (how to resume)

- **Colima VM**: `colima start` (configured 16 CPU / 20 GB / 80 GB disk).
- **Build container**: `verkbuild` (Debian bookworm, privileged), image
  `verkbuild-img` (snapshot with all apt build-deps installed).
- **work/ on an ext4 Docker volume** `verkwork` mounted at `/os/work` — because
  macOS APFS is case-insensitive and the kernel source won't extract on it
  (see docs/BUILD.md → "Building on macOS"). The repo is bind-mounted at `/os`.

Resume a build:
```sh
colima start && docker start verkbuild
docker exec verkbuild make system ARCH=aarch64      # continues; per-package
                                                    # stamps skip finished work
```
Boot the result (once built) on the Mac host, which has qemu-system-aarch64:
```sh
make run ARCH=aarch64
```

## Stages

- [x] `make check` — host prerequisites
- [x] `make fetch` — all 36 sources downloaded + SHA-256 verified; pip sdists staged
- [x] `make toolchain` — cross binutils/gcc/glibc/libstdc++ (aarch64-verk-linux-gnu)
- [x] `make temp` — full LFS ch.6 temporary tools (17 packages)
- [~] `make system` — in progress. Boot-critical path:
      ncurses/bash/coreutils ✓, util-linux ✓, zlib/xz/expat/libcap ✓,
      pkgconf ✓, kmod ✓, gperf ✓, libffi ✓, Python ✓, ninja ✓, setuptools/wheel ✓
      → **currently blocked in the meson/jinja2 pip step**, then dbus, systemd.

### ⏭️ Resume here (current blocker)

The offline pip install of jinja2 fails:
```
flit_core.config.ConfigError: license field should be <class 'dict'>, not <class 'str'>
```
A flit_core ↔ jinja2 metadata mismatch: the staged flit_core version rejects
jinja2 3.1.4's `license` string (PEP 639 changed this). Fix options for next
session (in scripts/40-final-system.sh / config/versions.sh + re-`make fetch`):
- pin an older flit_core (e.g. 3.9.0 is already set — but the *downloaded* one
  may differ; pin it explicitly in the pip download and `--find-links` install), OR
- bump jinja2 to a version whose metadata the flit_core accepts, OR
- stage a matching flit_core/jinja2 pair.
The stamps mean a re-run jumps straight back to this pip step in seconds.
- [ ] `make kernel` ARCH=aarch64
- [ ] `make image` + `make run` → **first boot** 🎯

## Real bugs found & fixed by building (all committed)

1. `make fetch` exited 1 on the all-hashes-pinned success case.
2. Linux headers used `aarch64` instead of the kernel's `arm64`.
3. macOS APFS case-insensitivity broke kernel extraction → work/ on ext4 volume.
4. libstdc++ headers installed off the cross g++ search path (gcc pass 2 libcody).
5. Missing merged-/usr symlinks → chroot "failed to run /usr/bin/env".
6. Native glibc rebuild needed bison/python (ch7) → skip it (cross glibc suffices);
   gate the heavy full-userland rebuild behind VERK_FULL_USERLAND=1.
7. coreutils refuses to configure as root → FORCE_UNSAFE_CONFIGURE=1.
8. util-linux liblastlog2 needs sqlite3 → --disable-liblastlog2.
9. kmod needs pkg-config → build pkgconf; needs scdoc → --disable-manpages.
10. libffi installed to /usr/lib64 (off linker path) → --libdir=/usr/lib + ldconfig.
11. Python 3.12 ensurepip lacks setuptools → stage setuptools/wheel sdists.

Plus: per-package build stamps (`/sources/.nst`) so re-runs skip finished packages.

## Notes / deferred

- Only the **boot-critical** subset of the final system is built by default. Set
  `VERK_FULL_USERLAND=1` to also rebuild the compiler + full GNU userland.
- The native glibc rebuild is intentionally skipped (cross-toolchain glibc is
  used). A proper LFS ch7-temp-tools + ch8-glibc pass is a future improvement.
- Next expected friction: systemd's meson build options for this version.
