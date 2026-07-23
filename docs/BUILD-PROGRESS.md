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
- [x] `make system` — **COMPLETE**. Full boot-critical userland + systemd 256.7
      as `/usr/sbin/init`. Path: ncurses/bash/coreutils, util-linux, pkgconf,
      zlib/xz/expat/libcap, kmod, gperf, libffi, Python 3.12, ninja,
      meson/jinja2/markupsafe (pip), perl 5.38.2, libxcrypt, dbus (meson),
      **systemd**, + /etc config (fstab, resolv.conf, machine-id, default target).
- [x] `make kernel ARCH=aarch64` — Linux 6.12.9 arm64 (vmlinuz, 44M).
- [x] `make image ARCH=aarch64` — ext4 root disk image (stage 60 now builds a
      disk image, not a multi-GB initramfs; the 4.5 GB rootfs made initramfs
      impractical).
- [x] 🎯🎉 **FIRST BOOT ACHIEVED** — `make run ARCH=aarch64` boots Linux 6.12.9,
      mounts the ext4 root, runs **systemd 256.7 as PID 1**, reaches the
      Multi-User target, and shows the `verkos login:` prompt. Booted in QEMU
      (virt board, cortex-a72) inside the Colima container.

### Known non-fatal issue at first boot
- `systemd-logind` fails in a restart loop — it needs PAM + a running D-Bus
  system bus, and systemd was built `-PAM`. Boot still reaches Multi-User and
  the login prompt. Fix in the full-userland pass (add shadow/PAM, wire dbus).

## Phase 2 progress

- [x] **Full userland** (`VERK_FULL_USERLAND=1`): gcc, binutils, bison, flex,
      sed, grep, gawk, tar, ... all built natively into the rootfs.
- [x] **Working login**: shadow 4.16.0 gives /bin/login + passwd; root password
      set (dev default `verkos`); serial-getty autologin drops boot straight to
      a **`root@verkos:~#`** shell. Verified booting the rebuilt ext4 image.
- [x] **Networking + SSH** — VerkOS goes online and is SSH-accessible:
      - **dhcpcd** is the active DHCP/DNS client (chosen over systemd-networkd as
        the first step toward replacing systemd bits); networkd is built but
        left disabled as a swappable option.
      - **OpenSSL 3.4.0 + OpenSSH 9.9p1**: sshd runs, root login via password
        (verkos) AND an ed25519 key. Verified: SSH'd in over the QEMU hostfwd
        (2222→22), `eth0: up`, DNS resolves (getent hosts one.one.one.one →
        Cloudflare), outbound works.
- [x] Fun: verkfetch (neofetch) with cyan→purple ASCII (user is refining the art).
- [x] **Userland tools** — iproute2 (ip/ss), iputils (ping), curl, procps-ng
      (ps/top/free), less, nano; plus bison/flex. Verified live over SSH:
      `eth0 UP 10.0.2.15/24`, ping 0% loss, `curl -sI http://example.com`
      → 200 OK, ps/free work.
- [x] **Own package manager — vpk** (Phase 5 "make it ours"): a self-contained,
      source-based (ports-style) package manager in C. Links libcurl/liblzma/zlib
      and implements SHA-256, tar extraction, recipe parsing, recursive
      dependency resolution and the install DB itself. `vpk install|remove|list|
      info` (`verk` alias). Ships a CA bundle so HTTPS works. **Verified on
      VerkOS itself**: `vpk install hello` fetched over HTTPS, verified sha256,
      built from source with our gcc, installed; `hello` runs.
- [ ] logind/PAM (needs Linux-PAM + systemd -Dpam=enabled) — deferred.
Then multi-arch (x86_64) and GRUB/real-hardware boot.

VerkOS is now a self-hosting distro: it builds software from source and manages
its own packages, on itself.
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
12. markupsafe wheel was cp311 (host) not cp312 (target) → build it from sdist.
13. dbus 1.16 dropped autotools → build with meson.
14. systemd needs libcrypt (glibc 2.38+ drops it) → build libxcrypt.
15. libxcrypt needs perl → build perl; 5.40.0 miscompiles → pin 5.38.2.
16. `make && make install` under `set -e` doesn't abort on make failure (masked
    perl's failure and stamped it done) → use separate statements everywhere.

Plus: per-package build stamps (`/sources/.nst`) so re-runs skip finished packages.

## Notes / deferred

- Only the **boot-critical** subset of the final system is built by default. Set
  `VERK_FULL_USERLAND=1` to also rebuild the compiler + full GNU userland.
- The native glibc rebuild is intentionally skipped (cross-toolchain glibc is
  used). A proper LFS ch7-temp-tools + ch8-glibc pass is a future improvement.
- Next expected friction: systemd's meson build options for this version.
