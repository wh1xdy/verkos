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

## D-020 — Encrypted root: LUKS2, opened by urverk before there is a root

**Decision:** `vinstall --encrypt` puts the root filesystem inside a LUKS2
container, and urverk's initramfs stage finds it, asks for the passphrase and
opens it before anything is mounted. cryptsetup does the cryptography; we do
not.

**Why cryptsetup rather than our own.** This is the same judgement as ext4 in
D-014, only sharper: a mistake in a disk encryption implementation is silent
and unrecoverable. What VerkOS owns here is the *plumbing* — finding the
container, asking for the passphrase, and handing it over — not the key
derivation or the cipher.

**How the pieces fit.** The installer writes `rd.luks.uuid=<container>` and
`root=/dev/mapper/cryptroot` into vboot.conf. urverk reads the LUKS header
directly to find which partition holds that container — nothing else can
identify an encrypted partition, because it has no filesystem to read until it
is open — then prompts and execs cryptsetup with the passphrase on stdin.
Both LUKS1 and LUKS2 put the UUID at offset 168, so one reader covers them.

cryptsetup and its ten shared libraries are copied into the initramfs by
following the whole dependency chain rather than the direct NEEDED entries:
libcryptsetup names json-c, devmapper, popt and OpenSSL, and those name more. A
library missed here would only show up as "error while loading shared
libraries" at the moment the disk needs unlocking.

The kernel now has `DM_CRYPT`, `BLK_DEV_DM` and `CRYPTO_XTS` built in rather
than as modules: a module would have to be loaded from inside the initramfs
before the root can be opened, which means carrying the modules and a loader
there for no benefit.

**Verified end to end**, not by inspection: the partition reports
`crypto_LUKS` with `aes-xts-plain64` and refuses to mount without the
passphrase; the disk boots on its own; urverk finds the container by UUID,
prompts, opens it, and the system comes up with root on
`/dev/mapper/cryptroot`.

**What it cost.** Reaching this needed cmake, json-c, libdevmapper and
cryptsetup, and those needed a native GCC — which had never been built here,
because the flag that enables it never reached the chroot. Getting through that
found twelve defects in VerkOS' own tools, most of which produced a wrong answer
with a zero exit status. They are listed in D-021.

## D-021 — What building cryptsetup found in our own tools

Adding one feature put VerkOS' own userland in front of real third-party build
systems — autotools, gnulib, kwsys, libtool, pax archives — for the first time
at that scale. Twelve defects surfaced. Most produced a **wrong answer with a
zero exit status**, and none was found by reading code:

**vtar** (five). Requiring the ustar magic rejected pre-POSIX archives, which
popt still ships — a header is told from garbage by its checksum instead.
Extraction discarded modification times, so an unpacked tree looked newer than
its own generated files and autotools tried to re-run automake. Paths over 100
characters were truncated: not merely lost, but written to the *wrong* path,
which cost 465 of cmake's 26212 files. `-p` was refused, and `-` was treated as
a file name rather than standard input, so GCC's header install moved nothing.

**vawk** (two). An FS longer than one character is an extended regular
expression, not a literal; using only its first character made `-F'[(, ]+'`
split on `[`. A regex literal's `\t` was passed to regcomp, which reads it as
the letter t. Both produced empty fields where GCC expected CPU names, and its
build refused to continue with an error naming a generated file.

**vsed** (two). A command written immediately after `{` was swallowed by the
skip to the next separator, so a block ran and did nothing — cmake's namespace
substitution came out half-applied. `w FILE` was parsed and discarded, so
gnulib's generated headers never appeared.

**verkbox** (three). `sort` had no `-c`, so libtool's duplicate-object check
always "failed" and it reported conflicts in archives that had none. It had no
`-t`/`-k` either, so LVM2's version script was written empty and ld reported a
syntax error on line 0. `rm` had no `-v`.

**The lesson is about where to look.** A deliberate sweep for silent failures
earlier in the same session — running every tool through unknown flags, wrong
formats, missing files — found eighteen problems and none of these. Synthetic
probes test what we thought to ask. Real software asks things we did not think
of, and it asks them in combinations: `sort -nt_ -k5`, `tar xpf -`,
`sed -n -e 'w $@-t'`. Every one of these was found by a build failing somewhere
far from the cause.

**Also worth knowing, not bugs:**
- The cross toolchain's `libstdc++.a` is a stub — no sized delete, no
  `basic_string`. **Static C++ linking does not work** until a native GCC has
  been built. GCC's own build links its tools that way, so it needs
  `--with-stage1-ldflags=no`.
- An interrupted `make install` of GCC leaves the system unable to rebuild
  itself: the new driver is in place but its internal headers are not, so gcc
  cannot find `stddef.h` and g++ segfaults. Recovery depended on the cross
  toolchain's own drivers still being present, which is luck rather than design.
- 46 GB for one package's build directory on a 79 GB volume is not sustainable;
  the build does not clean up after itself.

**Corrected on 2026-07-26: this entry said "eight" and enumerated more.** The
headline was written before the per-tool lists were, and never revisited; the
four counts below it summed to thirteen, and `vawk` said "(three)" while its
own paragraph described two. Settled against the commits rather than against
either number: vtar five (`a2a1abc` three, `959415d` two), vawk two (`b32054f`,
`565889c`), vsed two (`0cdcbbb`, `bf85e67`), verkbox three — **twelve**. D-028,
D-033 and STATUS.md cited the eight and were corrected with it.

It is worth noting what this was. The entry is *about* reports that do not depend
on what they report, and its own headline did not depend on the list underneath
it. Nothing here was wrong about the defects; the summary just stopped being
recomputed when the detail grew.

## D-022 — Two ways to run a system that forgets itself

**Decision:** `vinstall --immutable` keeps the root on disk but mounts it
read-only under a tmpfs overlay. `vinstall --ramdisk` goes further: there is no
root partition at all — the system is one ext4 image on the EFI partition,
loop-mounted at boot with the same overlay on top, and `rd.toram` copies it
into RAM so the disk stops mattering once boot finishes.

**Why both.** They answer different questions. An immutable install is a
machine that must come up identical every time but still holds its own data —
the changes cost tens of megabytes of RAM, not the 958 MB the system occupies.
A diskless install is a machine where the disk is only a delivery mechanism.
Offering only the first would not be "running a ramdisk"; offering only the
second would demand a gigabyte of RAM to get ephemerality.

**One mechanism, three uses.** urverk gained exactly three options, and between
them they cover both modes *and* the live ISO still to come:

    rd.image=PATH   the real root is a filesystem image on the device root= names
    rd.toram        copy that image into RAM first
    rd.overlay      stack a tmpfs over whatever the root turned out to be

An immutable install is `rd.overlay`. A diskless one is `rd.image` +
`rd.overlay` (+ `rd.toram`). A live ISO will be the same with `root=` naming
the medium. Loop attachment is done with ioctls rather than by running
`losetup`, so the initramfs does not have to carry another binary to hand three
numbers to the kernel.

**Refusals.** `--ramdisk` with `--immutable` (it already forgets everything),
with `--encrypt` (the image would sit unencrypted on the ESP, which is not what
asking for encryption means), and with `--swap`/`--home` (nothing is kept).

**`mkfs.ext4 -d` was the obvious way to build the image and is wrong here.** It
populates a filesystem straight from a directory tree with no intermediate
copy — but it has no way to exclude anything, and the image has to live on the
EFI partition, which is mounted *inside* the tree being copied. It copied
itself into itself until it ran out of blocks. `copy_system()` already knows
what to skip, and `/run` — where the ESP is mounted — is on its list.

**Verified across two boots each**, which is the only way this can be checked:
a file written on the first boot is gone on the second, and `/proc/mounts`
shows the root as `overlay`.

## D-023 — Two things vboot got away with only because it had met one filesystem

Hardening before the live ISO, on the reasoning that a failed ISO boot should
be attributable to the ISO and not to something vboot had always got wrong.

**A single Read is not a whole file.** `read_file()` sized the file, allocated a
buffer, and issued one `Read` — then used whatever count came back as the
length. EFI explicitly permits a filesystem driver to return fewer bytes than
asked for, so a short read would have produced a truncated kernel or a
half-parsed config with no error reported anywhere. FAT satisfies the whole
request, which is why this never showed. It reads in a loop now, and says so if
the file turns out shorter than its size claimed.

**The filesystem decides the case of a name, not us.** `scan_kernels()`
compared `vmlinuz` byte for byte in lower case. An ISO 9660 volume without Rock
Ridge presents `VMLINUZ.;1`, so on such a medium vboot would have found no
kernel and offered nothing to boot — while reporting no error, because finding
no kernels is a legitimate outcome.

The comparison now lives in `pkg/vboot/kernelname.h`, separate from any
firmware, so it can be tested on the build host: ten cases covering lower,
upper and mixed case, the ISO version suffix, and near-misses that must not
match. Verified on real hardware terms too — a file called `VMLINUZ.UPP` placed
on the ESP now appears in the menu, and the system still boots.

Neither defect was reachable from anything VerkOS had booted from so far. That
is the point: they were waiting for the first medium that behaves differently.

## D-024 — The live ISO, and the three things that stood between us and it

**Decision:** `make iso` produces a bootable live medium carrying two files —
a FAT32 EFI image (vboot, kernel, initramfs, a live boot configuration) and the
ext4 system image — with the EFI image registered as an El Torito no-emulation
entry. It boots: firmware → vboot → kernel → initramfs → urverk mounts the ISO
by its volume label, loop-mounts the system image out of it, and stacks a tmpfs
overlay. The installer is on the medium and runs from it.

**vboot needed no changes**, which was the design's central bet: the firmware
presents the embedded FAT image as an ordinary volume, so vboot opens it exactly
as it does a disk's ESP. The bet was hedged rather than trusted — D-023 fixed
the two ways it could have bitten before the ISO was ever built, so that when
the first boot failed, the failure was unambiguously somewhere else.

It failed three times, each somewhere different, and each worth recording:

1. **urverk mounted the medium writable.** `rw` on the command line is a request
   aimed at the real root; on a live medium the root is an image *on* the
   filesystem, and the filesystem itself is only ever read. ISO 9660 answers
   EACCES. It retries read-only now.
2. **The loop device was configured but the image would not mount.** Not the
   loop attachment — that worked, with the right size, straight out of the ISO.
   The image had `needs_recovery` set, and ext4 will not replay a journal onto
   a device it cannot write to, so it answers EROFS. Fixed on both sides: stage
   71 runs `e2fsck -fp` so the shipped image is clean, and urverk falls back to
   `noload`, because a live medium can never replay a journal whatever we do.
3. **The EFI image came out too small.** `$OUT_DIR/vmlinuz` is a symlink, and
   `stat -c%s` measures the link — fourteen bytes rather than forty-five
   megabytes. `stat -L`.

urverk also learned to read ISO 9660 volume labels, since nothing else can
identify a live medium: it has no filesystem the other probes understand until
it is mounted. Writing that reader produced a stack overflow the compiler
caught — the label field is 32 characters, longer than any other format's, and
the callers' buffers were sized for the shortest.

**Deliberately not done:** no isohybrid MBR/GPT (so the ISO is not yet
`dd`-able to a USB stick as a hybrid image), no Joliet or Rock Ridge (the
system ships as one opaque image, so the ISO needs neither), and no squashfs
compression — the medium is 1.3 GB because the system carries a full compiler.

## D-025 — Trimming the live medium: 1.3 GB to 731 MB

Two findings, one of which was waste in every VerkOS install and not only the
ISO.

**The superseded cross toolchain was still installed.** When the native GCC was
built it added its own triple (`aarch64-unknown-linux-gnu`) and left the cross
one (`aarch64-verk-linux-gnu`) in place: two complete copies of `cc1`,
`cc1plus` and `lto1` side by side, 150 MB. Nothing referenced the older one —
checked before removing, by asking gcc itself for its search directories, its
C++ include paths and the specs files it reads. Removed in stage 60's trim, and
the compiler is exercised afterwards: C and C++ both still build and run,
including `std::mutex`.

**A live image needs neither headroom nor a journal.** Stage 60 sizes the
system image for a *writable installed* system — a quarter of slack, and a
journal. On a medium that can never be written, both are dead weight, and the
journal is worse than dead: an image with `needs_recovery` set cannot be mounted
read-only at all, which is what cost a boot attempt in D-024. Stage 71 now
builds its own image with `mkfs.ext4 -O ^has_journal`, sized to the files plus
five per cent. This removes the problem at the source rather than working around
it at every boot — urverk keeps its `noload` fallback for media built elsewhere.

    system image   1098 MB → 648 MB
    ISO            1.3 GB  → 731 MB

`mkfs.ext4 -d` is used here, which D-022 rejected for the diskless installer.
The difference is where it runs: on the build host the rootfs is an ordinary
directory with nothing mounted inside it (stage 60's guard enforces exactly
that), so the self-inclusion that broke the installer cannot arise.

The live ext4 is labelled `VERKROOT` and the ISO volume `VERKLIVE`, so the two
can never be confused by a label lookup.

## D-026 — isohybrid: an MBR, and deliberately no GPT

**Decision:** `viso` writes a 512-byte MBR at the start of the image with one
partition of type 0xEF covering the embedded EFI image. `-M` suppresses it.

Written to a USB stick, an ISO is simply a block device, and firmware looks for
a FAT filesystem — either as the whole device or inside a partition. The El
Torito catalogue that makes it bootable as a CD is not consulted. One MBR entry
gives the firmware something to find, and costs 512 bytes with no checksum and
no second copy to keep consistent.

**No GPT, on purpose.** A GPT would add compatibility with firmware that reads
nothing else, but it requires a backup header at the end of the *device* — which
is the stick, not the image, and therefore not somewhere the image can put it.
Getting that wrong is silent: the medium either boots or does not, with no
diagnostic. That is the shape D-014 says to buy rather than own, so if GPT
becomes necessary it is the point to hand the whole job to xorriso.

**Verified as a USB stick, not by inspection.** The ISO was attached to QEMU as
a `usb-storage` device behind an xHCI controller, which is what a real stick
looks like. The firmware's own log settles which path it took:

    BdsDxe: loading Boot0001 "UEFI QEMU QEMU USB HARDDRIVE 1-0000:00:01.0-1"

— a hard drive, not optical media — and the running system sees `sda` (iso9660)
with `sda1` (vfat) on it, so the partition table is what the firmware read.
Three further checks agree that the partition covers the right bytes: `fdisk`
reports `ef EFI (FAT-12/16/32)`, xorriso resolves the partition back to
`/EFI.IMG`, and mounting at that offset gives the FAT volume with the kernel in
it.

## D-027 — The live medium moves aside, and the ESP is a file

Installing *from* the live medium did not work: vinstall reported

    is the bootloader reachable? NO
    vinstall: no bootloader at /boot/EFI/BOOT/BOOTAA64.EFI

because nothing mounted `/boot` on a live system. The obvious fix — point
`rd.esp` at the ESP by label, as an installed system does — fails on a live
medium:

    mounting the EFI partition at /boot: Device or resource busy

The medium is one device that is *already mounted whole*. The EFI image inside
it is not a partition the kernel can mount separately; it is a region of a
filesystem that is in use. Nothing about hybrid MBRs or El Torito changes that.

So the live path was restructured. urverk mounts the medium, `MS_MOVE`s it to
`/media`, loop-mounts the system image from there, and — because the EFI image
is now reachable as an ordinary *file* — loop-mounts `/media/efi.img` onto
`NEWROOT/boot`. `rd.esp` therefore takes two kinds of value: a leading `/` means
a file on the medium, anything else is a device spec, as before.

The result is that the installer sees the same `/boot/EFI/BOOT/BOOTAA64.EFI` it
would see on an installed system, and needs no live-specific case at all.

## D-028 — A menu entry that named a flag nothing implemented

Testing the ISO's four boot entries one at a time found that the rescue entry
booted exactly like the normal one. The cause was not a typo: the kernel command
line carried `urverk.rescue` faithfully from the menu, and **urverk had never
implemented it**. The flag had been in the boot menu since vboot was written —
in the installed system's configuration too — and choosing it had always been
indistinguishable from choosing the first entry.

Nothing could have caught this except selecting the entry and looking. A boot
option that does nothing does not fail; it silently does something else, which
is the same shape as the twelve defects in D-021.

Two changes:

1. `urverk.rescue` now does what the menu promises: a shell after the root is
   mounted at `/newroot` and before the handover, so the system can be repaired
   from a medium that is not it. Leaving the shell carries on booting.
2. `make test` now checks the invariant directly: every `rd.*`/`urverk.*` token
   that any boot configuration hands to urverk must be a token urverk reads.
   The check found no second instance — but it would have found this one, and
   it is the only defence against the next entry someone adds and forgets.

Writing that check also produced a small lesson of its own: the first version
matched `[a-z_]+` after the dot, so `rd.luks.uuid` was compared as `rd.luks` and
reported as unimplemented. A test that reports a defect that is not there is
worse than no test, because the next person deletes it.

## D-029 — Four defects in vpk, one of them able to delete a running system

Surveying vpk to "finish" it found the opposite of what was expected: no stubs,
no unimplemented commands. Fetching, sha256 verification, building, caching a
`.vpk`, installing, refcounted removal and dependency ordering all work, and a
full cycle was run to prove it — `hello` fetched, built, installed, executed,
removed with all 47 files and no empty directories left, then reinstalled from
the cache in 0.02 s.

The defects were elsewhere.

**`base=yes` did not survive a reinstall.** `record_and_merge` wrote a fresh
`meta` with only `name=` and `version=`, and `cmd_reinstall` has no base guard.
So `vpk reinstall zlib` silently stripped the protection, after which
`vpk remove zlib` was permitted — and the reinstall had just written a `files`
manifest for the removal to follow. On a running system that deletes `libz`,
which nearly everything links. Demonstrated end to end before fixing.

**Refusals exited 0.** `vpk remove <base>` and `vpk remove <not installed>`
printed to stderr and returned success, so `vpk remove x && echo done` printed
`done`. vpk now carries an exit status through to `main`.

**An install that could not write a file still reported success.**
`copy_and_record` did `if(out<0){ close(in); continue; }` and
`if(write(...)!=rn) break;` — an unwritable or truncated file was skipped, still
recorded in the database, and the install printed `installed`. Same for a failed
`readlink`/`symlink`. All of these now fail the install.

**The `.vpk` writer truncated paths over 100 characters.** ustar splits a long
path across `prefix`(155) and `name`(100); `tp_hdr` wrote only `name`, so a long
path was cut and the package unpacked to the wrong place. This is *the same
defect that was fixed in vtar* (D-021) — fixed there, still present here,
because the two have separate tar writers. The reader already understood
`prefix`; only the writer did not.

Three of the four are the D-021 shape again: a wrong answer with a zero exit
status. They were found by reading the code with the question "what returns
success when it should not", after a boot-menu entry that did nothing (D-028)
made that the obvious question to ask.

## D-030 — stam's boot protocol: a device tree, and a shim for everything else

The question was put directly: define our own protocol between loader and
kernel, or follow Multiboot2 or Limine so the loader can boot other kernels
too? It decides how the memory map and the modules are handed over, and it is
hard to change later.

**Multiboot2 was rejected because it is x86-only.** It has no meaningful
aarch64 story, and aarch64 is the architecture VerkOS actually runs on. Adopting
it would give the kernel two different pictures of the world — tags on one
architecture, a device tree on the other — and that split spreads into every
driver that needs to know where something is.

**Limine is the best of the ready-made ones** — x86_64, aarch64 and riscv64,
handing over a memory map, framebuffer, modules and RSDP already prepared. But
adopting it means our bootloader implements someone else's specification, and
we would still write our own for anything Limine does not target.

**A device tree was chosen, because it already is a boot protocol** — and a
better one than most. It carries the memory map, the console, the initrd, the
command line and every device in the machine. It is not a hand-rolled struct
but a standard the whole ARM world shares, which means the kernel has exactly
one way to learn about hardware on every architecture. On x86_64, where
firmware provides no tree, the loader will build one and hang the ACPI RSDP off
`/chosen`; the kernel will not know the difference.

stam uses the standard arm64 `Image` header rather than anything of our own,
so QEMU's `-kernel`, U-Boot and every ARM bootloader can already start it. That
is interoperability without implementing anybody's specification.

**The decision that actually prevents lock-in is the separation**, not the
choice: `bootinfo.h` is what the kernel reads, and a shim fills it in.
`handover_dtb.c` is the first. Supporting Multiboot2 or Limine later is a new
file, not a change to the kernel.

One consequence, found while looking: vboot hands over with `LoadImage` and
`StartImage`, i.e. it treats the kernel as an EFI application, which works for
Linux because its arm64 Image is also a PE binary with an EFI stub. For vboot
to start stam, stam needs its own stub — or vboot needs a raw-Image path.
That is the next protocol decision, not this one.

## D-031 — stam milestone 1: the kernel finds its own hardware

6 591 bytes. It boots under QEMU, reads the device tree it was handed, and
reports the machine: model, console type and address, CPU count, memory base
and size, initrd, command line.

Nothing in it is hardcoded. The console driver finds its UART through
`/chosen/stdout-path`, falling back to searching for a `arm,pl011` or
`ns16550a` node, and honours `reg-shift` and `reg-io-width` so a board that
spaces its registers differently still works. Two drivers rather than one on
purpose: with a single driver it is possible to pretend the register layout is
universal, and every board that disagrees then costs a rewrite.

Proved by varying the machine rather than by reading the code: 1, 2, 4 and 8
CPUs; 512 MiB, 1 GiB and 4 GiB of memory; a command line and an initrd passed
through. Every number followed.

For scale: the Linux kernel this replaces is 36.3 million lines, of which we
compile 6.8 million and ship 1 298 modules, producing a 46 MB image — to run on
one machine.

## D-032 — stam milestone 2: vectors before virtual memory

The kernel was renamed from the placeholder `vkern` to **stam** — the trunk
everything else branches from.

Two things landed, and the order is the decision. **Exception vectors were
installed before the MMU was enabled.** A mistake in a page table is a fault,
and a fault with no vector table is a machine that resets or hangs with nothing
on the console — the single most expensive failure mode to debug, because it
destroys the evidence. Installing the handler first means the first thing
virtual memory can do wrong is *reported*.

The handler earns its place by being readable rather than by being clever: it
decodes ESR_EL1's exception class into a sentence ("data abort (bad load or
store)"), decodes the fault status into a reason ("translation fault (nothing
mapped)"), and prints the address that was touched, the instruction that
touched it, and the first registers. It is exercised on demand with
`stam.fault` on the command line, because a handler that has never run is one
you find out about at the worst possible moment.

The mapping is deliberately coarse — 1 GiB blocks, one 4 KiB table, no second
level. Finer mapping is only useful when there is something to protect from
something else, and there is not yet. What matters at this stage is that memory
gets the right *attributes*: normal write-back for RAM so the caches can be
turned on, Device-nGnRnE for everything else so a store to a UART register is
not sitting in a cache line while we wait for the character to appear.

Two things were learned by running it rather than by reading it:

- The first self-test touched `0x7000000000` expecting nothing to be there, and
  got an *external abort* instead of a translation fault — because that address
  **is** mapped, as device memory, by a table that covers 512 GiB. The test now
  uses an address above the 39-bit space, which is the shape of a real kernel
  bug.
- `exceptions.S` and `exceptions.c` both build `exceptions.o`. make picks one,
  and the link then fails with an undefined symbol that is plainly defined in a
  file you are looking at. The assembly is now `vectors.S`.

## D-033 — stam: interrupts, and a summary that told the truth

The interrupt controller and the timer complete milestone 2. Both are found in
the device tree, including the part most often hardcoded: **which interrupt
number the timer is on**. It is 30 on nearly every ARM machine, which is
precisely what makes assuming it dangerous — on a machine where it is not, the
kernel boots normally and then never ticks, with nothing to say why. stam reads
the `arm,armv8-timer` node's `interrupts` property, takes the non-secure
physical triple, and adds 16 because PPI numbering in a device tree is
relative.

GICv3 is detected and refused rather than half-supported. A controller
configured wrongly delivers no interrupts, which is indistinguishable from
having no timer, and that ambiguity is expensive.

Which produced the finding worth recording. Running on a GICv3 machine, stam
printed the refusal correctly — and then finished with *"milestone 2: hardware
found, faults reported, memory mapped, time running"*. Time was not running.
The timer had never started. The summary was a fixed string, so it said the
same thing whether or not the work it described had happened.

That is the same defect as the boot menu entry that named a flag nothing
implemented (D-028), as the four vpk refusals that exited zero (D-029), and as
the twelve tool bugs in D-021: **a report that does not depend on the outcome it
reports.** Three of them in one day, in three unrelated pieces of code, found
three different ways — by selecting a menu entry, by reading for it
deliberately, and by running on a machine we do not support.

It suggests the question is worth asking routinely rather than occasionally:
*what does this print when the thing it describes did not happen?*

## D-034 — The same kernel, two ways in

stam now boots both ways: as a raw arm64 Image that QEMU's `-kernel` or any ARM
bootloader can start, and as a UEFI application that firmware — or vboot —
loads. One set of sources, two link outputs, two handover shims, and nothing
above `bootinfo.h` knows which happened. That was the promise of D-030, and it
held: adding UEFI support was a new file, not a change to the kernel.

The PE recipe is vboot's, unchanged, because it was already proved against
AAVMF: plain gcc with `-fshort-wchar -fpic`, `ld -shared -Bsymbolic`, objcopy to
`efi-app-aarch64`, and `pe-fixup.py` to clear IMAGE_FILE_RELOCS_STRIPPED. Both
of the flags that are easy to omit produce failures that do not look like their
cause — without `-fshort-wchar` every firmware message is garbage, and without
`-Bsymbolic` the link fails on our own assembly symbol.

Three things were learned by running it.

**The firmware had no device tree.** AAVMF describes the machine with ACPI, and
`EFI_DTB_GUID` is simply absent from the configuration table. stam refused, with
a message naming exactly what was missing — which is the same state x86_64 will
be in permanently, and precisely the gap vboot exists to fill by synthesising a
tree and installing it under that GUID. The design was validated by failing in
the way it predicted. (`-machine virt,acpi=off` makes the firmware publish a
tree, and then it boots.)

**Switching TTBR0 while the MMU is on is not allowed**, and firmware hands over
a machine with the MMU already enabled. So the MMU has to go off, be
reconfigured, and come back on — and the entire sequence must be one assembly
block with no memory access in it. Between switching the data cache off and
back on, any load or store, *including a function call's use of the stack*,
bypasses a cache that still holds the newest copy and reads something stale.
The page table also has to be cleaned out to memory first, because the walker
reads memory and not our cache. This presented as a kernel that stopped after
"vectors installed" with no fault at all.

**vboot passed a command line and stam reported none.** A device-tree loader
writes it into `/chosen/bootargs`; UEFI passes it as LoadOptions on the loaded
image, in UTF-16. The string was there the whole time, in a place nothing was
reading. The shim now converts it and it overrides the tree, because a loader
that took the trouble to pass one meant it.

## D-035 — vboot builds the device tree the firmware would not

The asymmetry: aarch64 firmware usually publishes a device tree, and x86_64
firmware never does — it describes the machine with ACPI. Left alone, that
gives the kernel two ways to learn about hardware and every driver gets written
twice.

vboot now builds a tree and installs it under `EFI_DTB_GUID`, the same GUID
firmware would have used. The kernel looks in exactly one place on every
machine and cannot tell which happened.

**Proved on aarch64 rather than argued about.** Running AAVMF with ACPI enabled
puts us in precisely the x86 situation: no tree in the configuration table. On
that machine vboot distilled the UEFI memory map into a `/memory` node, read
ACPI's SPCR table to find the console, emitted a matching `serial@9000000` node
with `compatible = "arm,pl011"`, and installed the result. stam then found its
console through a tree that did not exist until the loader made one. The same
code path is what x86_64 will use, so it is tested before the architecture it
was written for exists.

The tree is deliberately thin: what cannot be discovered without firmware help
— where memory is, where the console is, where ACPI's root pointer is — and
nothing more. Anything a driver can probe for itself is the driver's business.

`/memory` is one coarse range on purpose. The UEFI map is only valid until
ExitBootServices, which the kernel calls *after* vboot is gone, so a detailed
map baked in here would describe a machine that no longer exists. The kernel is
meant to refine it from the map it takes itself.

Three things this does not do yet, each reported honestly by the kernel rather
than papered over:

- **No CPU or interrupt-controller nodes.** Those live in ACPI's MADT, which
  vboot does not read yet, so stam says `cpus 0` and "no interrupt controller
  we support, so nothing ticks" — which is true.
- **Port-I/O consoles are refused.** SPCR can describe a console in I/O space,
  which is what x86 usually has, and a device-tree `reg` cannot express it.
  Emitting one anyway would hand the kernel an address it would write to. x86
  will need its console described another way — the UEFI framebuffer, through
  the standard `simple-framebuffer` binding, is the obvious candidate.
- **The page allocator wastes almost all memory under UEFI.** It starts at the
  end of the kernel image, which firmware places high, so it reported 58 MiB
  free on a 1 GiB machine. Correct, and nearly useless; the fix is to allocate
  from the UEFI map's free regions rather than from one bump pointer.

## D-036 — The synthetic tree, finished

vboot now reads three ACPI tables and turns each into the device-tree node the
kernel expects:

- **SPCR** → the console: a `serial@…` node with `arm,pl011` or `ns16550a`.
- **MADT** → the interrupt controller and the processors: an `intc@…` node with
  the distributor and CPU-interface ranges, and one `cpu@N` per enabled entry.
  ACPI has no separate processor list on ARM; counting GIC CPU interfaces *is*
  how you count CPUs.
- **GTDT** → the timer: `arm,armv8-timer` with its interrupts.

That last one carried the trap. ACPI gives the timer as a *global* interrupt
id; a device tree wants it relative to the start of the per-processor range,
which begins at 16. Subtract wrongly and the kernel boots normally and never
ticks, with nothing to say why. It is the same number, expressed in two
conventions, and only one of them is written down in either specification.

A GICv3 is deliberately **not** described even though MADT reports it. stam
supports v2 only, and a v3 node it would then misconfigure is worse than no
node at all: the machine would look like it was working while taking no
interrupts. Refusing produces the message we already print.

**The allocator now uses the firmware's account of free memory.** It used to
start after the kernel image, which is right when the kernel is handed all of
RAM and badly wrong under UEFI, which loads the image high — it found 58 MiB
free on a machine with a gigabyte. Taking the largest EfiConventionalMemory
region gives 836 MiB. `pmm_init` distinguishes the two cases by asking whether
the kernel image actually lies inside the region it was given, rather than by
being told which situation it is in.

**A round-trip test now holds the two halves together.** vboot's tree writer
(`fdtgen.h`) and stam's tree reader (`pkg/stam/fdt.c`) were written
independently and never met except during a boot, where a disagreement about
byte order or a block offset would show up only as a kernel that works on some
machines. `make test` now builds a tree with one and reads it back with the
other, checking cells, strings, paths, `compatible` lookup and two `reg`
ranges. 140 → 141 checks.

## D-037 — stam milestone 3: threads the timer takes the CPU from

Round robin, one queue, no priorities. That is not a placeholder for something
cleverer so much as the honest shape of the problem: every thread here is kernel
code we wrote, and there is nothing yet to be unfair to. Priorities become
meaningful when threads belong to different people.

What matters is that the switch happens *from the timer interrupt*, without the
running thread agreeing. The demonstration is three threads in tight counting
loops that never yield, never sleep and never call into the kernel. After two
seconds each had been scheduled 51 times — exactly evenly — and each had counted
to roughly three hundred million. A cooperative scheduler would show one counter
rising and two at zero.

Two ordering details that would each have produced a machine that schedules once
and then stops:

- **EOI before the switch.** Telling the interrupt controller we are finished
  has to happen before changing threads, or the interrupt stays active while
  another thread runs and that thread can never be interrupted in turn.
- **The trampoline unmasks interrupts.** A thread is first resumed from inside
  the timer interrupt, where the hardware masked them on entry. Without
  `msr daifclr, #2` the first thread would run with interrupts masked forever.

### The bug worth the whole entry

The scheduler worked immediately on the raw-Image path and faulted on the UEFI
path, with an instruction abort at `0x193c` — an address suspiciously close to
zero.

`0x193c` was `thread_trampoline`'s **link-time** address. This kernel is built
position-independent and **nothing applies relocations to it**, so anything that
ends up in a GOT entry or a statically initialised pointer holds the address the
image had when linked, not where UEFI actually loaded it.

Three separate things fell out of that one cause, and each needed its own fix:

1. `static const char *const kind_name[16]` — a table of string pointers is data
   full of link-time addresses. It is now a `switch`, which materialises each
   address PC-relative. This is why the exception report printed an *empty
   description of itself*, which was the first clue.
2. `-fvisibility=hidden` for the C sources, `.hidden` for the assembly symbols
   and `HIDDEN()` for the linker-script symbol, so referencing them does not go
   through the GOT at all.
3. Even hidden and local, taking `&thread_trampoline` from C still produced a
   GOT entry. Rather than keep negotiating with the compiler, the address is now
   computed in assembly with `adrp`/`add` by a two-instruction function. It
   cannot be wrong.

`make test` now asserts the invariant directly: **stam.efi must need zero
relocations.** That check is what keeps the other three fixes honest, because
any future one of them silently regressing looks exactly like this did — a
kernel that boots one way and not the other.

## D-038 — A test that was only sometimes right

Running `make test` three times in a row gave 141, 142 and 140 passes. One
check was flaky: *"vcpio strips a leading / instead of writing outside"*.

It compared modification times with `find -newer`, and the archive and the
extracted file land in the same second often enough that the answer flipped at
random. Nothing about vcpio was wrong.

The rewrite measures the thing that actually matters instead of a proxy for it.
The file is archived by its absolute path, then its contents are replaced with
a sentinel. If vcpio honours the leading `/` it overwrites the original and the
sentinel is gone; if it strips it, the sentinel survives and a copy appears
inside the extraction directory. No timestamps involved. Eight consecutive runs
now give 142 and 0.

Worth saying plainly, because it is the same lesson as D-028, D-029 and D-033
seen from the other side: **a check that is only sometimes right is worse than
no check.** A test that always fails gets fixed. A test that fails one run in
three teaches everyone to re-run it, and after that a real regression looks
exactly like the noise they have learned to dismiss.

It was also found the same way as everything else this session — by running the
thing repeatedly rather than once, and noticing the number moved.

## D-039 — stam milestone 4: something ran that was not the kernel

A program now runs at EL0, asks the kernel for things through `svc`, and is
killed rather than taking the machine with it when it oversteps.

The program is assembly carried inside the kernel image as bytes. An ELF loader
is a separate problem from reaching EL0 and returning, and doing both at once
means not knowing which one is broken. It uses Linux's aarch64 system-call
numbers — 64 write, 93 exit, 172 getpid — because the numbering is the cheapest
part of an ABI to agree with, and choosing our own is a decision we could only
undo. Three calls: one with an effect, one that returns a value, one that does
not return at all.

**One address space, not two.** The kernel is mapped AP=00 — EL1 may touch it,
EL0 may not — and user pages AP=01. The hardware enforces it on every access. A
split TTBR0/TTBR1 layout is the eventual answer, because it lets processes share
the kernel's half of the map instead of copying it, but it is not what makes the
protection work, and doing it now would change the address of every kernel
symbol at the same moment as introducing userspace.

The claim was proved rather than asserted: a second program deliberately reads
`0x40000000`, gets a permission fault, and is killed while the kernel carries
on. If that load ever succeeds, nothing built above it can be trusted.

### The vector table had been wrong since milestone 2

The first system call arrived at the handler for an SError. Not a mistake in the
handler — a mistake in the *table*. Each of the sixteen entries is exactly 128
bytes, and an entry holding a full save-and-restore sequence runs to about 160.
Every entry after the first long one had been pushed 128 bytes down the table
since interrupts were added. Interrupts still worked, because the IRQ entry sits
*before* the overflow. Entry 8 was the first displaced entry anything used.

Each slot now holds four instructions and a branch, and `.org` directives make
the size a build error: the assembler will not move its location counter
backwards, so an entry that outgrows its slot cannot be assembled.

### Never take the address of a symbol from C

This kernel is position-independent and nothing applies relocations to it, so
anything the compiler routes through the GOT holds the address the image had
when it was *linked*. Hidden visibility is not enough — the compiler still
emitted a GOT entry for a hidden, local symbol.

It has now been found twice. First as a thread beginning execution at `0x193c`
(D-037). Then, immediately after, as the first user program being copied out of
empty space and faulting: `extern const unsigned char user_prog_start[]` has the
same problem. Both are now reached through two-instruction assembly accessors
that compute the address with `adrp`/`add`.

The second instance was caught by the test the first one prompted — `stam.efi`
must need zero relocations — which is the argument for writing the check at the
time rather than only fixing the bug.

## D-040 — ELF, and why not a format of our own

The question came up directly: now that stam runs programs, should VerkOS define
its own executable format?

**No — for the same reason Multiboot2 was rejected in D-030.** ELF is not a
component we can replace, it is an *interface*. GCC and binutils produce ELF, so
anything else we invent is a *conversion from* ELF, which means something still
has to read ELF — on the build host instead of in the kernel. And we would lose
`readelf`, `objdump`, `nm`, `gdb` and every profiler, all of which understand our
binaries today. The loader for the subset that matters is about eighty lines:
check the machine, walk the program headers, place each PT_LOAD. Sections,
symbols and relocations are for the linker, not the loader.

The consistent principle, now stated twice: **adopt the interface everyone
speaks; own the implementation behind it.** We write our own tar, cpio, ISO and
package tools, but they all speak formats other software understands.

There is one thing a format of our own could add that ELF does not have, and it
is not ownership: **a signature**. vboot already refuses to boot a kernel that
does not verify against our Ed25519 key. A thin container around an ELF —
signature, length, key id — would let the kernel refuse to *run* a program on
the same grounds. That is the coherent form of the "antivirus" idea: not
searching for known-bad code, but declining to execute anything that is not
ours. Recorded here as the open question it is; not built.

The loader is fed from the **initrd**, whose address comes from the device tree,
because that is how a kernel starts its first program before it has a
filesystem. `progs/hello.c` is compiled by the ordinary compiler, static and
freestanding, and runs at EL0 with four system calls and no libc. An unknown
call number returns `-ENOSYS` rather than killing anything, which is the
behaviour a program can actually cope with.

Known gap: this works on the raw-Image path, where QEMU writes the initrd's
location into `/chosen`. On the UEFI path vboot publishes the initrd through
LoadFile2 for an EFI stub to collect, and stam's stub does not collect it yet,
so there is nothing to run. The kernel says so rather than appearing to have
nothing to do.

## D-041 — The initrd arrives two different ways

A device-tree loader writes the initrd's address into `/chosen`. UEFI has no
such convention, so Linux invented one and everyone now uses it: the loader
installs a LoadFile2 protocol on a device path carrying a known vendor GUID,
and whoever wants the file goes and asks for it.

vboot had been publishing it that way all along. stam's EFI shim was not
collecting it, so a kernel started by our own bootloader had a program available
and did not run it. It now asks, and the whole chain runs: firmware → vboot →
stam → a program the compiler built.

This is the third thing that arrives differently depending on who started us,
after the command line and the memory map, and all three are handled the same
way — the shim learns it, `bootinfo.h` carries it, the kernel does not know
which happened.

The boot output was also reorganised into four numbered questions, each with its
answer, because three demonstrations running in sequence with no separation read
as noise. A log nobody can follow is a log nobody reads.

## D-042 — The kernel moved to the high half

TTBR1 now holds the kernel's map and TTBR0 a process's own. While the kernel
lived in the low half it shared TTBR0 with whatever was running, so every
process would have needed a copy of the kernel's mappings and a process switch
would have meant switching the kernel's map too. Now the halves are independent:
the kernel is mapped once, for everyone, and switching process touches only
TTBR0.

This was possible only because the kernel is built position-independent and
needs no relocations — the same instructions run correctly at `0x40080000` and
at `0xffffff8040080000`. Nothing had to be rewritten, only re-pointed.

The move itself is one function that does not return. It cannot: the stack it
was called on is full of return addresses in the low half, and the moment TTBR0
stops describing the low half those become faults. So `go_high` moves the stack
pointer to the same memory seen through its high alias, abandons the old frames,
and branches to `kmain_high`, which starts a fresh call chain.

Dropping the low half immediately afterwards is deliberate. Any pointer still
holding a physical address then faults at once and loudly, rather than working
by accident until something moves.

An unexpected gain: the isolation got *stronger*. A program reaching for
`0x40000000` used to take a permission fault, because the kernel was mapped in
its address space but marked EL1-only. It now takes a translation fault —
nothing is mapped there at all. The kernel is not merely unreadable to a
program; it is absent.

### Three things that had to be re-pointed, and one that was missed

Everything the kernel learned *before* the move holds physical addresses: the
UART's registers, the device tree, the GIC's two banks, the initrd. Each is
rebased once, in `kmain_high`.

The one that was missed was `VBAR_EL1`. The vector table had been installed at
its low address, so the moment the low half went away the first interrupt was a
fault whose handler was also unreachable — a hang with no output whatsoever.
`exceptions_install` computes the address PC-relative, so calling it again from
the high side installs the right one.

### The two builds had different code generation, and it hid a bug

After the move the raw-Image build faulted in a loop while *reporting* a fault,
printing an empty description of itself; the UEFI build was fine. The two
Makefile variable lists had drifted apart, and only the UEFI one had `-fpic`.
Without it the compiler builds tables of absolute pointers — a `switch`
returning string constants becomes exactly that — holding link-time low
addresses that stop resolving the instant the kernel moves.

`-fno-jump-tables` was not enough; that only governs branch tables, not tables
of returned constants. Both builds now share a single `COMMON` list, and
anything affecting how addresses are formed belongs in it. Two builds of one
kernel differing in code generation is a bug generator: it means a defect can
be present on one path and invisible on the other, which is exactly how this one
survived until the address space changed underneath it.

## D-043 — Two programs at once, each with its own memory

Every thread now carries its own low half. A process switch swaps TTBR0 and
nothing else — the kernel's half stays where it is, mapped once, for everyone.
That is what moving to the high half (D-042) bought.

The demonstration is one binary run twice. Both copies have their code at
`0x3f0000`, their data at `0x41ffe8` and their entry point at `0x4000d0` —
identical addresses — and each holds a different value in the same variable at
the same address. They interleave, because the timer takes the CPU from one and
gives it to the other mid-run.

The proof is inside the program rather than in the log. It records its own
process id in a global, spins long enough to be preempted several times, and
then checks the global still matches. If the two shared an address space the
second would have overwritten the first's copy and the check would fail. Both
exit zero.

An assertion in a log is something the reader has to believe. A program that
checks its own memory and exits non-zero if it was disturbed is something the
machine believes.

## D-044 — GICv3, and the end of "QEMU only"

stam supported GICv2 and said so honestly when it met anything else (D-033).
That was the right answer while nothing depended on it, but nearly every ARM
machine built in the last decade is v3 — including the board on the desk — so
"v2 only" meant "QEMU only".

They are not variations on a theme. In v3 the CPU interface **stopped being
memory-mapped** and became system registers, and the per-CPU interrupts moved
out of the distributor into a redistributor of their own, one per core. Three
things must all happen or the machine takes no interrupts and explains nothing:
wake this core's redistributor and wait for it to say its children are awake;
set `ICC_SRE_EL1` and *read it back*, because on some implementations the bit is
write-once; and enable group 1 in both the distributor and the interface.

The trap is `gic_enable_irq`. In v2 a per-CPU interrupt is enabled in the
distributor. In v3 those registers are reserved and the real ones are in the
redistributor's second 64 KiB frame. Configuring the timer the v2 way on a v3
machine writes to reserved registers, returns no error, and the timer simply
never fires.

vboot now describes a v3 too, which it deliberately refused to do before: it
reads the redistributor's address from MADT's type 0x0E entry and emits
`arm,gic-v3` with distributor and redistributor ranges instead of distributor
and CPU interface. The refusal was correct when stam could not drive one — a
node the kernel would misconfigure is worse than no node — and became wrong the
moment it could.

Verified across the whole matrix, five configurations, each answering all five
of the kernel's own questions:

| | |
|---|---|
| raw Image, GICv2 | 5 of 5 |
| raw Image, GICv3 | 5 of 5 |
| UEFI + ACPI, GICv2 | 5 of 5 |
| UEFI + ACPI, GICv3 | 5 of 5 |
| UEFI + device tree | 5 of 5 |

What remains before stam runs on the Arduino UNO Q is a Qualcomm GENI console
driver and an Android boot image; the interrupt controller is no longer in the
way.

## D-045 — Two pieces written for hardware we have not run them on

Everything else in this project has been verified by running it. These two
cannot be, yet, and it is worth saying so plainly rather than letting them sit
alongside the tested work as if they were equal.

**A GENI console driver.** Qualcomm's serial port is neither a PL011 nor an
8250: it is a programmable serial engine told to behave like a UART, so every
character is a transfer that must be announced, waited for, and acknowledged in
sequence. Skipping an acknowledgement leaves the engine busy and the next
character never leaves. It is written from that sequence; no Qualcomm hardware
has executed it.

Two things were done because it is untested. It is a *third* driver, which is
what keeps the console code honest — with one driver it is easy to believe the
register layout is universal. And every wait loop in the console, including the
two that do work, is now bounded. An untested driver that spins forever is a
hang with no output at all, which is the most expensive failure this kernel can
produce; giving up prints nothing but lets the machine carry on, so whatever
goes wrong next is still visible.

**vbootimg**, which writes and reads the Android boot image format. Qualcomm
boards do not boot from an EFI system partition — the chain ends at ABL, which
takes a header followed by a page-aligned kernel and ramdisk. It is the only way
onto such a board.

`show` exists so the writer can be checked against a reader that does not share
its assumptions, which is the only self-check available for a format we cannot
yet hand to the hardware that consumes it. `make test` round-trips sizes and the
command line, checks the magic, and confirms that a truncated file and a file
that is not an image are both refused. 142 → 150 checks.

The tool prints, every time it runs, that `fastboot boot` loads an image without
writing anything and `fastboot flash boot` overwrites the board. That belongs in
the output rather than in a document, because the moment someone needs to know
it is the moment they are typing the command.

What remains before stam runs on an Arduino UNO Q is now only the parts that
require the board itself.

## D-046 — fork, and why exec is not here yet

`fork` is the one system call whose implementation is about the shape of the
stack it was called on rather than about its arguments. The child has to wake up
*inside* the parent's system call and finish it with a different answer.

So the child's kernel stack is built to look exactly like the parent's does at
that moment — a byte copy of the exception frame, with `x0` set to 0 instead of
the child's id — and its saved context is arranged so that the first thing it
does when scheduled is `ret` into a routine that restores that frame and returns
to EL0. One instruction, two processes, two answers.

The address space is copied outright rather than shared copy-on-write. Copy-on-
write is the thing to do eventually and the wrong thing to do first: it needs
write faults, reference counts per page, and a fault handler that can tell "not
allowed" from "not yet". Being able to *see* fork work is worth more right now
than seeing it work quickly.

The proof is again inside the program. Both processes have the same variable at
the same address; the child sets it to 222 and the parent checks that it still
reads 100, exiting non-zero if not. Parent 5 reports 100, child 6 reports 222.

**`exec` is deliberately absent.** The mechanism is easy — replace the address
space, load an ELF, jump, do not return. What is missing is anything to point
at: with no filesystem there is exactly one program, the initrd, so `fork` then
`exec` would start the same program again, forever. The honest next step is to
make the initrd a cpio archive — vcpio already writes them — so it can hold
several programs with names. Writing `exec` before there is something to exec
would be writing a demonstration rather than a feature.

## D-047 — exec, once there was something to exec

The initrd is now a newc cpio archive — what every Linux initramfs is, and what
VerkOS' own `vcpio` already writes. Same principle as ELF and the device tree:
adopt the format everyone speaks, own the implementation behind it. It is not a
filesystem; there is no directory structure, nothing can be written, and finding
a file is a walk from the beginning. That is all an initramfs has ever been.

`execve` keeps the process and replaces everything in it. The interesting part
is that it does not jump anywhere: it edits the exception frame the system call
is about to return through — the address to resume at, and the stack — so the
ordinary return to EL0 lands in the new program instead of after the `svc` in
the old one. The process id, the kernel stack and the place in the scheduler all
stay as they were, which is exactly what makes it the same process running
something else.

`progs/init.c` does what every Unix has done since: fork, and let the child
become something else. Process 5 forks 6, 6 execs `/hello`, `/hello` runs and
exits, and 5 is still 5 afterwards.

### The bug that only two user processes could reveal

**SP_EL0 was not being saved.** It is a single register shared by every process,
and the exception frame saved x0–x30, ELR and SPSR but not the user stack
pointer. With one user process at a time that is invisible. The moment `exec`
set a stack for the child, the parent's was gone — and when the parent resumed
it faulted just above its own stack, which reads like memory corruption and is
nothing of the kind.

It is saved and restored with everything else now. There was already room in the
frame; the omission was not a size problem but a failure to notice that a
register outside the general-purpose set is still per-process state.

A second, smaller one from the same batch: the exec message printed the path
*after* installing the new address space, where the string no longer exists, and
duly printed whatever now lived at that address. The name is copied into the
kernel before anything changes.

Both are the same shape: state that belongs to a process, read or held at a
moment when the process it belongs to is no longer the one installed.

### Not done, and named rather than hidden

There is no `wait()`, so `init` spins instead of blocking — the next system call
to write. `exec` does not reclaim the old address space; nothing frees pages
yet, which is a real leak. And `argv` is ignored.

## D-048 — wait(), and giving memory back

Two of the three gaps named in D-047 are closed.

**Pages can be returned.** The allocator could only ever hand memory out, which
was fine while nothing ever finished. Freed pages now go on a list threaded
through themselves — the first eight bytes of a free page hold the address of
the next — which costs nothing and needs no separate bookkeeping, and that
matters when the bookkeeping would itself require pages. A process that exits
gives back every page of its address space, tables included: 50 pages from one
`fork`/`exec`/`exit`.

Only the low half is walked. The kernel's mappings are in TTBR1 and are not a
process's to release. Nothing is shared, because `vm_copy_space` copies outright
— which will stop being true the day copy-on-write arrives, and `vm_free_space`
is the function that will have to learn about reference counts.

**A finished process is a zombie until someone looks at it.** It keeps its exit
status, because a parent may ask, and its kernel stack, because it is standing
on it. Both are released by whoever collects it. Nobody frees themselves.

`wait4` blocks rather than spins, and the difference is visible from outside: a
spinning parent burns every slice it is given, a blocked one is not scheduled at
all until its child is gone. Waiting with no children returns `-ECHILD` instead
of blocking forever — a distinction worth the extra line, because a program that
waits for a child it never had should be told so.

Still open, and still named rather than hidden: `argv` is ignored, and there is
no way to read from the console, which is what a shell would need.

## D-049 — A shell, which is the point of all of it

`vsh` prints a prompt, reads a line, and if the line names a program, forks,
lets the child become it, and waits. That is what every shell has done since the
sixth edition, and everything it needs from stam already existed: read, write,
fork, execve, wait4, exit.

**That is the evidence, not the shell itself.** A shell is not a feature of a
kernel; it is a program like any other, and being able to write it as one —
with no privileges, no special calls, nothing the kernel knows about it — is
what shows the interface underneath is real rather than a demonstration.

Reading required the console to work in the other direction, and a line
discipline: characters echoed as they arrive, backspace removing one, the line
delivered on Return. That belongs in the kernel because it is what a terminal
driver is for. A program should not have to know that undoing a backspace on
screen takes three bytes.

**`read` waits by yielding**, because nothing wakes a thread on input. A UART
interrupt would let a reader genuinely sleep instead of being scheduled only to
find nothing there; until it exists, a program waiting for input costs a time
slice each time round. That is written in the code where the loop is, rather
than left for someone to discover.

One thing came out of using it rather than building it: the kernel's running
commentary — every segment loaded, every exec — sat between the prompt and the
answer. It is off as soon as anything reads from the console, on the reasoning
that somebody reading is somebody sitting there. It stays available, because a
program that will not load is exactly when the loader's reasoning is wanted.

A shell running a program that itself forks, on a kernel that is a day old:

    verk$ forky
      [parent] I am process 7 and my variable says 100
      [child]  I am process 8 and my variable says 222

Still missing, and named: argv, pipes, redirection, and any filesystem that is
not the initrd.

## D-050 — The console interrupts, and the machine sleeps

`read` used to wait by yielding: the thread stayed runnable, was scheduled,
found nothing, and gave way again — burning a slice each time round. It worked,
and it is the difference between a machine that is idle and one that is merely
not doing anything useful.

The console now raises an interrupt, found in the device tree like everything
else: the serial node's `interrupts` property, with the same relative numbering
as the timer's — type 0 is a shared interrupt counted from 32, type 1 a
per-processor one counted from 16. Getting that wrong gives a port that works
perfectly when polled and never interrupts, which is a hard thing to look for.

Characters go into a small ring buffer, and the handler **drains the port
completely** rather than taking one: the interrupt says something arrived, not
how much, and a character left behind may be the reason no further interrupt
comes.

A reader with nothing to read is now properly blocked — not scheduled at all
until the interrupt puts it back in the queue. The idle loops in the kernel
`wfi` between ticks. Sitting at the shell prompt for ten seconds, QEMU consumed
no measurable CPU at all.

### System calls now run with interrupts enabled, and that is a debt

They have to. A call that waits for a device cannot be woken by that device if
the exception entry left interrupts masked; that is a deadlock, not a slow path.

It also means a system call can be preempted, and **this kernel has no
locking**. One CPU and a scheduler simple enough that it holds today, but it is
the next thing that will be wrong — the moment there is a second CPU, or a data
structure that takes more than a moment to update. It is written at the line
that enables them rather than left to be discovered.

GENI is deliberately left polling: its receive path is unwritten, and enabling
an interrupt for a driver that cannot read would produce a storm.

## D-051 — Setuid and argv[0] dispatch are incompatible

**The rule: every setuid binary has exactly one `main()` and can only ever
become one thing. Shared code is shared at the source level, never through
dispatch.**

A multi-call binary lets its caller choose what it does — busybox does this, and
so did `vlogin`. A setuid binary runs as root. Put them together and the
privileged code is selected by a field the caller sets freely: `argv[0]` is
whatever the caller passes to `execve`, and it is not a name the program can
trust about itself.

VerkOS had this shape. `su` and `passwd` were copies of the multi-call `vlogin`
with mode 4755, and `main` chose between eleven tools by `argv[0]` — falling
back to `argv[1]` when the name matched nothing. Both inputs belong to whoever
starts the program. It was found by reading for the shape rather than by
anything failing, and fixed before VerkOS had run anywhere but here.

Now: `vlogin_core.c` is a library. `vlogin` is the multi-call tool and is **not**
setuid — dispatching on `argv[0]` is fine when the caller gains nothing. `su.c`
and `passwd.c` are separate programs with a `main()` each that calls one
function and reads no name at all.

Three things came with it.

**Startup hygiene.** `vlogin_secure_startup()` runs first in both setuid
programs. It attaches `/dev/null` to any of file descriptors 0, 1 and 2 that is
missing, because a caller may close them: the program's first `open()` then
*becomes* descriptor 0, its second becomes 1, its third becomes 2 — so a file it
opened for its own purposes is now its standard error, and the next message it
prints lands inside that file. On a program that opens `/etc/shadow` this is
worth more than the two lines it costs. It also clears `LD_PRELOAD`,
`LD_LIBRARY_PATH`, `LD_AUDIT`, `IFS`, `BASH_ENV` and `ENV`, which nothing here
reads but anything it executes might.

**Hardening, which was entirely absent.** The account tools were built with
plain `-O2`. They are now built `-D_FORTIFY_SOURCE=3 -fstack-protector-strong
-fPIE` and linked `-pie -Wl,-z,relro,-z,now,-z,noexecstack`, and the properties
are verified in the binary rather than assumed from the flags.

**An inventory nobody had taken.** The rootfs held four setuid binaries beyond
ours — `mount`, `umount`, `ssh-keysign` and `dbus-daemon-launch-helper` — each
installed by its package for a feature VerkOS does not use. The build now
removes the bit from everything except `su` and `passwd` and fails if any
remains. `make test` checks the same set, that the bit is set in exactly one
place in the build, and that neither setuid program contains a string
comparison, because a program that compares strings to decide what to do is
dispatching whatever it calls the variable.

This is the same family as the rest of this log: a defect visible in the shape
of the code and in no output at all. Nothing failed, nothing printed a warning,
and every test passed — because the tests asked whether the tools worked, and
they did.
