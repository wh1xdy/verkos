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

## D-003 — systemd as PID 1  *(superseded by D-009)*

> **Reversed on 2026-07-25.** systemd is no longer built or installed; VerkOS'
> own init, `urverk`, is PID 1. The reasoning below is kept because it was right
> at the time — the reason it stopped applying is in D-009.

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

## D-006 — QEMU direct kernel boot now, GRUB later  *(superseded by D-010)*

> **Reversed on 2026-07-25.** VerkOS boots through its own UEFI bootloader,
> `vboot`; GRUB was never adopted. See D-010.


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

---

## D-008 — Python build tools, staged offline

**Decision:** systemd is built with meson/ninja and needs python3 + jinja2. We
build **libffi, Python, and ninja from source** inside the chroot, and install
**meson, jinja2, markupsafe** with `pip --no-index --no-build-isolation` from
**sdists fetched up front** (`make fetch` downloads them into `sources/pip/` on
the host, which has network + TLS).

**Why:** It keeps the chroot build **fully offline** and avoids dragging in the
classic LFS dependency avalanche just to satisfy a build tool — no OpenSSL (so
Python needs no TLS for pip) and no Perl. All sources are still fetched and
present before the build starts, matching how the rest of VerkOS is built.

**Cost we accept:** `make fetch` needs a host `pip`. If it's absent, the tools
aren't staged and the build stops with a clear message rather than
failing deep in the build.

**Rejected:** Building OpenSSL + Perl so Python can pip-install over the network
mid-chroot (larger, network-dependent build); hand-vendoring meson (brittle).

---

## D-009 — Our own init: `urverk` replaces systemd

**Decision:** Write VerkOS' init and service manager, **urverk**, and remove
systemd from the build entirely.

**Why:** By the time the question came up, most of what systemd was chosen for
(D-003) had already been replaced by our own tools — networking by `vdhcp`, the
console login by `vgetty`, the userland by `verkbox` and the vX tools. systemd
was left doing service supervision and early boot, both of which are tractable,
and it was the largest remaining component nobody in the project understood
end to end. Owning PID 1 is also what makes the rest coherent: one binary is both
the initramfs `/init` and PID 1, so there is a single place where "how VerkOS
starts" lives.

**Cost we accept:** everything systemd quietly did now has to be done
explicitly, and each omission is a bug we get to find. The loopback interface was
one — nothing brought `lo` up after the removal, and every connection to
127.0.0.1 hung until urverk took the job over. Expect more of that shape.

**Also:** shadow had to be rebuilt with `--disable-logind`, because its
`login`/`su`/`passwd` link `libsystemd` by default. Removing systemd without that
would have made it impossible to log in.

**Rejected:** keeping systemd for logind/udev alone (it is the whole package or
nothing); runit or s6 (excellent, but adopting someone else's init contradicts
the point).

---

## D-010 — Our own bootloader: `vboot`, not GRUB

**Decision:** Write a **UEFI bootloader in C** rather than adopting GRUB.

**Why:** GRUB solves a much larger problem than we have — BIOS, dozens of
filesystems, its own scripting language — and cross-building it is notoriously
awkward. A UEFI application, by contrast, is an ordinary C program: the firmware
gives us services, the kernel is already a valid EFI binary through
`CONFIG_EFI_STUB`, so loading it is `LoadImage` + `StartImage` with no
architecture-specific boot protocol to reimplement. One source tree covers both
aarch64 and x86_64; the only real difference is the calling convention.

**Cost we accept:** UEFI only. There is no BIOS path, so genuinely old x86
hardware cannot boot VerkOS. That is a deliberate trade for a bootloader we can
read in an afternoon.

**Cost we did not expect:** producing a PE/COFF image that a strict EDK2 loader
accepts took three separate fixes — section layout, the relocations-stripped
flag, and keeping dynamic-linking metadata out of `.data`. All three are recorded
in `pkg/vboot/build.sh` and the linker script, because each one presented as a
silent, misleading failure.

**Rejected:** GRUB (size and cross-build pain); systemd-boot (would have meant
keeping systemd); writing a BIOS bootloader (16-bit assembly for hardware we do
not target).

---

## D-011 — Verify what boots, with our own Ed25519

**Decision:** vboot verifies the kernel and initramfs against a public key
compiled into it, using an Ed25519 implementation in the tree.

**Why:** The bootloader is the only place where "is this the software I
installed?" can still be asked before anything untrusted runs. Since the
firmware loads vboot, embedding the key in vboot makes the bootloader itself the
trust root. Ed25519 was chosen for small keys and signatures and an
implementation that fits in a header with no libc.

**Cost we accept:** rolling our own crypto, which is normally poor advice. It is
mitigated by testing rather than confidence: `pkg/vboot/ed25519-test.c` checks
the RFC 8032 vectors and that tampering is rejected, and it runs in CI. The rule
is that the crypto is not trusted until those pass.

**Enforcement is conditional:** with no key configured (the default, an all-zero
key) vboot boots without checking. Otherwise turning the feature on would have
made every existing install refuse to start.

**Rejected:** a pinned SHA-256 (no way to re-sign a kernel without rebuilding
the bootloader); linking a crypto library (nothing to link against before the OS
exists); UEFI Secure Boot with vendor keys (a different problem — that is about
whose firmware trusts us, not whose kernel we trust).

## D-012 — Our own module tools: `verkmod` replaces kmod

**Decision:** one multi-call binary (`pkg/verkmod/verkmod.c`) provides `insmod`,
`rmmod`, `lsmod`, `modinfo`, `modprobe` and `depmod`.

**Why:** kmod was removed together with systemd, which left the ~1300 modules
the kernel installs unloadable — the system booted only because everything it
needed was compiled in. Loading is two syscalls (`finit_module`,
`delete_module`); the only real work is answering "which file is module X, and
what does it need first?", and the answer is in each module's `.modinfo` ELF
section, which is where depmod reads it too.

**`.modinfo` is the source of truth, `modules.dep` is a cache.** modprobe
resolves dependencies by reading the modules themselves, so a stale or missing
index can never cause the wrong module to load. `depmod` still writes
`modules.dep` because other software and people expect the file, and
`scripts/50-kernel.sh` runs it at build time (`depmod -b ROOTFS RELEASE`) so a
freshly installed system has an index before anything runs depmod itself.

**Verified by loading, not by exit codes** (the lesson from vbzip2): on a booted
system `modprobe btrfs` pulls in `zstd_compress`, `raid6_pq`, `xor` — which
itself needs `xor_neon` — and `libcrc32c`, after which the kernel reports the
refcounts and btrfs appears in `/proc/filesystems`; `rmmod` of a module still in
use is refused. Three bugs were found this way and none of them changed an exit
code:

- `load_deps` used `strtok` and recurses. The first dependency that had
  dependencies of its own reset the shared parser state, so the outer loop
  ended early and silently skipped the rest — the module then failed on an
  unresolved symbol. Now `strtok_r`.
- `depmod` re-walked the whole tree per dependency, ~1300 × 1300 stats. It now
  walks once into an in-memory index: minutes to 0.5 s.
- Dependency names use `-` and `_` interchangeably (`tps6594-core` for a file
  named `tps6594_core.ko`). Normalising only the filenames hid the
  dependencies of 300 modules.

`modules_install` also leaves `build`/`source` symlinks pointing into the build
tree, a second copy of every `.ko`; indexing them doubled the index and resolved
dependencies to the build tree. They are removed at install time and skipped by
the walker. `scripts/test.sh` covers all of this against a fixture tree built
from ordinary executables carrying a `.modinfo` section.

## D-013 — Our own account tools: `vlogin` replaces shadow

**Decision:** one multi-call binary (`pkg/vlogin/vlogin.c`) provides `login`,
`su`, `passwd`, `chpasswd`, `useradd`, `usermod`, `userdel`, `groupadd`,
`groupdel`, `pwconv` and `nologin`. shadow is no longer built or fetched.

**Password hashing stays libxcrypt's, deliberately.** Everything else in VerkOS
is ours, including the Ed25519 in vboot (D-011), but a hand-rolled password hash
is a different bet: it is the one place where a subtle mistake is silent,
permanent and only discovered by an attacker. `crypt_gensalt(NULL)` picks the
strongest scheme libxcrypt supports (yescrypt) and `crypt()` verifies every
scheme it knows, so hashes shadow already wrote keep working — verified by
logging in as root with the password shadow's `chpasswd` had set, using a
`$y$` hash it produced.

**Reads go through libc** (`getpwnam`/`getspnam`/`getgrnam`) so nsswitch still
applies; **writes** rewrite the file into a sibling temp file, fsync, and
rename, under `lckpwdf()` — the same lock shadow takes. A crash mid-write
cannot leave `/etc/shadow` truncated, which would lock everyone out.

**`su` and `passwd` are setuid root; nothing else is.** They are the only tools
an unprivileged user runs that must reach `/etc/shadow` (0640). This is exactly
where the test suite earned its keep: with the binaries not yet setuid, `su`
rejected the *correct* password, because as an ordinary user it could not read
the hash at all. Privilege is dropped in the order that matters — `setgid`,
then `initgroups`, then `setuid`, every return value checked — and then proved
gone by asserting `setuid(0)` fails. Verified on a booted system: the shell su
starts has both real and effective uid 1000, cannot become root without a
password, cannot read `/etc/shadow`, cannot add or remove accounts, and cannot
use `login -f` to skip authentication.

**A new account is locked (`!`), never passwordless** — `useradd` writes a
locked shadow entry, so an account that exists but has had no password set
cannot be logged into.

**Not carried over from shadow:** `chage`, `chfn`, `chsh`, `expiry`, `faillog`,
`gpasswd`, `newgrp`, `sg`, `groupmod`, `groupmems`, `grpck`, `pwck`,
`pwunconv`, `vipw`, `vigr`, `newusers`, `chgpasswd`, `logoutd`. They can come
back if something needs them.

**Known limitation:** a session's utmp entry is not marked dead when the session
ends, so `last` reports "gone - no logout" for finished `login` sessions.
`/run/utmp` is created by urverk at boot (it is a fresh tmpfs each time, and
`pututxline` cannot create the file itself) — without that, `who` and `w` had
nothing to read at all.

## D-014 — Making filesystems: our own FAT32, e2fsprogs for ext4

**Decision:** `pkg/vmkfs/vmkfs.c` is VerkOS' own FAT32 formatter; ext4 is
created by e2fsprogs, which is now built.

**Why the split.** An installer that cannot create an EFI system partition
cannot install anything, and UEFI requires that partition to be FAT. FAT32 is
small and specified precisely enough to own: a boot sector describing the
layout, two copies of a table of 32-bit cluster links, and a root directory
that is itself a cluster chain. ext4 is a different proposition — journal,
extents, group descriptors, checksums — and a subtle mistake there is silent
until someone's data is gone. Owning the easy half and buying the hard half is
the honest trade.

**The judge is deliberately not us.** `fsck.fat` accepts what vmkfs writes at
33 MB, 100 MB and 512 MB; `blkid` reports vfat with the right label; the kernel
mounts it and files come back byte-identical, including after real use. The
geometry lands within 12 sectors of dosfstools on the same volume.

Testing found one defect: the volume serial came from `time()` alone, so every
filesystem made in the same second shared a UUID and `blkid` could not tell two
disks apart. It comes from `/dev/urandom` now, with the clock as fallback.

**FAT32 only, on purpose.** FAT12 and FAT16 differ in the width of every table
entry and in having a fixed-size root directory. Supporting them would roughly
double the code to serve volumes under 33 MB — smaller than any sane ESP — so
vmkfs refuses those rather than quietly producing something mislabelled.

**e2fsprogs also wakes up a feature already written:** urverk has always had a
boot-time fsck that skipped itself because no `fsck.ext4` existed.

## D-015 — `vinstall`: the installer refuses rather than guesses

**Decision:** `pkg/vinstall/vinstall.c` writes a running VerkOS onto a disk:
GPT, a FAT32 ESP holding vboot and the kernel, an ext4 root, fstab and
vboot.conf keyed by UUID.

This is the only tool in VerkOS that destroys data on purpose, so the design
rule is that it refuses instead of guessing. Verified: it rejects a target that
is not a block device, one with anything mounted on it, one that does not
exist, and the disk the running system booted from. It checks that every tool
it needs exists **before** touching anything. `-n` leaves the disk
byte-identical.

**Confirmation is the disk's name, not "y".** The question is not "are you
sure" — nobody misreads that carefully — it is "is this the disk you meant".

**Root is referred to by UUID**, in both fstab and vboot.conf: the name a disk
gets depends on what else is plugged in, and an installed system has to survive
that.

**`-s DIR` installs from a tree instead of the running system**, which is what
an ISO will do, and what makes the installer testable without a live medium.

**Verified end to end** on aarch64: a bare 4 GB disk, installed onto from a
running VerkOS, then booted on its own with nothing else attached — UEFI →
vboot → kernel → initramfs → urverk → sshd, root and /boot both resolved by
UUID, 25/25 tools and 75 packages present, and the installer on the installed
system correctly refusing to install over the disk it is running from.

Booting what it produced is what found the defects; none of them changed an
exit code, and the install "succeeded" every time:

- **The config was written in the wrong language.** vboot's keys are
  `kernel`/`initrd`/`cmdline` with EFI (backslash) paths; the installer wrote
  systemd-boot's `linux`/`initrd`/`options` with Unix paths. vboot fell back to
  a kernel it found on the ESP, so the machine still booted — with no initrd
  and an empty command line — and panicked mounting root.
- **Copying the source's `/boot` overwrote the new ESP.** The target's EFI
  partition is mounted there, so the live medium's vboot.conf landed on the
  installed system and won, pointing root at the *live* disk.
- **The source was only checked after the target was destroyed.** A missing
  bootloader was discovered after partitioning, formatting and copying. Every
  requirement is now verified before the disk is touched.
- **urverk could not resolve the fstab entry the installer wrote.** It passed
  `UUID=1730-7CA7` straight to `mount(2)`, which wants a device node, and only
  knew how to read an ext superblock anyway. It now reads FAT boot sectors too,
  so `/boot` mounts. Without this the ESP was silently absent — and an
  installed system cannot install onward, or update its own kernel, without it.

## D-016 — The image build refuses a rootfs it does not recognise

Two guards now run before `make image` packages anything, both because
something got through:

**Nothing may be mounted inside the rootfs.** The trim does
`rm -rf $ROOTFS/sources/*`, and `$ROOTFS/sources` is a bind mount of the
shared, SHA-verified tarball cache during a build. A leaked mount turns a
cleanup into deleting every source the project has downloaded. It also silently
packages whatever is mounted — which is how a 589 MB image became 4.0 GB
without anything reporting a problem.

**The top level must have plain-text names.** vtar, handed a compressed file it
could not read, treated the compressed bytes as tar headers and created 36
files and directories named after that binary garbage, in whatever directory it
was running from — during a build, `/`. The extraction reported success, the
corruption was copied into the image, and it reached a booted VM. It was only
noticed because the installer enumerated `/` and tried to `cp` a name full of
control characters, which the shell rejected as a syntax error.

The lesson is the same one vbzip2 taught: a tool that fails without saying so
does not merely fail to work, it leaves damage somewhere nobody is looking.

## D-017 — Hunting the tools that succeed without working

A probe ran every VerkOS tool through cases that must fail — unknown options,
input in the wrong format, missing files, unwritable output — and reported any
that exited 0. Eighteen did. This is the shape that has cost this project the
most: vbzip2 `-dc` destroyed the file it was given, vtar extracted nothing from
a `.tar.xz` and scattered 36 garbage-named files into `/`, and both reported
success.

Fixed, worst first:

- **vcpio extracted outside the directory it was unpacked in.** An archived
  `/etc/passwd` overwrote the real one; `../` climbed out. Leading slashes are
  stripped with a notice, as tar does, and a `..` component is refused.
- **vcpio read a non-archive, printed "bad magic", and exited 0.**
- **vsed and vawk printed "cannot open" for a missing input file and exited 0**,
  so a script could not tell an unreadable file from an empty one. Both exit 2
  now, matching GNU.
- **vip installed a route without its gateway** when the gateway address did not
  parse — quietly not the route that was asked for.
- **vdate ignored `-d` forms it could not parse** and printed the current time
  as though it were the requested one. It also ignored `-r FILE` when the file
  was missing.
- **vtree printed "0 directories, 0 files" for a directory that did not exist**,
  which reads exactly like an empty one.
- **vgzip, vhex, vtree, vdate, vawk, vcpio and vxargs ignored unknown options.**
  In vxargs the unknown option silently became the command to run.

Left deliberately: `vpager` ignores unknown flags (less has dozens, and a pager
that refuses `-R` is worse than one that ignores it — and its output is on
screen, so nothing is hidden), and `ved` treats an unknown argument as a file
name, which is what vi does.

`scripts/test.sh` now runs the whole class as regression tests.

## D-018 — An install must not inherit the image's password

**Decision:** `vinstall` sets the passwords for the system it installs, and
refuses to run unattended without being told what they should be.

The source system's `/etc/shadow` is copied along with everything else, so
without this every installed machine kept whatever root password the image was
built with — for a VerkOS image, the development default that is written down
in this repository. The installer now prompts (twice, without echo) when it has
a terminal, takes `--passwd-file FILE` in chpasswd format when it does not, and
`--lock-root` for installs that should only be reachable through a named
account — which it refuses without `-u`, since that would leave no way in at
all.

Passwords are applied with the **installed system's own chpasswd**, so the
hashing is whatever that system would have produced itself.

Verified by the check that matters: after installing, the image's `verkos`
password is rejected and the one given at install time is accepted.

**Also added:** `--swap MB` and `--home MB`. Both surfaced a gap in urverk —
its fstab handling resolved `UUID=` only for ext filesystems, and only after
the swap branch had already run. It reads FAT boot sectors and swap headers
now, and resolves before anything uses the device. Without that, an fstab line
the installer had just written was quietly ignored: `/home` stayed on the root
partition and swap never came on, with nothing reported.

**Not done yet:** installing onto existing partitions (dual boot) and
encryption. Encryption in particular is an ext4-shaped decision — LUKS is not
something to hand-roll.

## D-019 — Installing beside another system, without taking it over

**Decision:** `vinstall --root-part DEV --esp-part DEV` installs into partitions
that already exist, and VerkOS announces itself to the firmware with its own
`Boot####` variable rather than by claiming the fallback loader path.

**Why the NVRAM entry is the point.** `\EFI\BOOT\BOOTAA64.EFI` is the
removable-media fallback. On a disk we partitioned it is ours to write; on a
machine that already boots something else it belongs to that something else, and
overwriting it makes that system unbootable. So on a disk we were only handed a
partition of, the fallback path is left exactly as it is (unless nothing is
there), our loader goes to `\EFI\verkos\`, and a boot entry is written straight
into efivarfs — the same thing efibootmgr does, which VerkOS does not have.

The variable holds a load option: attributes, the device path length, a UTF-16
label, then a device path naming the partition by its **GPT GUID** — so it keeps
working when disks are renumbered — followed by the file on it. `BootOrder` is
rewritten with the new entry first and everything that was already there kept
after it. efivarfs marks variables immutable, so the flag comes off first and
the write includes the attribute word in one call.

**Proved by the firmware's own log**, not by inspecting what we wrote:

    BdsDxe: loading Boot0003 "VerkOS" from
      HD(1,GPT,48FD33FD-…,0x800,0x40000)/\EFI\verkos\BOOTAA64.EFI

**What it refuses:** a whole disk where a partition is required, a partition
that is mounted or is the running root, and an "ESP" that is not FAT — that
last one otherwise produces a system the firmware cannot boot, discovered only
at the next reboot. The ESP is never formatted in this mode, and the
confirmation says in as many words which partitions are erased and which are
only added to.

**Still not done:** encryption. LUKS is an ext4-shaped decision — not something
to hand-roll — so it means taking cryptsetup as a dependency.
