# Design decisions

This is the running log of *why* VerkOS is built the way it is. Every entry is a
decision we actually made, with the reasoning and the alternatives we rejected.
When we change our minds, we add a new entry rather than deleting the old one.

---

## D-001 — Distro first, custom kernel later

**Decision:** Build a Linux distribution from source (LFS-style) as Phase 1.
Replace/rewrite parts of the stack — up to a custom kernel — as Phase 2.

**Why:** Building a distro teaches the whole stack (toolchain → kernel → libc →
init → userland → boot) and gives us something bootable and useful early. Writing
a kernel first would mean a long time before anything runs, and we'd understand
less of what sits on top of it.

**Rejected:** Start with a hand-written kernel (too slow to first-usable);
Buildroot/Yocto (less learning — they hide the hard parts).

---

## D-002 — glibc as the C library

**Decision:** Build on **glibc**.

**Why:** We want a *usable*, eventually *desktop-capable* system. glibc has the
broadest compatibility with existing software (browsers, toolkits, proprietary
blobs), which matters once we add a desktop. It's the classic LFS choice, so the
well-trodden LFS book maps directly onto our build.

**Cost we accept:** glibc is larger and its multi-arch story is fiddlier than
musl's. We mitigate this by building **natively per architecture** (see D-005)
rather than fighting cross-compilation for everything.

**Rejected:** musl (smaller, cleaner, easier multi-arch — but weaker binary
compatibility for desktop software; revisit if we ever want a tiny embedded VerkOS).

---

## D-003 — systemd as PID 1

**Decision:** Use **systemd** as the init system and service manager.

**Why:** It's the de-facto standard for modern desktop Linux. Choosing it now
means the desktop stack (logind, udev, D-Bus activation, journald) lands on
familiar foundations instead of us reinventing service management.

**Cost we accept:** systemd is heavy and genuinely hard to cross-compile. This
reinforces the native-build model (D-005): we build systemd on/for each target
natively rather than cross-compiling it.

**Rejected:** BusyBox init (too minimal for the desktop goal); runit (lovely and
small, but we'd have to build the desktop session plumbing ourselves).

---

## D-004 — Three architectures: x86_64, i686, aarch64

**Decision:** Support **x86_64** (primary), **i686** (32-bit x86), and
**aarch64** (64-bit ARM, e.g. Raspberry Pi).

**Why:** The user wants both ARM and x86, 32- and 64-bit. x86_64 is the primary
development target (fast in QEMU); aarch64 gets us onto real, cheap hardware;
i686 keeps us honest about portability and revives old machines.

**Primary target:** `x86_64`. The others follow the same pipeline via per-target
config in `config/targets/`.

---

## D-005 — Cross-toolchain bootstrap, then native builds

**Decision:** Bootstrap each architecture with a **cross-compilation toolchain**
(binutils → gcc → glibc → gcc), then build the rest of the system **natively**
inside a chroot (native arch) or under QEMU user-mode emulation (foreign arch).

**Why:** This is exactly how Linux From Scratch works, and it's the only sane way
to get glibc+systemd on three architectures. Cross-compiling all of userland
(especially systemd) is a well-known source of pain; native builds inside the
target environment sidestep it.

**Rejected:** Pure cross-compilation of the entire system (fragile, endless
`configure` breakage); building only on native hardware for each arch (we don't
have three machines — QEMU user-mode lets one host build for all).

---

## D-006 — QEMU direct kernel boot now, GRUB later

**Decision:** During development, boot with **QEMU loading the kernel + initramfs
directly** (`-kernel`/`-initrd`). Add **GRUB** and disk images when we target real
hardware.

**Why:** Direct kernel boot is the fastest iteration loop — no bootloader, no
disk image, works identically across all three arches. GRUB matters only when we
boot physical machines, which is a later milestone.

---

## D-007 — Name: VerkOS

**Decision:** The distribution is named **VerkOS**.

**Why:** "Verk" is Swedish for *work/creation* and is essentially unused as an OS
name (a web search found only unrelated AI-SaaS products, no operating system).
The `OS` suffix disambiguates from those products. Short, distinct, brandable.

**Note:** We may spin off differently-named variants later (e.g. a musl-based
embedded build). VerkOS is the flagship glibc+systemd desktop-track distro.
