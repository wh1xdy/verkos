# VerkOS roadmap

Where we are and where we're going. Checkboxes track real progress.

## Phase 0 — Foundations  ✅ (scaffolding done)

- [x] Project structure, docs, and decision log
- [x] Version-pinning config (`config/versions.sh`)
- [x] Per-architecture target config (x86_64, i686, aarch64)
- [x] Branding (`config/os-release`)
- [x] Build orchestration skeleton (`Makefile`, `scripts/build.sh`, `common.sh`)
- [x] QEMU boot launchers + GRUB template
- [x] Checksum-pinning tool (`scripts/pin-hashes.sh`) + 29/36 real SHA-256 pinned
- [ ] Pin the last 7 (github-hosted: xz, flex, expat, libffi, ninja, systemd, +
      zlib) — blocked by this build container's egress policy; run
      `scripts/pin-hashes.sh` once on a normal-network machine to fill them

## Phase 1 — First boot (x86_64)

The milestone: `make run ARCH=x86_64` drops us at a VerkOS shell.

- [x] Stage 0: source fetch + SHA-256 verify — *script done* (needs real hashes pinned)
- [x] Stage 1: cross-toolchain (binutils, gcc, glibc, libstdc++) — *script done*
- [x] Stage 2: temporary tools cross-built into the rootfs — *full LFS ch.6 sequence written*
- [x] Stage 3: chroot entry (native + qemu-user foreign) — *script done*
- [x] Stage 4: final system driver — glibc→util-linux→dbus→**systemd** critical path *written*
- [x] Stage 4b: full core userland (bzip2, file, m4, final binutils+gcc, bison,
      flex, sed, grep, gawk, diffutils, findutils, tar, gzip, make, patch) +
      system config (fstab, resolv.conf, machine-id, default target) *written*
- [x] Stage 5: Linux kernel build — *script done*
- [x] Stage 6: initramfs assembly — *script done*
- [ ] ▶ **Actually compile it** on real hardware and fix what breaks
- [ ] 🎯 **Boots to a login prompt in QEMU**

> Legend: *script done* = the build logic is written and syntax-clean, but the
> heavy compile hasn't been run/validated on real hardware yet. The remaining
> unchecked boxes are about *running* the build and fixing real-world breakage.

## Phase 2 — Multi-arch

- [ ] aarch64 via qemu-user native builds → boots in QEMU
- [ ] i686 → boots in QEMU
- [ ] Shared CI that builds all three

## Phase 3 — A real system

- [ ] Networking (systemd-networkd, DNS, DHCP)
- [ ] SSH, package fetching tools
- [ ] A minimal package format / build recipes (Beyond-LFS style)
- [ ] GRUB + disk images → boots on real hardware (a physical PC, a Raspberry Pi)

## Phase 4 — Desktop

- [ ] Wayland + a compositor
- [ ] Graphics stack (Mesa, libinput, fonts)
- [ ] A minimal desktop session
- [ ] Audio (PipeWire)

## Phase 5 — Make it ours

The long game — replace stack components with our own:

- [ ] Custom initramfs init / early userspace
- [ ] Our own coreutils-equivalents where it's instructive
- [ ] Eventually: **a custom kernel** (the original dream), starting as a bootable
      toy kernel on x86_64 and growing from there

## Parked ideas

Things we want to do but not yet:

- [ ] **Integrate Alex's own software** into VerkOS in some fun way — e.g.
      preinstall his apps in the image, ship them as systemd services, or write
      package recipes for them. (Details TBD — Alex to describe the software.)

---

*Milestones are deliberately front-loaded on "get something booting" — everything
after Phase 1 is more fun when there's a running system to test against.*
