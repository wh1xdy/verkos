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

## D-052 — The ESP was FAT16 the whole time

**Every document in this repository said the EFI system partition was FAT32.
Every one that VerkOS has ever built was FAT16.**

`mkfs.vfat -n VERKESP` was called without `-F`, so dosfstools chose the type
from the cluster count, and at both sizes VerkOS builds — 48 MiB for the ISO's
embedded image, 160 MiB for the boot disk — it chose FAT16. Measured, not
reasoned about: `file` reports `FAT (16 bit)` on both. `-F 32` is accepted at
both sizes and produces `FAT (32 bit)`, so the fix costs three characters.

UEFI requires FAT32 for an ESP on fixed media and permits FAT12/16 only on
removable media. **AAVMF and OVMF accept FAT16 anyway**, which is the entire
reason this has always booted. That is D-023's shape exactly — two things vboot
got away with only because it had met one filesystem — except that this time it
was the firmware we had only met one of, and the untried real hardware in
STATUS.md is precisely where it would have surfaced.

What made it invisible is worth naming, because it is the log's recurring
defect wearing new clothes. `mkfs.vfat` succeeded. The firmware booted. Every
test passed. The comment immediately above the call said *"FAT32 needs about
33 MB to exist at all"* and the code sized the image to 48 MiB **for that
reason** — so the intent was recorded, acted on, and never checked against what
came out. A statement of intent next to a call is not a test of the call.

`make test` now asserts both halves: that each stage script passes `-F 32`, and
— where `mkfs.vfat` exists — that an image built that way actually comes out
FAT32. The structural grep says what we asked for; the second says what we got,
and only the second would have caught this. 161 → 165 checks.

## D-053 — The kernel stopped booting, and nothing said so

stam did not boot. Not a fault, not a panic, not a truncated log — **no output
at all**, which D-045 already names as the most expensive failure this kernel
can produce, and which was reached here by the shortest possible route.

It was found by accident, while trying to run the kernel to check an unrelated
change. The change was innocent; the tree at HEAD was already dead. Nothing had
reported it because **`make test` builds stam and never boots it**: it checks
the Image magic, the vector ordering, the relocation count and the PE header,
all of which were fine.

`-d in_asm` gave the whole answer in one line. Execution reached `fdt_init`,
then branched to `0x200` and hit an undefined instruction — `0x200` being the
synchronous-exception offset from a `VBAR_EL1` of zero, i.e. a fault taken
before the vector table exists. The instruction before it:

    40082 3c0:  f801c003   stur  x3, [x0, #28]

`fdt_t` has `addr_cells` at offset 0x1c and `size_cells` at 0x20. `fdt_init`
sets them to 2 and 1, and GCC 12 merged the two 32-bit stores into one 8-byte
store at an offset that is 4-aligned and not 8. **The kernel runs with the MMU
off until it has read the device tree, and before the MMU everything is Device
memory, where an unaligned access is an alignment fault rather than a slower
load.**

So the fix is `-mstrict-align`, in `COMMON` where both builds get it, which is
the same rule D-042 arrived at from the other direction: anything affecting how
the compiler forms accesses belongs to both paths or to neither.

**What makes this worth an entry is that nothing in the source changed.** The
code was correct and stayed correct; a different compiler decided to merge two
stores, and a kernel that had booted for a week stopped. Any build flag that
only matters before the MMU is on has this property — it is invisible until the
toolchain changes its mind, and then it is invisible in the worst way.

`make test` now disassembles the kernel and asserts there is no `stur`/`ldur`
of a 64-bit register at an offset that is not a multiple of 8. That form is
exactly the fingerprint: a 64-bit `str` can only encode an 8-aligned offset, so
an unaligned access *has* to become a `stur`. Verified in both directions —
1 hit with the flag removed, 0 with it. 165 → 166 checks.

The gap it exposes is larger than the fix. **Nothing here boots stam**, so the
next defect of this shape will also be found by a person trying to use it. A
check that starts the kernel under QEMU and looks for its own banner is the
obvious next thing to write.

## D-054 — argv, and a slot that was never given back

Two of the three things D-049 named as missing are closed.

**Arguments.** `execve` takes an argv, and the shell splits its line and passes
one. The kernel copies the strings into its own memory *before* installing the
new address space — the same trap the path fell into in D-047, one address
space further along — then lays them out at the top of the new stack in the
order every aarch64 Linux program is started with: `argc`, the pointers, a
NULL, an empty environment. Nothing invented; a compiler's idea of `main` would
already find them.

It is bounded at 16 arguments and 512 bytes and **refuses rather than
truncating**, with `-E2BIG`. A silently shortened argument list is a program
quietly given different instructions, which is this log's recurring defect in
the one place it would be hardest to see.

A program with no argv still gets `argv[0]`, synthesised from the name it was
exec'd under. `init` passes none, so that path is exercised on every boot.

**The proof is in the program.** `hello` prints its own argument list and exits
non-zero if `argv` is not NULL-terminated, so a wrong answer is a status a
shell can check rather than prose a person has to read:

    verk$ hello one two
      [hello] argc = 3
      [hello] argv[0] = "hello"
      [hello] argv[1] = "one"
      [hello] argv[2] = "two"

`argv[0]` is the name as typed, not the path that was resolved, because that is
what a program printing its own name expects.

### The slot that was never given back

Found while reading the scheduler for this work, not by anything failing.
`sched.h` said a collected slot **may be reused**; `sched.c` handed out slots
with `threads[n_threads]` and only ever incremented. Sixteen processes for the
whole life of a boot, after which `fork` refused — a machine running out of a
resource it was never returning, with a comment describing behaviour the code
did not have.

**Reusing the slot while it still *was* the id would have been worse than the
leak.** `t->id = n_threads` made the two the same number, and `parent` holds an
id: give the same number to a new process and an unrelated program inherits
somebody else's children. So the id became a counter of its own — monotonic,
never reused — and the slot became separately allocatable. `next_runnable` used
the id as an array index and now takes the position from the pointer.

One thing had to be kept that the old code got right by accident. It published
a slot with `n_threads++` *after* building the frame; now a slot is visible the
moment it is taken, and `schedule()` runs from the timer interrupt. So a slot
stays `THREAD_FREE` until its stack and saved registers are complete, and only
the last line of each creator makes it runnable. The window between "this slot
is mine" and "this slot has a stack" is a window in which a thread can be
resumed on a stack pointer of zero.

**Measured both ways**, twenty `hello` runs typed into the shell:

    old scheduler   10 of 20 ran, then "fork: no room for another process"
    new scheduler   20 of 20 ran, ids climbing to 25, no failures

Still missing, and still named: pipes, redirection, any filesystem that is not
the initrd, and no user-pointer validation. And one found on the way: **a burst
of input longer than a few characters is lost** — typed slowly the shell reads
a whole line, but a line delivered all at once arrives as its first three
characters. The ring buffer is 256 bytes and not the cause; the PL011 receive
FIFO is the first place to look.

## D-055 — Something finally boots stam

D-053 ended by naming the gap it had just fallen into: nothing here starts the
kernel, so a kernel that stops booting looks exactly like one that works.
`make test` now boots stam under QEMU with its initrd and reads what it printed.

The six assertions are chosen so that each depends on work having happened
rather than on the kernel having reached a line of code: the scheduler's own
verdict after two seconds of threads refusing to yield, a program's report of
its own arguments, the pages a finished process gave back, the final milestone.
A fixed banner would say the same thing either way, which is D-033's lesson.

**Validated by breaking the kernel on purpose.** With a store to an unmapped
address injected at the top of `kmain_high`, four of the six go red and two stay
green — and the two that stay green are exactly the ones describing work that
completed before the injected fault. That is the check reporting where the
kernel died, not merely that it did.

**It does not catch D-053, and the comment in the test says so.** Whether an
unaligned access with the MMU off actually faults is the emulator's choice:
QEMU 7.2 in the build container runs the offending kernel to completion, QEMU
11.0 on the developer's machine kills it in `fdt_init`. The disassembly check
from D-053 is what catches that one, on every host. This one catches the faults
every QEMU agrees are faults. Writing that down mattered more than the check
did — a test believed to cover something it does not is worse than a gap
everyone knows about.

166 → 172 checks. CI has no qemu-system-aarch64 and skips the six, which is
also worth knowing: the boot is proved where the kernel is built, not on push.

## D-056 — A pointer from a program is a claim, not an address

Every system call that took a pointer dereferenced it. `sys_write` said so in a
comment — *"nothing here validates it yet, which is honest to note and wrong to
leave"* — and the reasoning it gave for waiting had expired: it said checking
needs a notion of which pages belong to which process, and that arrived in
D-043.

**The MMU does not help here.** It stops EL0 reaching the kernel's pages. It has
nothing to say about EL1 touching an address EL0 *handed over*, and at EL1 that
same access is permitted. So `write(1, kernel_address, 32)` printed 32 bytes of
kernel memory to the console, and a read into an unmapped address was a data
abort at EL1 — which is not a program being killed, it is the machine stopping.
Both were demonstrated before the fix, which is how those two sentences are
known rather than assumed.

`vm_user_ok(va, len, write)` walks the caller's own tables — the hardware's
tables, so there is no second opinion that could disagree with the first — and
requires every page of the range to be valid and `AP=01`. A kernel page is
`AP=00` and fails whatever address was claimed. The range must also not wrap and
must lie inside the 512 GiB the low half can describe at all, because a length
of `0xffffffffffffff00` otherwise passes any check that only looks at the start.

Strings are the harder half: their length is not known until they have been
read, and reading is the thing being made safe. `vm_user_strlen` therefore
validates one page, scans to the end of it, and only then looks at the next.

Applied to all of it — `write`, `read`, `wait4`'s status pointer, `execve`'s
path, the `argv` vector, and every string inside it. argv is two levels: the
array may be mapped while the strings it points at are not, so each entry is
checked before it is followed. `wait4` is checked twice, before blocking and
before the store, because the answer to "is this address the caller's" is only
true at the moment it is used.

**The proof is a program that lies.** `progs/badptr.c` hands over eight
addresses that are not its own and exits non-zero if any is not refused with
`-EFAULT`. `init` runs it on every boot and `make test` reads its verdict rather
than our reading of a log. With `vm_user_ok` stubbed to return 1, the first case
prints kernel memory and the second kills the kernel — so the test discriminates
rather than merely passing.

### Userspace must not use floating point, and the compiler does not know

Found by `badptr` dying at EC 0x7 — trapped FP/SIMD — on a line that contains no
arithmetic. stam never enables FP for EL0 (`CPACR_EL1` is untouched), and GCC
reached for a `q` register to initialise a two-element array of pointers.

Either the kernel enables FP or programs must not emit it. `PROG_CFLAGS` now
carries `-mgeneral-regs-only`, the same flag the kernel is built with, which is
the honest choice while nothing needs floating point. Without it a program can
die on a statement that looks incapable of failing.

172 → 173 checks.

## D-057 — The locking D-050 said would be needed next

D-050 turned interrupts on for the duration of a system call, because a call
that waits for a device cannot be woken by that device otherwise, and wrote at
the same line that this kernel has no locking and that it would be the next
thing to be wrong. This is that.

**On one CPU the answer is not a lock, it is masking the interrupt.** There is
no other processor to race with — only the handler that can interrupt us. So
`lock.h` is `irq_save`/`irq_restore` and deliberately not dressed up as a
spinlock: an uncontended lock word here would say, falsely, that the SMP
question had been answered. It has not been. Every section marked is a place
that will need a real lock the day there is a second core, and marking them is
most of that work done in advance.

Four races, each a read-modify-write an interrupt can land inside:

**The page allocator.** Popping the free list is read the head, follow it, store
the next. A timer tick between the first and third reschedules, and a second
thread reads the same head — after which two address spaces own one page and
nothing reports anything until they disagree about what is in it. `free_page`
and the bump path have the same shape.

**Choosing who runs next.** `schedule()` reads every slot's state while the
console interrupt writes those states. The mask is held *across* `cpu_switch`,
which looks wrong and is not: each thread saved DAIF on its own stack going in
and restores its own value when something schedules it again. A thread running
for the first time never reaches that line — it starts in the trampoline, which
has unmasked interrupts itself since D-037.

**Collecting a child**, which scans slots and then claims one.

**And the one that is a real bug rather than a theoretical one: the lost
wakeup.** `sys_read` checked the buffer, found it empty, and then marked itself
blocked. A character arriving in that gap finds nobody blocked, wakes nobody,
and the reader sleeps with its interrupt already spent — a prompt that ignores
the key you pressed, which looks exactly like a dead console driver and would
have been blamed on one. The mask now covers the check *and* the marking, so
`thread_block_on_console` is documented as requiring it: the section belongs to
the caller, because the caller is where the condition is tested.

`thread_exit` frees an address space outside its section, on purpose. Returning
hundreds of pages with interrupts off is a timer tick missed and a console
character dropped, and nothing else can reach that space — the process is a
zombie before the first page goes back.

**What is not claimed:** none of these was reproduced. They are races whose
windows are a handful of instructions, and a test that provokes them reliably
is a larger piece of work than the fix. What can be said is what was run: the
kernel boots, the demonstration completes, the shell reads keys typed at it and
runs programs, and `make test`'s six boot checks pass. The lost wakeup is the
one with a visible symptom, and the shell reading input after the change is
evidence it is not being lost — but not proof it ever was.

## D-058 — Acknowledge the interrupt first, then read the port

A line typed at the shell arrived. A line *pasted* at the shell arrived as its
first three characters, and then the machine sat there. It was named in D-054
as a known defect with the ring buffer suspected, which was wrong: the ring is
256 bytes and was never near full.

`console_interrupt` drained the port and then wrote the acknowledgement:

    while ((c = hw_getc()) >= 0) ring_put(c);
    reg_write(PL011_ICR, ...);          /* too late */

A character arriving between the last read that found the port empty and that
write has its interrupt status cleared by it. The character is in the port, no
interrupt is pending for it, and the reader is asleep waiting for one that will
never come. Whatever is typed next wakes it and the earlier bytes appear late —
or, if nothing else is typed, never.

**Acknowledging first can only cost a spurious interrupt with nothing to read**,
which the drain loop handles by finding the port empty. That is the whole fix,
and it is two lines moved.

It was worth isolating which change did it. The receive FIFO was also turned on
in the same session — `LCR_H`'s FEN bit had never been set, so the port held
exactly one character and `IFLS`, set to "raise at 1/8 full", was an eighth of
nothing. That is a real defect too. But tested on its own, **the ordering fix
alone is sufficient and FEN alone would only have made the window smaller**. A
fix that hides a bug is worse than no fix, and the two are recorded apart so
nobody later concludes the FIFO was the answer.

Changing `LCR_H` requires the UART disabled and the transmitter idle first, or
the character in flight is mangled. QEMU does not enforce that; real hardware
does, which is the sort of difference discovered on a board rather than here.

### The check, and why this one is allowed to exist

`make test` now boots the shell initrd, waits for the prompt, writes a whole
line in one go, and asserts that `argv[3]` arrives. Feeding one character at a
time is exactly the case that always worked, so the line has to go in at once
or the check tests nothing.

D-038 says a check that is only sometimes right is worse than none, so it was
run three times before being kept: 174, 174, 174. With the acknowledgement moved
back after the drain it reports 173 passed, 1 failed. It waits for the prompt
and for the answer rather than sleeping a fixed time, which is what keeps the
number steady.

173 → 174 checks.

## D-059 — A descriptor names an object

`read` meant the console and `write` meant the console, and the number in front
of them was checked only to reject anything unexpected. A shell cannot be
written that way: `a | b` is two programs that both believe they are talking to
a terminal, and the whole trick is that neither of them knows otherwise.

So the number names an object, the object is refcounted, and the table belongs
to the process. `fork` duplicates the table and each object gains a holder,
which is what lets both halves of a pipeline close the end they do not use
without taking it from the other. `exit` closes what is left, and **that is
where end-of-file comes from** — a reader is released when the last writer lets
go, not when the first does. A pipe that never reports EOF is a shell that
hangs after every command.

Fixed pools rather than an allocator: eight pipes of one page each, thirty-two
file objects. A machine that wants more wants a different kernel, and saying so
with `-EMFILE` is better than discovering it as a failed page allocation
somewhere unrelated.

Five calls added, all with Linux's aarch64 numbers: `close`, `dup`, `dup3`,
`pipe2`. There is no `pipe` or `dup2` on aarch64 — `pipe2` and `dup3` are the
whole interface, and adopting that rather than inventing friendlier names is
D-030's principle again.

**`upper` is the evidence.** It reads standard input, writes it back in
capitals, and knows nothing about where either is attached:

    verk$ hello world | upper
      [HELLO] ARGV[1] = "WORLD"
    verk$ hello plain
      [hello] argv[1] = "plain"

Same program, same binary, same argv. The capitals are the descriptor having
been redirected rather than the program behaving differently — which is why the
unpiped case is also checked. A shell that simply capitalised everything would
pass the first assertion and fail the second.

### Two checks that were wrong before they were right

**The alignment check from D-053 was reporting a defect that was not there.**
It scanned for a `stur` of a 64-bit register at an offset that is not a
multiple of 8, on the reasoning that a 64-bit `str` can only encode an aligned
offset. The reasoning is right about the instruction and wrong about the check:
the displacement is measured from whatever the base register holds, and the
compiler routinely points the base into the middle of a struct.
`ldur x2, [x1, #44]` in `thread_wake_pipe` was flagged while `x1` was already
`&threads[i].state`, making the real offset 64 and perfectly aligned.

D-028 said it in as many words: *a test that reports a defect that is not there
is worse than no test, because the next person deletes it.* It has now been
learned twice. The check is the flag's presence in `COMMON` — which is what
would have caught the original defect anyway, since the flag was simply absent.
D-053's claim that only the disassembly check could catch it was wrong.

**And the pipeline check was flaky, for a reason that was not timing.** It
failed roughly one run in three and survived a longer timeout unchanged. Each
`printf ... > "$SD/p.in"` opens the fifo and closes it, and that close is
end-of-file on QEMU's input — after which the second line may never be read at
all. Holding one descriptor open across both writes made it 176/176 four times
running. The instinct to blame timing for an intermittent test is the thing to
distrust; the first fix attempted was a longer wait, and it changed nothing.

174 → 176 checks.

## D-060 — Redirection, and the smallest thing that can hold a file

`> f` and `< f` needed something to open. The initrd answers half of it: it is
a cpio archive, so a name can be found and read, and `upper < saved` has a
source. It answers none of the other half — a cpio archive has no directory
structure and nothing can be written to it (D-047).

Rather than pretend otherwise, writing goes to a small fixed table of named
buffers: four files of one page each, gone at the next boot. **It is not a
filesystem, which is why it is not called one.** It is enough for `a > f` and
`b < f` to mean what they say, and calling it a filesystem would be a claim
the code cannot support.

Reading prefers a memory file over the archive entry of the same name, because
a program that wrote a file expects to read back what it wrote.

`openat` is the call — aarch64 has no `open` — and the `dirfd` argument is
accepted and ignored rather than pretended about, since there are no
directories for a relative path to be relative to.

The shell strips `>` and `<` from the words before anything else looks at
them, so a filename is never mistaken for an argument, and it opens in the
child between the fork and the exec. That is the only moment the descriptors
can be arranged for a program that does not know they were.

**Both directions in one exchange, because either alone proves less:**

    verk$ hello redir > saved      (nothing on the console)
    verk$ upper < saved
      [HELLO] ARGV[1] = "REDIR"

Checking only `> f` would pass on a shell that discarded the output entirely,
so the test asserts both that the lower-case line never reached the console and
that the upper-case one did.

### The flaky test, third time

This check went intermittent twice more before it was right, and both times the
first instinct — wait longer — was wrong.

The fix that worked was to stop waiting for each command's *output* and start
waiting for the **prompt to come back**. Waiting on output makes a command that
failed indistinguishable from one that is slow, and the next line then goes in
while the shell is still busy. Counting prompts is the shell saying it is ready,
which is the only thing worth waiting for.

Ten consecutive runs at 178/0 before it was kept. There was one 177/1 earlier in
that sequence, immediately after an edit and before a clean rebuild, and its
failing check was not recorded — so it is written down here rather than assumed
away.

176 → 178 checks.

## D-061 — The bootloader gets a key it can actually refuse with

D-011 wrote the verification and left it off: `VBOOT_PUBKEY` is thirty-two
zeroes, `verify_payload` returns 0 before reading a signature when no key is
configured, and nothing in the build ever ran `vsign`. So the property D-011
claims — that vboot refuses a kernel it cannot verify — was true of the code
and false of every build. The README says so now; this makes it available.

**Signing is opt-in, and the key stays out of the repository.**

    vsign keygen verkos.key verkos.pub
    VERK_SIGN_KEY=$PWD/verkos.key make boot ARCH=aarch64

A committed key would be worse than none: an unsigned build is honestly
unverified, while a build trusting a key everyone has only *looks* verified.
So stage 70 derives the public half, generates the header into the build
directory, compiles vboot against that, and signs the kernel and the initramfs.
The checked-in all-zero header is never edited and a clone still builds a
bootloader that boots.

Two details that are decisions rather than mechanics:

**The public key is derived from the private one, never read from a file
beside it.** A build handed a `.pub` that does not match the key it signs with
produces a boot chain that refuses every kernel — or, if the mismatch runs the
other way, one that trusts a key nobody is holding. Derived, they cannot
disagree. `vsign pubof` exists for that and nothing else.

**`vboot.c` includes `<vboot-key.h>` rather than `"vboot-key.h"`.** A quoted
include is resolved from the including file's own directory first, so the
committed header could not be overridden by `-I` and signing would have meant
editing a tracked file. That is how a private key ends up in a public
repository.

**The initramfs is signed too, not only the kernel.** It is where the root is
found and where the passphrase for an encrypted one is read, so an unverified
initramfs is an unverified system however carefully the kernel is checked.

### What is checked, and what is not

`make test` gains seven checks, and the two that matter are the ones that look
at what came out rather than what was asked for: **a vboot built against a
generated header contains those thirty-two bytes, and the default build does
not.** A build that silently kept the zeroes passes every structural check
while verifying nothing — which is exactly the state this entry is fixing.

**Not checked: a machine refusing to boot a tampered kernel.** The signature
mathematics is covered against the RFC 8032 vectors and a flipped byte, and
the key is known to reach the binary, but no firmware has been asked to start
a signed system and then refused a modified one. That needs a full distro
build, and it is the honest next step for this feature rather than something
to claim now.

### The shell test, flaky a third time

`a < f` failed one run in ten. The prompt counter used `grep -c`, which counts
matching *lines* — and a program's output shares a line with the prompt often
enough that the count stopped rising while the shell was perfectly ready, after
which the next command went in early and the last check in the sequence failed.
Counting occurrences with `grep -o` fixed it: ten consecutive runs at 185/0.

Three separate causes now, in one test: a fifo closed between writes, waiting on
output instead of readiness, and counting lines instead of matches. Each looked
like timing and none of them was.

178 → 185 checks.

## D-062 — Chains, background jobs, append — and a test harness that had to be admitted to

`a | b | c`, `a &` and `a >> f`. The kernel needed two things: an append mode
for `openat`, and `WNOHANG` on `wait4` so a shell can collect a background job
without blocking. A shell that never collects one leaves a zombie holding a
process slot, and sixteen of those is a machine that cannot fork — D-054's leak
arriving by a different road.

The chain itself is a loop around the same three calls, and the parent keeps
exactly one descriptor between iterations: the read end the next program will
use. Holding any more means a pipe whose writer has finished still has an open
write end somewhere, and its reader never sees end of file.

### The part worth recording is the debugging, not the feature

The change appeared to break the kernel: programs were killed, exit status 255.
It was reverted rather than pushed, on the reasoning that a crash is worse than
a missing feature.

It was not the kernel, and it was not the shell. **It was the test.** Applied in
halves against the existing suite, the kernel changes gave 185/0 and the shell
changes gave 185/0. Only the new harness failed — and its debug output said so
in one line, once it was asked to print the log instead of a verdict:

    verk$ argv\[3\] = "three"
      [kernel] exec: no '/argv\[3\]' in the initrd

The harness was sending the *grep pattern* as the command. A helper had been
refactored from `(label, command, want)` to `(label, want, command)` and the
call sites were never reordered, so every one of them ran its own assertion as
a shell command and then failed to find it.

Two hours went into "which half of the product is broken" for a bug that was in
neither half. The lesson is specific: **when a change breaks a test, find out
whether the test still tests what it says before deciding the product is
wrong.** The revert was still the right call at the time — it was made with the
information available, and it cost nothing that a saved patch did not keep.

### And then the checks were switched off anyway

Even with the arguments the right way round, one run in ten fails, on whichever
check is unlucky. Four different causes have been found and fixed in this
harness already — a fifo closed between writes, waiting on output instead of
readiness, counting lines instead of matches, and now argument order — and a
fifth remains.

So the six shell checks are kept and do not run unless asked:

    VERK_SHELL_CHECKS=1 make test

D-038 is the reason: a check that is only sometimes right teaches everyone to
re-run it, after which a real regression looks exactly like the noise they have
learned to dismiss. Switching them off is worse than having them work and
better than having them lie, and the state is written in the file rather than
left for someone to discover.

**The fix is not a longer timeout.** That was tried four times and was wrong
every time. It is to stop driving an interactive shell over a fifo: teach vsh to
read commands from a file in the initrd, and the class disappears because there
is no timing left to get wrong. That is the next piece of work on this, and it
is a feature rather than test scaffolding, which is usually the sign that it is
the right one.

**What is verified by hand and not by `make test`:** three-stage chains,
`a >> f` keeping what `a > f` wrote, and `a &` returning the prompt with the job
collected at a later one. Each was run and watched; none is guarded.

185 → 186 checks when the shell checks are on, 180 when they are not.

## D-063 — A shell that can be run, not only typed at

`vsh` opens `/rc` from the initrd onto its own standard input when the archive
carries one, and runs it. The loop below that does not change at all — the
shell does not know whether it is being typed at, which is the point. A second
code path for scripts would drift from the interactive one, and then a test
would be exercising something nobody uses.

It needed one real change: **the shell had to learn to split lines itself.**
The console hands over exactly one line per read, because the line discipline
is in the kernel (D-049); a file hands over as much as fits in the buffer. Once
`vsh` buffers and splits, the same loop reads a person and a script without
knowing which it has. Reading 0 bytes is end of input, and the shell exits —
which is what makes the script terminate rather than spin.

Scripted lines are echoed, because a terminal is what normally echoes them and
a log that shows the answers but not the questions cannot be read.

### What this was actually for

The shell checks were switched off in D-062 because the harness that drove them
failed one run in ten. **Five separate causes had been found and fixed in it:**
a fifo closed between writes, waiting on output instead of readiness, counting
lines instead of matches, an argument order that sent each assertion to the
shell as a command — and a fifth never identified. Every one of them presented
as timing, and the first instinct each time was to wait longer. It was wrong
every time.

`make test` now boots one machine with `initrd-script.cpio`, waits for the
shell to say `bye`, and reads the log. No fifo, no input, no synchronisation,
because there is nothing left to synchronise with. **Twelve consecutive runs at
189/0**, against nine in ten before.

The nine checks that were switched off are on again, and three of them cover
things that had only ever been verified by hand: three-stage chains, `>>`
keeping what `>` wrote, and `&` returning the prompt with the job collected at
a later one.

One thing the script exposed that a person never would: **`hello bg &` followed
immediately by `exit` never runs the background job at all.** The shell reads
its next line from memory, so it never blocks, so the child is never scheduled
before the shell leaves. Correct behaviour, and invisible when a human types at
a prompt because typing is itself a delay. The script has two ordinary commands
after the background one, which is what a person's typing would have been.

186 → 189 checks, and the six that needed asking for now run by default.

## D-064 — Copy-on-write, and a reference count that was off by one owner

`fork` copied every page of the address space outright. D-046 said that was the
right thing to do first and the wrong thing to do forever, and named what
copy-on-write would need: write faults, a reference count per page, and a
handler that can tell "not allowed" from "not yet".

All three are here. `vm_copy_space` maps the parent's pages into the child and
takes the write permission away from **both** — copying only for the child
would let the parent scribble on memory the child believes is its own. The
counts live in one byte per page of RAM, 128 KiB for a 512 MiB machine, taken
from the allocator once its bounds are known. A page nobody has shared has a
count of zero rather than one, so the common case costs nothing to maintain.

The fault handler is the part that had to be careful. A write to a shared page
is not a program doing something wrong; it is a program doing the ordinary
thing to memory the kernel arranged to share behind its back. The fault status
says "you may not write here" in both cases, so the handler looks at the tables
instead: mapped, EL0-accessible, and read-only means copy-on-write. Anything
else falls through and the program is killed, exactly as before.

`fork` now returns 34 pages to the free list where it returned 50.

### The bug, which is the entry

`forky` — the program from D-043 that records its process id, spins long enough
to be preempted, and checks the value survived — printed the right answers and
*then* died with an undefined instruction at a code address that had been
running a moment earlier.

`page_unref` returned the count from **before** the decrement. It reads
naturally and it is exactly wrong: two processes sharing a page gave 1, the
first to exit freed it, the allocator handed it to somebody else, and the
survivor carried on executing whatever was written there next.

It is the quietest possible corruption and it is only reachable once fork stops
copying — which is to say, the feature and its worst bug arrived together. What
made it findable was that the failure was *loud and immediate*: an undefined
instruction, not a wrong answer. The counts are now defined as holders
remaining after letting go, with zero meaning the page is nobody's.

An I-cache maintenance pass was written first, on the reasoning that a copied
page may hold instructions and the fetch side would not know. It changed
nothing — the fault was in the freeing, not the copying — but it is kept,
because the reasoning is sound and the day a program writes to its own text is
the day it would have mattered.

**Verified by running it:** child sets 222, parent still reads 100, and the
script initrd runs to `bye`. `make test` is 189/0, four consecutive runs.

## D-065 — mtools and dosfstools leave the build

The last third-party tools in the boot path were the ones that *filled* the
partition the machine starts from. `vmkfs` has made the ESP since D-014, and
then `mkfs.vfat`, `mmd` and `mcopy` were called anyway — because owning a
formatter is not the same as owning a filesystem, and writing a file into one
is the larger half.

It is also where D-052 hid for a year: nothing checked what came out of
`mkfs.vfat`, so the ESP was FAT16. That defect cannot come back now, and not
because a flag is remembered — **vmkfs is FAT32-only and refuses rather than
choosing**, so there is no size at which the wrong answer is available.

`vmkfs --mkdir` and `vmkfs --copy` read the volume's geometry back from its own
boot sector rather than recomputing it. vmkfs halves the cluster size in a loop
until the count is legal, so the size table alone does not predict what is on
the disk, and a writer that disagrees by one sector corrupts silently.

Three things the design had to get right before the first file, rather than
after the first failure:

- **`DIR_FstClusHI` is a separate field from `FstClusLO`.** Every cluster
  number below 65535 fits in the low half, so a writer that sets only the low
  one works perfectly until the first file past 32 MiB of data — which on this
  build is the kernel, on the first build.
- **The allocator stops at the last real cluster.** The FAT is over-provisioned
  by Microsoft's sizing approximation, so there are entries past the end that
  read as zero and look exactly like free space. Allocating one writes past the
  end of the image; a regular file simply grows, and stage 70's
  `dd ... seek=1 conv=notrunc` then puts those bytes over the backup GPT.
- **The FAT sector cache is flushed, into every copy.** A 34 MB file is ~66 000
  links across 500-odd sectors; without a cache it is unusably slow, and with a
  cache the missing final flush leaves the last hundred links zero and the file
  99.8% correct.

### The bug the tests found, which is the entry

A directory cluster holds sixteen entries and a long name costs one entry per
thirteen characters *plus* the short one, so the root of a signed ESP is two
slots from needing a second cluster. Growing it worked. Reading it back did
not: `fsck.fat` walked the chain and reported all 32 files, and **mtools stopped
at five**.

The gap was a single byte. When a directory grows, the slots left unused in the
cluster being left behind still hold `0x00` — and a first byte of zero does not
mean "this slot is free", it means **"the directory ends here, and nothing
after this is a file"**. Every entry in every later cluster was invisible to
anything that honours the marker. They are filled with `0xE5` now.

What makes it worth recording is which tool was right. A checker that walks the
chain and a reader that honours the marker disagreed, and **the reader was
right** — firmware is a reader. Had only `fsck.fat` been consulted, this would
have shipped, and the failure would have been a machine that boots until the
eighth file is added to the ESP.

### Judged by three things that are not ours

`fsck.fat` accepts the volume and renames nothing. mtools reads every file back
byte-identical, including a 34 MB one past cluster 65535, one in a
subdirectory, an empty one, and twenty long names spread across four directory
clusters. And **the Linux kernel's own vfat driver mounts it** and `cmp` agrees.

`make test` gains nine checks and loses the two that asked whether `mkfs.vfat`
was called with `-F 32`, since it is not called at all. 189 → 199.

Still bought, and deliberately: `parted` for the GPT, and ext4 (D-014). The
partitioner is the obvious next one to take, because `vinstall` already writes
a GPT — that code exists and would only need lifting out.

## D-066 — The partition table, and the last third-party tool leaves the boot disk

`parted` said where the EFI system partition *is*, which is the first question
firmware asks and the last one VerkOS was still outsourcing. `vpart` writes it
now: a protective MBR, a header at LBA 1 and its mirror at the last sector, and
128 entries after each.

**`vinstall` did not already own this.** The plan assumed it did — it writes a
GPT, so surely the code was there to lift out. It is not: `vinstall` generates
an `sfdisk` script and runs util-linux. Checking before building saved writing
the wrong thing; the assumption survived until somebody grepped for it.

Three details that are decisions rather than mechanics:

**The two headers are not copies.** They differ in `MyLBA`, `AlternateLBA` and
where their entry array lives, and each one's CRC is computed with its own CRC
field zeroed — so the checksums differ too. Copying the primary's checksum into
the backup produces a disk that passes every check until the primary is
damaged, which is the single moment the backup is read.

**A type GUID is mixed-endian**: the first three fields little-endian, the last
two as written. Getting it wrong produces a partition the firmware does not
recognise as an ESP, with no error anywhere — it simply finds nothing to boot.

**It is not a partition editor.** It writes a table from nothing; it does not
read one, resize one, or move anything. Adding a partition beside somebody
else's data is a much higher bar (D-015) and a different program.

### Judged against parted, and against the case that matters

`sfdisk --verify` reports no errors. The geometry is byte-for-byte what
`parted` produces for the same disk — same first sector, same last, same type
GUID — which is the useful comparison, because parted is what this replaces.

And the backup is tested by destroying the primary: `make test` zeroes LBA 1
and requires the partition to still be found. A backup that merely exists
proves nothing, and this is the one structure whose whole purpose is to be read
when the other is gone.

### What is left

Nothing, on the boot disk. `make boot` now goes from an empty file to something
a machine starts from using only code in this repository: `vpart` for the
table, `vmkfs` for the filesystem and its contents, `vboot` for the loader,
`vcpio` and `vgzip` for the initramfs.

Still bought elsewhere, and still deliberately: `mkfs.ext4` and `e2fsck` for
the root filesystem (D-014), `sfdisk` inside `vinstall` — which now has an
obvious replacement — and `xorriso` as a test-only judge of what `viso` writes.

199 → 207 checks.

## D-067 — sfdisk leaves the installer too

D-066 took `parted` out of the boot disk and left `sfdisk` inside `vinstall`,
where it was doing the same job on a real machine's real disk. `vpart` grew the
three partition types the installer needs — root, `/home`, swap — and the
installer calls it instead of generating a script for somebody else's tool.

**The root is sized here, not left to take "the rest".** Only the last
partition can mean the rest, and root is not last once `/home` or swap exist.
A partitioner that quietly gives the remainder to the wrong one produces a disk
that installs, boots, and has no swap — with nothing reported anywhere, which
is this log's recurring shape.

`vinstall`'s required-tools list is the other half of that. It refuses to touch
a disk before checking that everything it needs exists (D-015), and `sfdisk`
was on that list; now `vpart` is. A tool list that names something the program
no longer runs is a check that passes for the wrong reason.

One small thing worth its own line, because it cost a red test: the check
"vinstall never calls sfdisk" was written as `! grep -q sfdisk`, and failed on
the *comment* explaining that vpart replaced sfdisk. A check that greps for a
word rather than for a call is a check about prose. It looks for the call now.

207 → 211 checks.

### Where this leaves the third-party tools

    ours          vpart, vmkfs, viso, vboot, vcpio, vgzip, verkbox, the vX set
    bought        mkfs.ext4 + e2fsck for the root filesystem, deliberately (D-014)
    bought        libxcrypt for password hashing, deliberately (D-013)
    bought        cryptsetup for LUKS, deliberately (D-020)
    test only     xorriso, fsck.fat, mtools, sfdisk — as judges of what we wrote

The remaining three are the ones D-014 called the honest trade: own the half
that is small and specified precisely enough, buy the half where a subtle
mistake is silent until someone's data is gone. Nothing left in the boot path
is bought by accident.

## D-068 — Firmware read what our tools wrote

Everything in D-065, D-066 and D-067 was checked against `fsck.fat`, `mtools`,
`sfdisk` and the Linux vfat driver. All four are readers, and none of them is
the one that matters: the machine's firmware is what decides whether a disk
boots, and it had never seen a partition table or a filesystem VerkOS wrote.

`make boot` was rerun so the boot disk came entirely from our own tools, and
the result was started under AAVMF. Its own log:

    BdsDxe: loading Boot0001 "UEFI Misc Device" from VenHw(...)
    BdsDxe: starting Boot0001 ...

      vboot: VerkOS 0.1
      [..] loading \vmlinuz
      [ok] device tree built and installed (firmware had none)
      [ok] kernel loaded, 46780928 bytes
      [..] loading \initramfs.img
      [ok] initrd loaded, 6569926 bytes
      [->] starting VerkOS

    EFI stub: Booting Linux Kernel...
    EFI stub: Loaded initrd from LINUX_EFI_INITRD_MEDIA_GUID device path
    EFI stub: Using DTB from configuration table
    EFI stub: Exiting boot services...

Four separate claims, each settled by something other than us:

- **The firmware found the partition.** The table came from `vpart` — protective
  MBR, both headers, both entry arrays — and BdsDxe located a bootable device
  in it.
- **The firmware read the filesystem.** `\EFI\BOOT\BOOTAA64.EFI` was placed
  there by `vmkfs --copy`, and the firmware loaded and started it.
- **vboot read the large files back at full size.** 46 780 928 bytes of kernel
  and 6 569 926 of initramfs, through vboot's own FAT reader, which shares no
  code with the writer. The kernel is the file that crosses cluster 65535 —
  the `DIR_FstClusHI` case — and a writer that got that wrong would produce
  exactly this log up to the point where the kernel is short.
- **Linux accepted the initrd and the device tree**, and exited boot services.

**What was not seen: a login prompt.** The serial log ends at
`Exiting boot services`, and repeated attempts to keep QEMU alive long enough
to catch the rest were killed by the harness rather than by anything in the
system. So what is claimed here is the boot *chain* — table, filesystem,
loader, kernel handover — and not the userland behind it, which STATUS.md
already records from earlier boots and which none of this week's work touched.

It is worth saying which of the two halves this proves. The readers that
passed earlier are thorough about structure and indifferent to intent: fsck
will tell you a directory chain is well-formed and not that a reader stops at
the first `0x00` in it (D-065). Firmware is the opposite — it says nothing
about structure and everything about whether the machine starts.

### D-068, continued: the login prompt, and the condition on it

The boot does reach a shell — `root@verkos:~#`, 492 programs, the whole
userland — but only when **the firmware provides a device tree**. That is the
finding, and it was found by chasing a prompt that would not appear.

Under plain AAVMF, which describes the machine with ACPI and publishes no tree,
the log ends at `EFI stub: Exiting boot services...` every time, to the byte.
vboot says `device tree built and installed (firmware had none)`, hands Linux
the tree it synthesised, and **Linux goes silent** — no console, no panic,
nothing. Started with `-machine virt,acpi=off`, so that the firmware publishes
a real tree, vboot says `firmware provides a device tree` and the same disk
boots to a prompt.

So the synthetic tree of D-035 and D-036 is sufficient for **stam** and not for
**Linux**. That is not surprising in hindsight — it was built to the question
*what does our kernel need to find its hardware*, and Linux asks a great deal
more — but it was not known, and the way it fails is the expensive kind: a
machine that starts, takes the kernel, and says nothing ever again.

It also qualifies a claim. `docs/STATUS.md` records the boot chain as verified,
and it is — but with a firmware that hands over a tree. A real ARM server with
ACPI-only firmware would boot VerkOS into silence today, and nothing in the
tree said so until now.

**Not diagnosed:** which node Linux is missing. The obvious candidates are the
`chosen/stdout-path` the console binds through, and the CPU and interrupt
nodes D-035 listed as absent and D-036 filled in from MADT — filled in well
enough for stam, which asks for one CPU and one timer. Finding out means
dumping the tree vboot installs and comparing it against the one QEMU
publishes, which is a contained piece of work and the obvious next step.

Everything else in D-068 stands unchanged: the firmware read the partition
table vpart wrote, loaded vboot out of the filesystem vmkfs wrote, and vboot
read back all 46 MB of the kernel. Those hold in both configurations, because
they happen before the tree matters.

## D-069 — Why Linux went silent: four things the synthetic tree never had

D-068 established that Linux boots when the firmware publishes a device tree
and says nothing at all when vboot synthesises one. This is which nodes are
missing, established by listing every `fg_node`/`fg_prop` call in `vboot.c` and
comparing it against the tree QEMU's `virt` machine publishes for the same
hardware.

**1. There is no `phandle` anywhere, and therefore no `interrupt-parent`.**
`grep -c 'phandle\|interrupt-parent\|clock' pkg/vboot/vboot.c` returns zero.
QEMU gives its GIC `phandle = 32771` and points the machine at it. Without one,
Linux cannot resolve *any* `interrupts` property it reads — including the
architected timer's, which it needs before it can do anything at all. This is
the fatal one, and it is fatal early enough to explain a machine that prints
nothing.

**2. The serial has no clock.** QEMU's PL011 carries
`clock-names = "uartclk","apb_pclk"` and `clocks` pointing at a fixed-clock
node. Linux's `amba-pl011` driver will not probe a PL011 without one. vboot's
serial node has exactly two properties: `compatible` and `reg`.

**3. The serial has no `interrupts`,** which the same driver also requires.

**4. `compatible` is `arm,pl011` alone**, where QEMU writes
`"arm,pl011","arm,primecell"`. The second is what makes the AMBA bus claim the
device in the first place.

Also missing but survivable: `enable-method = "psci"` on the CPUs and a `/psci`
node, without which secondary cores never start. Linux would come up on one CPU
and say so — which is a different failure from saying nothing.

### Why this was invisible

Every one of the four is something **stam does not need**. stam reads the
timer's interrupt number straight out of the `interrupts` property and never
resolves a phandle, because it has exactly one interrupt controller and knows
where it is. Its console is a register write; a PL011 clock is a fact about a
driver model it does not have. So the tree was built to the question *what does
our kernel need to find its hardware*, it answered that question completely,
and the answer was tested by a kernel that asks a fraction of what Linux asks.

D-035 said the tree was "deliberately thin: what cannot be discovered without
firmware help, and nothing more". That was the right principle and the wrong
scope — for stam. For Linux, a phandle is not decoration.

**Not fixed here.** Writing it means giving the GIC a phandle, emitting an
`interrupt-parent`, adding a fixed-clock node and wiring the PL011 to it, and
reading the console's interrupt out of SPCR — which the ACPI reader does not
currently take. That is a contained piece of work with a decisive test already
built: boot the same disk under plain AAVMF and see whether a prompt appears.

It is worth stating what this does *not* affect. `vpart`, `vmkfs` and the boot
chain up to the kernel handover were proved in both configurations (D-068),
because all of that happens before the tree matters.

## D-070 — The four gaps were real, and none of them was the cause

D-069's diagnosis was acted on. vboot's synthetic tree now carries what it was
missing: a `phandle` on the interrupt controller and an `interrupt-parent` at
the root, a `fixed-clock` node wired to the PL011 through `clocks` and
`clock-names`, the `arm,primecell` compatible that makes the AMBA bus claim the
device, the console's interrupt read out of SPCR's global-system-interrupt
field, and `enable-method = "psci"` with a `/psci` node whose conduit comes
from the FADT rather than from a guess — `hvc` and `smc` are both plausible and
the wrong one fails by doing nothing.

Every one of those was genuinely absent and is genuinely needed. None of them
was the reason Linux went quiet.

**The kernel is not running at all.** Booted with
`earlycon=pl011,0x9000000` on the command line — which writes to the port
directly and consults no device tree whatsoever — the log still ends at
`EFI stub: Exiting boot services...`, to the same byte. A console that needs no
tree produced nothing, so the failure is before the console, not in it.

That reframes the problem and is worth more than the fix it invalidates. The
question was "why can Linux not find its console"; it is now "why does Linux
stop executing between the EFI stub's last message and its own first one". The
candidates are what the kernel touches in that window and nothing else: the
memory node, the handover state, or an FDT the stub cannot extend.

**Deliberately kept anyway.** The five additions are correct against the
binding documents and against the tree QEMU publishes for the same machine, and
a tree that is right for reasons unrelated to today's bug is still right. They
are also now the *only* remaining differences worth ruling out, which is
cheaper than having to add them later while diagnosing something else.

**What this cost, and what it bought.** Roughly an hour, a fix that did not
fix, and a much better question. The alternative — believing the diagnosis
because it was well-argued and the evidence lined up — would have produced a
tree that looks complete and a machine that still says nothing, which is
exactly the class of defect this log exists to catch. The check that settled it
took one command line: give the kernel a console that cannot fail and see
whether it speaks.

`make test` is 211/0, unchanged: none of this is covered by a check, because
nothing in the suite boots a distro under firmware. That is the honest gap, and
it is the same one D-055 closed for stam and nobody has closed for Linux.

## D-071 — The timer fired at nobody, and printing the tree is what found it

Linux now boots on the tree vboot synthesises. It reaches userspace and runs
`urverk`. The thing that had stopped it was not any of the four gaps D-069
named and D-070 filled — it was the timer.

`vboot` wrote the architected timer's `interrupts` as four identical triples of
`<1 ppi 0x08>`. Two things wrong with that, and the second is the one that
mattered:

- **The four are not the same interrupt.** GTDT lists secure EL1, non-secure
  EL1, virtual and hypervisor timers separately, and only the non-secure one
  was ever read. A kernel that uses the virtual timer — which one under a
  hypervisor does — was handed the physical timer's number.
- **The third cell of a per-processor interrupt is not a trigger type alone.**
  Its high byte is the mask of processors the interrupt is delivered to.
  `0x08` left that mask empty, so the timer was described as firing at **no
  processor at all**. QEMU writes `0x304`: processors 0 and 1, level, high.

An interrupt targeted at nobody is not an error to anybody. The controller
accepts the configuration, the timer runs, and the delivery goes nowhere.

### What actually found it

Not reasoning. Three rounds of careful, well-supported, wrong diagnosis came
first — a missing phandle, a missing clock, a missing compatible — each argued
from a real difference against QEMU's tree, each fixed, and none of them the
cause. What settled it was **printing the tree**.

`dumpdtb` in `vboot.conf` walks the blob that was just built and prints every
node and property before installing it. The identical triples were visible
immediately, in one line, having been invisible in the source for as long as
the source was what anybody read. It also caught two bugs introduced *by the
previous round of fixes*: `compatible` and `clock-names` were written with
`sizeof s - 1`, which strips the terminating NUL, so both properties were
malformed strings that a reader would take as raw bytes.

That is the third time in this log that the answer was to make the program
report what it did rather than what it was asked to do — after the boot menu
entry that named a flag nothing implemented (D-028), and the ESP that was
FAT16 while every document said FAT32 (D-052).

### Where it stops now

`urverk` runs and cannot find its root: `/dev/vda` never appears. The synthetic
tree has **no PCI host bridge**, so a `virtio-blk-pci` disk is invisible to
Linux — the tree describes memory, the console, the CPUs, the interrupt
controller and the timer, which is everything ACPI was asked for and nothing
about where the disks are. ACPI's MCFG has the bridge's configuration space,
and reading it is the obvious next node.

So the honest state: **Linux boots on our tree and reaches an emergency shell**,
which is further than it has ever come without firmware handing over a tree of
its own, and one node short of a system.

211 checks, unchanged. Nothing here is guarded, because nothing in `make test`
boots a distro under firmware — the same gap D-055 closed for stam and D-070
already named.

## D-072 — Stop handing Linux a worse description than it already had

The PCI bridge was going to be the next node. It cannot be: PCI resources live
in ACPI's DSDT as AML, so emitting `ranges` and `interrupt-map` would mean an
AML interpreter inside a bootloader. Hardcoding QEMU's windows was the other
option, and D-031's first sentence rules it out — *nothing in it is hardcoded*
is the reason the kernel runs on more than one machine.

Which made the question worth asking properly: **why is Linux being given a
device tree at all?**

vboot synthesises one because **stam** cannot read ACPI (D-035). Linux can, and
on arm64 does it well. But Linux prefers a device tree whenever one is present
— so installing ours makes it ignore a complete description of the machine in
favour of an incomplete one, and then go looking for a disk on a bus the
incomplete one never mentions. Every gap chased since D-069 was a consequence
of supplying the tree, not a reason to improve it.

So `synthdtb` is a configuration keyword and it is **off by default**. That
default is the decision. A kernel that cannot read ACPI turns it on; stam is
the only one.

**The result, under plain AAVMF, with no `acpi=off` and no tree from the
firmware:** the machine boots to `root@verkos:~#`. 492 programs, both CPUs, the
disk found, the whole userland. That is what an ACPI-only ARM server does now,
and it is what D-068 said would boot into silence.

### What the detour was worth

Four wrong diagnoses and two real bugs, all of them still fixed and all of them
still correct: the timer that fired at no processor (D-071), four identical
interrupt triples where four different ones belong, a missing phandle and
interrupt-parent, a PL011 with no clock and no interrupt (D-070). The tree is
much better than it was, and stam is the kernel that benefits.

The lesson is narrower than "we were wrong four times". Each fix was argued
from a real difference against a working tree, and each was needed. What none
of them questioned was the premise — that the tree should be there. The
evidence for the premise was that vboot had always installed one, which is not
evidence.

211 checks, unchanged. Nothing here is guarded, and after five rounds of this
that is the most uncomfortable line in the entry.

## D-073 — Something boots the distribution now

D-055 made `make test` boot stam and said, in as many words, that the next
defect of that shape would also be found by a person rather than by a test. It
was: five entries, D-068 to D-072, about a machine that took the kernel and
never spoke again. Not one of them would have been needed if something had
started the thing.

`make test` now boots the whole distribution under UEFI firmware and reads
what it printed. Seven checks, and each names a different link in the chain, so
a failure says where it broke rather than that it broke:

    the firmware finds the partition table vpart wrote
    vboot runs, out of the filesystem vmkfs wrote
    vboot reads the whole kernel back off that filesystem
    Linux runs and says which machine it is on
    the root filesystem is found and mounted
    the distribution reaches a shell
    nothing panicked on the way

**Plain `virt`, deliberately** — ACPI only, no device tree from the firmware.
That is the configuration D-072 fixed and the one a real ARM server is. Booting
the easy case would have passed on every day of the last week.

Two details that are the difference between a check and a nuisance:

**`snapshot=on` on both drives.** A test that writes to the images it is
testing passes once, and the second run inherits whatever the first left. It
also stops the test fighting a stray QEMU for a write lock, which is how the
first version failed — six red checks and an empty log, for a reason that had
nothing to do with the system.

**It waits for a marker, not a clock.** Waiting on time makes a system that
failed indistinguishable from one that is slow (D-063), and this one takes
about ninety seconds to come up.

**Verified by reintroducing the bug.** With the synthetic tree forced back on,
`Linux runs` still passes and `the root filesystem is found` and `reaches a
shell` both fail — which is exactly the shape of D-072, and exactly what the
checks are for: the kernel ran, and could not find its disk. Three consecutive
runs at 218/0 with the bug removed.

It skips where the artifacts or the firmware are absent, which includes CI. So
this is a check that runs where the system is built, like the stam boot before
it — the boot is proved on the machine that made it, and not on push.

211 → 218 checks.

## D-074 — The option that parsed and did nothing

D-072 made the synthetic device tree opt-in and left stam — the one kernel that
cannot read ACPI and therefore needs it — depending on a keyword nobody had
booted. That was named as the next thing to check. It was broken.

`vboot.conf` said `synthdtb`. vboot's parser read the file, matched keywords,
and never set the flag. `keyword()` required a **space** after the word, and a
keyword with no value is followed by a newline. So the option existed, was
documented, was parsed for, and did nothing — and stam, started through vboot
on firmware that publishes no tree, refused to continue and said exactly why:

    stam: no device tree in the UEFI configuration table.
    stam: Refusing to continue rather than guess.

That refusal is D-034 working as designed, which is the only reason this was a
five-minute diagnosis rather than another five entries: the kernel said what
was missing instead of stopping quietly.

It is also the same defect as D-028 — a boot menu entry naming a flag nothing
implemented — arriving this time in the parser that reads the flags. End of
line is a separator now.

### The check

`make test` builds a boot disk with `vpart` and `vmkfs`, puts vboot, `stam.efi`
and a cpio initrd on it, and boots it under UEFI firmware. Six assertions, and
between them they cover the whole chain in one run: our partition table, our
filesystem, our bootloader, the synthetic tree, and our kernel finding its
hardware through it.

    vboot starts stam as an EFI application
    synthdtb reaches vboot and a tree is installed
    stam finds its console through that tree
    stam finds the interrupt controller and the timer
    stam takes interrupts on the tree vboot built
    the initrd vboot published reaches stam

"takes interrupts" is the one that matters most and is deliberately not a
banner: it asserts on the count of timer interrupts actually taken, because a
fixed string would say the same thing whether or not any had arrived (D-033).

**Verified by reintroducing the bug.** With the space requirement put back,
three of the six go red and the other three still pass — which is the correct
shape: vboot still starts stam, and stam still refuses correctly.

One thing the first version got wrong, worth a line because it is the same
mistake in miniature: the guard required `$STAM_CROSS` to be non-empty, and on
an aarch64 host the cross prefix is *correctly* empty. The check skipped
silently on the only machine that builds the kernel. `none` is the value that
means there is no toolchain.

218 → 224 checks, three consecutive runs.

## D-075 — Signals, and the two places a process can be told to die

Ctrl-C works. A program that counts forever can be stopped from the shell,
which is the difference between a shell you can use and one you can start
things in.

**Default dispositions only, and no handlers.** A handler needs a frame built
on the user stack, a way back through `sigreturn`, and a mask that survives
it — three things worth having once a program wants to catch something, and
nothing here does. Terminating is a default action, and it is the one that was
needed. The exit status is `128 + signal`, which is what a shell already knows
how to report.

**Process groups, because otherwise there is no true answer to what Ctrl-C
interrupts.** Without them the kernel would have to guess, and a guess
interrupts the shell as often as the program. So a process has a `pgid`
inherited from its parent, `setpgid` sets it, and the terminal has a foreground
group that `ioctl(TIOCSPGRP)` points at — the real interface, not one of our
own, because a shell that used a private call could not be replaced by any
other shell (D-030). vsh puts each job in a group of its own and hands the
terminal over around it.

### Two things that were wrong first, and both were about *where*

**Ctrl-C was handled where characters are read.** That is where the line
discipline lives, so it looked like the right place, and it cannot work: while
a program runs the shell is blocked in `wait4` and nobody is reading the
console at all. The byte sat in the ring being nothing. A terminal driver
signals **on receipt** — the whole point is to reach a process that is not
asking for input. It is handled in the interrupt now, and not queued, because a
character that caused a signal is not also input.

**Delivery was only on the way out of a system call.** Also plausible, also
insufficient, and insufficient in exactly the case signals exist for: a loop
that counts makes no system calls, so it never reaches that point. It reached
the end of its 400 rounds and printed *"the child was never interrupted"* —
which is the only reason the gap was visible, because a test that had checked
for the absence of output would have passed.

Delivery now also happens on the way out of an **interrupt**, when the
interrupted context was EL0. Not when it was the kernel: a signal raised while
the kernel is in the middle of something has to wait until the kernel is
finished with it, and the syscall path already covers that case.

### What is guarded and what is not

`progs/sigtest.c` forks a child that spins without calling into the kernel,
signals its group, and checks the child exited with 130. It runs from the
script initrd, so `make test` covers raise, delivery to a running process, the
default action and the status — deterministically, with no terminal involved.

**The typed Ctrl-C is verified by hand and not by a check.** It cannot be
driven from a script: the character has to arrive at a terminal and a script is
read from a file. It was run, and the log says what happened —

    [spin] still here
    ^C
      interrupted
    verk$

— but nothing guards it, and the honest thing is to say which half is which.

224 → 226 checks.

## D-076 — a processor that accepted the start and never arrived

**2026-07-28.** `stam` now starts the other processors. On QEMU's `virt` with
`-smp 4`, all four come up:

```
  cpus      4
  cpu 1      online, mpidr 0x1
  cpu 2      online, mpidr 0x2
  cpu 3      online, mpidr 0x3
  cpus      4 online
```

The mechanism is PSCI `CPU_ON`, with the conduit — `hvc` or `smc` — read from
`/psci` in the device tree rather than assumed. Each core is named by the
affinity value in its `/cpus/cpu@N` `reg` property, not by a counter, because
`reg` is what `CPU_ON` takes and a counter only happens to match it.

What the secondary needs and cannot work out for itself is left in one
structure: a stack, `mair`, `tcr`, `ttbr1`, `sctlr`, and where to go once
virtual memory is on. Every value is **read off the running primary** rather
than recomputed. Two code paths that must agree about page tables are a defect
generator, and this way there is only one.

### The report that did not depend on what it reported

The first version waited for nothing. `CPU_ON` returned 0, so it printed the
core as started. That is a return value meaning *the request was accepted*, and
printing "online" from it would have said online whether or not any processor
was executing — the defect this project keeps finding. So the counter is
incremented by the secondary itself, in C, after it has configured its own MMU;
the primary waits to see it rise and otherwise says so:

```
  cpus      cpu 1 accepted the start and never arrived
```

Which is what it said, for the whole first afternoon. Twice, for two different
reasons.

### First: the address of a symbol, taken from C — for the third time

```c
psci_call(PSCI_CPU_ON_64, aff, kva_to_phys((void *)secondary_entry), ...);
```

`&secondary_entry` in this kernel is routed through the GOT, which holds the
address the image had when it was linked and which nothing relocates. D-037
found this as a thread starting at `0x193c`; D-039 found it again in the first
user program. Here it was PSCI faithfully starting a processor at an address
that no longer meant anything. The rule is in CLAUDE.md, and I broke it anyway,
so the fix is the two-instruction `adrp`/`add` accessor the rule prescribes.

That was necessary and it was not sufficient. The core still never arrived.

### Second: the ground it was standing on

`secondary_entry` runs with the MMU off and a program counter in the low half.
It installs the tables it was given and turns the MMU on. It was given the
primary's *current* `ttbr0` — which, since `mmu_drop_low_half()`, is `l1_user`:
an empty table. So the instruction after `msr sctlr_el1` had nothing to
translate it, and the vector table was unreachable for the same reason, and the
core died without a word.

It now gets `mmu_identity_table()`, the low map that has been sitting unused
since the kernel moved high, and `secondary_main` drops it as its first act.

### How it was actually found

Not by reasoning about it. By writing a character straight to the UART's
physical address as the first instruction of `secondary_entry`, with the MMU
still off, and counting how many appeared. One — so the entry point was reached
and the core died configuring itself, which is a different question from the one
I had been asking. Before that byte, "never arrived" covered both.

This is the same move as D-071. When a thing does not work and there are four
plausible explanations, the cheapest next step is almost never to test one of
them; it is to make the program say where it got to.

### Where it stops

Every secondary parks in `wfi`. It is online, it is idle, and it is not
pretending to share the work. The scheduler's `current` is one variable for the
whole machine and the kernel's locking is `irq_save`/`irq_restore` — masking
interrupts, which locks nothing at all against another processor (D-057 said so
when it was written). Letting a second core into the run queue means per-CPU
state and real spinlocks first. Parking is the honest place to stop.

## D-077 — catching a signal, and the write the kernel was not allowed to make

**2026-07-28.** A program can now install a handler and survive being
interrupted. `rt_sigaction` (134) and `rt_sigreturn` (139), with the interrupted
state saved on the program's own stack:

```
  [catch] interrupted, handled, and resumed with every register intact
  [catch] a handler ran in a loop that makes no system calls, and the program lived
```

The frame goes on the user stack rather than in the process, because a handler
may raise the same signal again or block, and one saved frame per process turns
the second delivery into a lost first one. The stack already nests.

The handler returns through an address the kernel puts in x30. Linux keeps that
trampoline in the vDSO; there is no vDSO here, so the program supplies it — the
arrangement Linux still uses where `SA_RESTORER` survives. A `sigaction` without
one is **refused**, rather than accepted and discovered when the handler returns
and jumps to zero: a fault at an address with nothing to do with the mistake.

The call is not Linux's `struct sigaction`. That struct carries a mask whose
width depends on a fourth argument and a flags word almost every bit of which
means something this kernel does not do, and reading fields that are ignored is
a call that looks supported and is not. The three arguments are the three things
used. The number is Linux's, because the numbering is the cheap part (D-039).

### What sigreturn may not restore

Everything comes back verbatim except SPSR, which says **which exception level
to return to** — and it is read from a page the program can write. Restoring it
as found is a program choosing to resume at EL1: the kernel handing over the
machine because it read a number out of userspace. Only NZCV survives; the rest
is rebuilt as EL0t. ELR is checked as a user address for the same reason.

The frame is also marked, because sigreturn is an ordinary call that any program
may make with any stack pointer, and without a marker the kernel would load 34
registers out of whatever was there. The marker does not make it safe — a
program can write the marker — which is why nothing downstream trusts the
contents either.

### The test, and why it is that shape

The child spins in a loop that makes **no system calls**, so the only way in is
delivery on the way out of a timer interrupt. Before the loop it puts four
values in x9–x12, which are caller-saved: the handler is an ordinary C function
and the compiler is entitled to destroy all four. After the handler returns the
loop compares them.

A check on the handler's own output would pass on a kernel that runs the handler
and loses the frame, which is worse than no handler at all. The exit status
separates the three endings — **7** handled and resumed, **3** handled and lost,
**130** never installed — and each is a number nothing else in the program
produces.

It read 130 the first two times.

### The second 130: fork did not copy the handlers

`thread_fork` calls `slot_init`, which clears the table. Right for a fresh
process, wrong for a copy of one. The child installed nothing and died of the
signal, and the exit status was the only place it showed.

### The third: the kernel was refused a write to the program's own stack

`vm_user_ok(..., write)` returned false for any read-only page, and after fork
every page is read-only because that is what copy-on-write is (D-064). So the
kernel could not build a signal frame on a stack the program owns.

This was never only about signals. **`read()` into a fresh child's buffer had
the same bug**, and never showed, because a program that writes to a page before
reading into it has already broken the sharing — a defect whose appearance
depends on what the caller happened to do first.

The program's own store would have faulted and been resolved. A write the kernel
makes on its behalf has no fault to resolve it, so `vm_user_ok` resolves it: a
read-only page in the range is copied there and then. A predicate with a side
effect, which is worth naming in the header rather than leaving to be
discovered.

`make test`: 231 checks, was 228.

## D-078 — the other processors do the work

**2026-07-28.** D-076 started them and parked them, and said why: `current` was
one variable for the whole machine and the kernel's locking was interrupt
masking, which locks nothing against another core. Both are now false.

```
    boot       scheduled 76 times, last on cpu 0
    worker-a    scheduled 62 times, last on cpu 0
    worker-b    scheduled 49 times, last on cpu 1
    worker-c    scheduled 45 times, last on cpu 3
  the work was spread over 4 processors.
    cpu 0 took 151 timer interrupts
    cpu 1 took  50 timer interrupts
    cpu 2 took  51 timer interrupts
    cpu 3 took  50 timer interrupts
```

### What changed

**`current` is per-processor**, in a `cpu_t` that `TPIDR_EL1` points at — a
register the hardware banks per core, so `this_cpu()` is one instruction on a
path that runs at every scheduling decision.

**A real spinlock**, `ldaxr`/`stxr` because ARMv8.0 has no LSE atomics, released
with `stlr` so that what was written inside the section is visible before the
lock is. No owner field and no recursion count: a lock that can be taken twice
stops reporting the thing it exists to report.

**One lock, not three.** The scheduler, the pipes and the console block on each
other's state — a reader must hold something from before "is there a character?"
until after "I am blocked", and that something is the same thing the waker takes.
Separate locks would be acquired in more than one order by paths that already
exist, and a lock ordering nobody can state is a deadlock that has not happened
yet. The page allocator gets its own, and it is a leaf: it may be taken while the
scheduler lock is held and never the reverse, which is the whole of the rule.

**`THREAD_RUNNING`, distinct from `THREAD_RUNNABLE`**, for as long as a
processor is on a thread. Two cores finding one runnable thread and both taking
it is not a race that gives a wrong answer; it is two processors on one kernel
stack, and there are no further answers.

**The lock is handed across `cpu_switch`.** Whoever is switched to releases it.
A thread that has never run does not resume at the line after the switch — it
starts in a trampoline — so both trampolines call `sched_first_run` first.

**An idle thread per processor**, so `schedule()` always has something to switch
to and there is no special case for "this core is running nothing". By the same
call on every core: the boot core is not the one that happens to work.

### Four wrong answers before it ran

**The secondary's vectors pointed at an address that no longer existed.**
`smpentry.S` installs VBAR before turning the MMU on, which it must — an
exception before that has nowhere to go. But it computes the address PC-relative
while executing in the low half, and `secondary_main`'s first act is to drop the
low half. The core then ran correctly until its first interrupt and vanished,
with no fault reported because reporting one needs the table it could not reach.
QEMU's monitor showed it: `PC=0x40081200` on CPU 1 while CPU 0 carried on. The
fix is to install them again, at the address they have now.

**`thread_wait` called `schedule()` while holding the lock**, which acquires it.
A processor waiting for itself. Split into `schedule_locked`, which assumes both
the lock and the mask and returns with both.

**Objects built against two different `thread_t`s.** `sched.h` grew two fields;
`sched.o` was rebuilt and `console.o` was not, because the Makefile had no header
dependencies. It presented as a scheduler reporting two slices where there had
been fifty and a processor index of 101 — not a number any code here can
produce. Every object now depends on every header: coarse, and a rebuild costs
seconds.

**`kprintf`'s `%u` read sixty-four bits regardless.** It parsed `l` and `ll` and
threw them away, so an ordinary `unsigned` argument read a register whose top
half the calling convention leaves unspecified. Usually zero, which is why it
survived until a count of 2 printed as 4294967396. The formatter honours the
length now, which fixes every call site at once.

### The measurement was wrong before the thing was

"The work was spread over N processors" first counted distinct values of
`last_cpu`, and reported **1** on a two-core machine whenever the same core
happened to run each thread last — a measurement that reports the final instant
and calls it the interval. It is a bitmask now, set by the scheduler on the core
that takes the thread, so it cannot say more than is true and it says "1
processor" on a one-core machine rather than falling silent.

`make test`: 233 checks, was 231, three consecutive runs.

## D-079 — the kernel reads a disk, and it is the one our formatter wrote

**2026-07-28.** Everything `stam` had run came out of an initrd: an archive
handed to it at boot and unpacked in memory. That is enough to prove a process,
a pipe and a shell, and it is not a system — a system reads what it was not
given. It now does:

```
  disk      virtio-blk v1, 131072 sectors (64 MiB)
  fat       FAT32 "STAMDISK", 1 sectors per cluster, 129010 clusters
verk$ diskprog from-the-disk
  [hello] argv[1] = "from-the-disk"
verk$ upper < /note.txt
A FILE THE KERNEL READ OFF A DISK OUR OWN FORMATTER WROTE
```

`diskprog` is in no initrd. `exec` had nowhere else to find it.

### Why FAT32, and why that matters more than the filesystem

Because **`vmkfs` writes it.** Since D-065 and D-068, every EFI system partition
this project produces is formatted and populated by our own tool, with no
dosfstools and no mtools. A reader in the kernel makes the two halves check each
other: an image one wrote and the other reads, sharing no line of code and
written for opposite jobs, so neither can agree with the other by sharing a
mistake about the format. `fsck.fat` and mtools have been judging vmkfs from
outside; this is the project judging it from the other end.

It is also the filesystem the firmware reads, so a kernel that can read it can
read the partition it was booted from.

Read-only. Writing FAT means allocating clusters, updating both copies of the
table and keeping FSInfo honest, and vmkfs already does all three — doing it
again in the kernel is a second implementation of the same on-disk rules, which
is the arrangement D-042 is about.

### The device

virtio-blk over MMIO, discovered from the device tree. Not a second ramdisk,
which would have been easier and would have proved nothing: a driver that talks
to a device through a queue in shared memory is the shape every real driver has.

Both transport versions are handled. Which one appears depends on the QEMU that
happens to be installed, and a driver that supports the one on this machine is a
driver that fails silently on the next.

Polled, not interrupt-driven. That is deliberate: an interrupt-driven version
needs the requesting thread to block and be woken, and what is being proved here
is that the device answers at all. Polling makes the failure "it never answered"
rather than "it answered and the wakeup was lost", and those are different bugs.

### Two places it did nothing, quietly

**The node walk started at -1.** `fdt_next_node` steps past the node it is
given, and -1 is not a node — so it walked nothing, found nothing, and returned
"no disk", which is indistinguishable from a machine that has none. It now says
which of the three it was: no transports in the tree, transports with nothing in
them, or devices that are not block devices.

**The used ring was at the wrong offset.** A legacy virtio-mmio device is given
one page-frame number and derives the descriptor table, the available ring and
the used ring from it and from the alignment the driver declares. The first
version put the used ring at the end of the page — where it would belong if the
alignment were the page size — and then declared an alignment of the page size,
which puts the used ring on the *next* page. Every read timed out with the
device having done nothing wrong. The alignment is the driver's to choose, so it
is 64 now and the whole ring fits in one page.

### Where it stops

The scheduler lock is **not** held across a disk read. Holding the lock that the
console, the pipes and every scheduling decision need for the duration of a
polled sector read is a machine that stops answering while it reads.

An executable is read whole rather than mapped: demand paging off a disk needs a
page cache and a fault handler that can block, and neither exists. The staging
pages are returned before the program starts, which is seventeen pages per run
that would otherwise leak.

`fat_read` walks the cluster chain from the beginning on every call, so a
sequential read is quadratic in clusters. For a 67 KB program that is fourteen
steps. The honest fix is a cached position in `fat_file_t`, and it is not needed
yet.

`make test`: 236 checks, was 233, three consecutive runs.

## D-080 — the whole disk is ours, and the kernel reads it

**2026-07-28.** D-079 gave `stam` a filesystem, on a device formatted as one
filesystem end to end. That is not what a bootable disk looks like. It now reads
the partition table too:

```
  disk      virtio-blk v1, 131072 sectors (64 MiB)
  gpt       EFI system partition 1: 98304 sectors at LBA 2048
  fat       FAT32 "VERKESP", 1 sectors per cluster, 96750 clusters
  boot      read the loader's own vboot.conf off this disk: 121 bytes, 2 entries
```

Every tool in that sequence belongs to this project and none of them shares a
line with the next:

| | |
|---|---|
| `vpart` | wrote the GPT — protective MBR, both headers, both entry arrays, CRCs (D-066) |
| `vmkfs` | formatted the EFI system partition and put the files in it (D-065) |
| `vboot` | is the loader in it, and parses `vboot.conf` to start the kernel |
| `stam` | reads the table, mounts the partition, and reads that same file back |

The last line is the one that matters. The bootloader parses `vboot.conf` to
decide what to start; the kernel it started reads the same file off the same
medium and counts the entries in it. Not a fixed message — a count taken from
the bytes read, so a kernel that read nothing cannot print it.

### What is checked and what is not

The GPT signature and the entry size, because those say the structure is a table
at all. **Not the CRCs.** `vpart` computes them and `sfdisk` verifies them in
`make test`, so a header reaching this code has been checked by something that
is not us; recomputing here would be a third implementation of the same
polynomial, and a disagreement between three copies is harder to place than one
between two.

The ESP is found by its type GUID, which is stored with its first three fields
little-endian and its last two not — a rule with no reason behind it, and the one
a reader who types the GUID out left to right gets wrong. `vpart` writes the same
bytes; this is the other end of D-066's note.

An unpartitioned image still mounts. `gpt_find_esp` says which of "no table" and
"a table with no ESP" it found, and the reader falls back to sector 0.

### Where the circle is not closed

**Not through the firmware.** `make test` boots stam through vboot under AAVMF,
and on that path vboot builds a device tree out of ACPI (D-072) which contains
the console, the interrupt controller, the timer and the PCI bridge — and no
virtio-mmio nodes, because nothing told it about them. So on the firmware path
stam finds no disk. The reading above happens on the `-kernel` path, where
QEMU's own tree describes the transports.

Closing that gap means either teaching vboot to publish what it found, or
teaching stam to enumerate PCI. Both are real work and neither is done, and
saying so is better than a diagram with a line that is not there.

`make test`: 238 checks, was 236, three consecutive runs.

## D-081 — through the firmware, and the circle is closed

**2026-07-28.** D-080 ended by naming the line that was not there: on the path a
real machine takes — UEFI, ACPI, a tree vboot builds out of it — `stam` found no
disk, because the only transport it understood was one that a hypervisor's
device tree lists and ACPI does not describe. It does now:

```
  stam: started as an EFI application
  stam: device tree found, leaving boot services
  pci       ECAM at 0x4010000000, no window in the tree — using the regions firmware placed
  disk      virtio-blk over pci, 139264 sectors (68 MiB)
  gpt       EFI system partition 1: 131072 sectors at LBA 2048
  fat       FAT32 "VERKESP", 1 sectors per cluster, 129010 clusters
  boot      read the loader's own vboot.conf off this disk: 109 bytes, 1 entries
```

Firmware started `vboot`, `vboot` parsed that file to decide what to start, and
the kernel it started read the same file back off the same medium — through a
partition table `vpart` wrote and a filesystem `vmkfs` formatted, over a bus it
enumerated itself.

### MCFG is the part that is not AML

D-072 says why vboot does not emit a PCI node: the routing lives in the DSDT as
AML, and emitting it would mean an interpreter inside a bootloader. That is true
of `ranges` and not of the base address. **MCFG** gives ECAM's address in a
fixed-format table, and with it a kernel can enumerate.

A node with `reg` and no `ranges` is not a half-truth. It says where
configuration space is and says nothing about routing, which is exactly what is
known. Firmware has already assigned every region by the time it hands over, so
nothing needs the routing: `pci_bar_assign` uses what is there and refuses to
place anything when there is no window, rather than writing an address the
bridge does not forward — an access that goes nowhere and reads back all ones,
which is indistinguishable from a device that is absent.

On the `-kernel` path nothing has assigned anything, and QEMU's own tree has the
window. Both paths work and neither is the special case.

### Three wrong answers

**A table of function pointers is taking the address of a symbol from C.** The
two transports were behind a `transport_t` of eight function pointers, which is
how anyone would write it. A static initialiser holding a function's address is
a relocation; this kernel applies none; the entries held link-time addresses. It
presented as an instruction fetch at `0x40088e20` with the kernel running at
`0xffffff8040088e20`.

D-037 found this as a thread, D-039 as a user program, D-076 as a second
processor. **This is the fourth, and the first where the trap wore the shape of
a design pattern rather than of one variable** — which is why the rule in
CLAUDE.md did not fire when I wrote it. The dispatch is a branch on an `int`
now: less elegant, and the same two calls either way.

**The device id names the legacy device, not the modern one.** Searching
1af4:1000–107f found `1af4:1000`, which is the network card QEMU adds by
default. The base class byte says "mass storage" whichever generation the card
is, and that is what identifies it.

**The kernel's window ends exactly where the region begins.** The high half is
512 GiB of straight mapping, physical P at P + KVA_OFFSET, and this machine's
64-bit prefetchable PCI window starts at 0x8000000000 — 512 GiB precisely. The
arithmetic did not fail; it wrapped, and physical `0x8000004000` was read as
`0x4000`, a page of firmware that answers every question wrongly rather than not
at all.

The top two gigabyte-blocks of the kernel's half are now not part of the
straight window: `mmu_map_device` points them wherever it is asked to. Two,
because one device may have its registers and its notification page in different
gigabytes, and a number small enough that running out is visible. The right
answer for a kernel that must map many such regions is a four-level walk and a
48-bit kernel half; that is a rewrite of everything above `mmu_go_high` for a
kernel that must map one.

`make test`: 241 checks, was 238, three consecutive runs.

## D-082 — writing, and a third party to say whether it worked

**2026-07-28.** `stam` writes to the disk. The shell redirects to an absolute
path and the bytes land on the medium:

```
verk$ hello written-by-the-kernel > /out.txt
verk$ upper < /out.txt
  [HELLO] ARGV[1] = "WRITTEN-BY-THE-KERNEL"
```

An absolute name means the disk; a relative one is still a memory file, as it
has been since D-060. One character, and it is the difference between a file
that survives the reboot and one that does not — which a program should be able
to say rather than have decided for it.

### The objection in D-079, answered rather than withdrawn

That entry declined to write FAT because vmkfs already implements the same
on-disk rules, and two implementations of one format is the arrangement D-042 is
about. That objection is still right. What makes the second implementation
acceptable is that **a third party judges them both**:

```
$ mdir -i stam.disk@@1M ::
OUT      TXT       333 1980-00-00   0:00
$ mtype -i stam.disk@@1M ::/out.txt
  [hello] argv[1] = "written-by-the-kernel"
$ fsck.fat -n esp-after.img
Free cluster summary uninitialized (should be 96611)
esp-after.img: 5 files, 137/96748 clusters
```

`make test` boots the kernel, has it write a file, waits for the shell to
finish, and then reads the image back with mtools and `fsck.fat`. Neither is
ours. Reading back what we wrote proves only that we are consistent with
ourselves; a kernel that wrote a plausible file into a filesystem it had
corrupted passes that and fails these.

Long names remain vmkfs's job. A name that does not fit 8.3 is refused here, by
name, rather than written with a checksum this code would be the only thing
computing.

### The free count says it does not know

FSInfo's free-cluster field is written as 0xFFFFFFFF, which the specification
defines as "not maintained" and every reader handles — `fsck.fat` says so in as
many words above, and finds nothing else wrong.

The first version counted the whole table on every close: about 750 sector reads
on a 48 MiB volume, in a driver that polls. It did not finish. **A number that
costs that much to keep true is a number that will be kept true only sometimes,
and a free count that is right on Tuesdays is worse than one that says it does
not know.** The next-free hint gets the same answer, and `clus_alloc` scans from
the start rather than trusting one — a stale hint is how two files come to share
a cluster.

### The flaky check, and what it was actually reporting

Three checks failed exactly once and passed on the seven runs after it. That is
the shape D-038 forbids shipping, so it was worth finding rather than re-running
until green.

The script had grown two commands and the wait for its `bye` had not. QEMU was
killed between writing `/out.txt` and reading it back — so mtools found a file
with no contents, which is a true report about a machine that was stopped, and
says nothing about the kernel that stopped writing to it.

Two changes, and neither is "wait longer" alone. The wait is longer, and **the
on-disk checks are skipped entirely when the shell never said `bye`**: judging a
volume the kernel was killed in the middle of writing is judging the kill.

`make test`: 246 checks, was 241.

## D-083 — no initrd

**2026-07-28.** `stam` boots with nothing handed to it but itself:

```
  initrd    none
  [kernel] no initrd; /init comes off the disk, 71608 bytes
verk$ hello world | upper
  [HELLO] ARGV[1] = "WORLD"
verk$ hello written-by-the-kernel > /out.txt
verk$ upper < /out.txt
  [HELLO] ARGV[1] = "WRITTEN-BY-THE-KERNEL"
  bye
```

Every program in that session came off the medium, and so did the script that
ran them. The kernel was started with `-kernel` and no `-initrd` at all.

This is what "booting from a disk" finally means. vboot has always *fetched* the
kernel from one — that is what a bootloader does — and until today the kernel
then depended on an archive the loader had put in memory for it. A kernel that
finds its userland by reading the medium is a different thing, and it is the
thing every step since D-079 was for.

The order is unchanged where both exist: the initrd wins. It is what the machine
was *started with*, and the disk is what it *found*; a boot whose behaviour
depended on what happened to be plugged in would be worse than either.

The image is read whole and the staging pages are returned once the loader has
copied it, the same as `execve` (D-079). Demand paging an executable needs a
page cache and a fault handler that can block, and neither exists.

### The check, and what it would catch

`make test` boots the same disk twice: once with the script initrd, and once
with no `-initrd` argument. The second asserts the line above — a boot that had
quietly found an archive prints something else — and then that the shell reached
`bye`, which is the `/rc` **on the disk** having run, because the vsh that read
it also came from there.

`make test`: 248 checks, was 246, three consecutive runs.

## D-084 — one variable for the whole machine, again

**2026-07-28.** `mmu.c` kept `cur_user_kva`: which user page table
`map_user_page` and `vm_cow_fault` are working in. One variable, for the whole
kernel.

That was true until D-078 made `current` per-processor, and then quietly was
not. Two cores in `execve` at the same time each wrote their pages into whatever
table the other had named last.

It presented as `elf_load` faulting on an address the program was entitled to —
sometimes a **permission** fault at `0x3f0000`, sometimes a **translation**
fault at `0x41ffe8`, depending on which core had written last and what it had
written. Nothing in either report mentioned a second processor.

### Why it hid

`cur_user_kva` is not in the scheduler and is not named by anything that looks
like scheduling. It is read from inside `execve` and from inside the fault
handler, neither of which mentions a CPU. D-078 went through every section
marked in `lock.h` and this one was never marked, because on one processor it
was not a race — it was a correct cache of a fact that only one thread at a time
could have.

And it only shows on a path that spends long enough between two mappings to be
preempted. Reading an executable out of memory does not. **Reading one off a
disk, through a driver that polls, does** — which is why D-079's work found a
defect that D-078 introduced, three commits later, in a report about neither.

### Per thread, not per processor

The obvious fix is to move it into `cpu_t`, and it is wrong: a thread may be
preempted between two mappings and resumed on a different core, and the answer
has to travel with the work rather than with the machine. So it lives in
`thread_t`, and `schedule_locked` swaps it through the `cpu_t` at every switch —
the CPU field is a cache, and the thread owns it.

### Two checks that came out of chasing it, and stay

Both were written to test theories that turned out to be wrong. Both are worth
keeping, because what they check is real and neither had anything watching it.

**A canary at the bottom of every kernel stack**, checked at every switch. The
stack was 8 KiB, which is not obviously enough for a call chain that carries a
512-byte argument buffer, and "it overflowed" was the first theory. It had not —
the canary has never fired — but nothing would have said so. The stack is 16 KiB
now as well, which is cheap and removes the question.

**A marker in every freed page**, checked when the allocator hands it out again.
"A live page was handed back" was the second theory. Also wrong, also unwatched:
a page written after it is freed is the quietest corruption there is, and this
turns it into a line of output at the moment it happens.

`make test`: 248 checks, unchanged — this fixes a defect nothing was catching,
which is worth saying plainly rather than counting.

## D-085 — directories, and three programs that only use the calls

**2026-07-28.** `stam` can list, make and remove directories on the medium:

```
verk$ mkdir /adir
verk$ ls /
  diskprog
  note.txt
  vboot.conf
  init
  hello
  upper
  rc
  OUT.TXT
  ADIR/
verk$ rm /gone.txt
```

`getdents64`, `mkdirat` and `unlinkat`, with Linux's numbers. `ls`, `mkdir` and
`rm` are ordinary programs with no privileges and nothing the kernel knows about
them — the same argument as the shell in D-049. A builtin would have proved
nothing about the interface.

`dirfd` is accepted only as `AT_FDCWD` and anything else is refused. There is no
working directory yet, so every path is absolute; ignoring the argument silently
would be a call that looks like it supports relative-to-a-directory and does not.

`rm` takes a file or an **empty** directory. Removing a tree means walking it,
and a walk that stopped half way would leave clusters belonging to a directory
that no longer exists — lost until something recounts the whole table.

A directory that fills up now grows by a cluster. Without that, the eighth file
in a root of sixteen slots failed with "no such file" — a true error code about
entirely the wrong thing.

### The bug this work found was not in this work

Three commits of it were spent chasing `elf_load` faulting on addresses the
program owned. That was D-084 — a variable the whole machine shared, introduced
by D-078, on main since then, and invisible until something spent long enough
between two page mappings to be preempted. The directory work found it by being
slow.

### The other thing that was slow

`clus_alloc` read a FAT sector **per cluster examined**. A sector holds a
hundred and twenty-eight entries, so it was paying for the same 512 bytes a
hundred and twenty-eight times, through a driver that polls. Not a wrong answer
— the same answer bought over and over — and it turned a boot that creates a few
files from seconds into minutes.

It is cached across the scan now, and the disk-only boot's check in `make test`
is given seven minutes rather than three. **That number is a driver limitation
and it is written down as one**: every sector is polled with interrupts masked,
so the machine stands still for the whole of every transfer. Interrupt-driven
block I/O is what removes it and is not written.

`make test`: 252 checks, was 248.

## D-086 — the interrupt arrives, and is not slept on

**2026-07-28.** The block driver's interrupt is found in the device tree,
enabled in the controller, and acknowledged in a handler. The line that says so
is printed **from the handler**, once:

```
  disk      virtio-blk over mmio v1, 131072 sectors (64 MiB), interrupt-driven
  disk      the device's interrupt arrived; requests sleep now
```

A line at set-up time would have said the same thing whether or not the device
ever raised anything, which is this log's recurring defect. This one cannot
appear unless an interrupt was taken.

### And then it is polled anyway

The first version blocked the requesting thread on the completion and woke it
from the handler. On two processors it worked, and it took the shell's boot from
over five minutes to **ten seconds**. On one processor it stopped at the first
*write*.

A wakeup that does not come is a thread that sleeps for ever, and this kernel
has no timeout to bound that with — no timer wheel, nothing that can decide a
sleeper has waited long enough. **A sleep with no floor under it is worse than a
spin**, so the sleep came out and the poll stayed.

What did not go back is the mask. The wait runs with interrupts *enabled* now,
so the timer ticks and the console is read while the device works — which was
most of what standing still cost. The single-processor boot that would not
finish in seven minutes finishes in about one.

Why one processor and not two was not established. The honest suspects are the
requesting thread holding the queue lock across the sleep, and a completion
raised while the waiter was between "not done" and "asleep" — the lost-wakeup
window the console reader closes by holding one lock across both, which this
does too, but through a second lock as well. Two locks and one order is a claim
that needs checking rather than repeating.

### What would make the sleep safe

A bounded sleep: a thread that has waited more than some number of ticks is made
runnable whether or not anything woke it, and re-checks its own condition. That
is a scheduler feature and not a driver one, and every blocking path here would
be safer for having it.

`make test`: 253 checks, was 252 — one more, and it is the one that says the
interrupt was taken.

## D-087 — a floor under a sleep, and the thing it did not fix

**2026-07-28.** D-086 took the disk's sleep out because a wakeup that never
comes is a thread that is lost, and this kernel had nothing to bound that with.
It has now: `thread_block_on_pipe_until(object, ticks)` sets a deadline, and
`schedule_locked` makes any blocked thread runnable once its deadline has
passed, whether or not anything woke it. The thread re-tests its own condition
either way, which it had to do regardless.

The wake-up scan lives in `schedule_locked` because every scheduling decision
passes through it, and an idle core reaches it on every timer interrupt — so a
deadline is never more than a tick late, without a timer wheel or a sorted list
to keep honest.

`thread_wake_pipe` clears the deadline when it wakes someone, so a thread that
was woken properly does not carry a stale one into its next sleep.

### And it did not fix the disk

With the floor under it, the driver's sleep was put back. **A single-processor
boot still stops at the first write.** Which settles one thing: the thread is
not in the sleep, because the deadline would have ended it. The cause is
somewhere this has not looked, and two processors do not show it.

So the sleep came out again and the poll stayed. Shipping a driver that sleeps
on two processors and stops on one would be worse than one that spins on both,
and "it works on my machine" is a claim about the machine.

The bounded sleep stays. It is right on its own terms — every blocking path in
this kernel is safer for a floor under it — and it is the thing that made the
second attempt worth trying rather than a repeat of the first.

`make test`: 253 checks, unchanged.

## D-088 — a machine that has stopped now says so

**2026-07-28.** A core with nothing to run while something is unfinished used to
stop silently. It prints the thread table instead:

```
  [kernel] cpu 0 has had nothing to run for five seconds:
    boot     state 0 reason 3 object ffffff804008f0b0 wake_at 0 now 1081
    idle-0   state 4 reason 0 object 0                wake_at 0 now 1081
    initrd   state 2 reason 1 object ffffff804008f0b0 wake_at 0 now 1081
    child    state 0 reason 3 object ffffff804008f0b0 wake_at 0 now 1081
```

That is the disk sleep of D-086 and D-087, at last saying something specific:
**no thread switch for five seconds, with two threads standing runnable.** The
sleeping thread is not the problem — `boot` and `child` are `THREAD_RUNNABLE`
and nothing picks them. That points at the scheduler's choice, not at the
sleep, and it is where this continues.

The sleep stays out until then. What all three attempts did leave behind is the
mask: the driver's wait runs with interrupts enabled, so the timer ticks and the
console is read while the device works.

### The report was wrong before it was right

The first version counted its own passes through the idle loop. Every thread
that runs and blocks again comes back here, so a busy machine and a stopped one
look identical from inside it — and it duly reported a stopped machine that was
merely idle between shell commands. **A report that does not depend on what it
reports**, written while chasing one. It counts thread switches now, anywhere,
by any processor.

The second version called `threads_alive()` on every timer interrupt to decide
whether to care. Walking the table sixteen times a second was enough to push the
shell's boot past the check that waits for it — a diagnostic that changed the
thing it was diagnosing. It counts first and looks only at the threshold.

`make test`: 253 checks, unchanged.

## D-089 — the chooser chooses, and nothing moves

**2026-07-28.** D-088's report said no thread switch happened for five seconds
while two threads stood runnable. The obvious suspect was `next_runnable` — so
the report now asks it directly, from inside the same locked section, and prints
what it says:

```
    slot 0 boot   state 0 home -1 ...
    slot 1 idle-0 state 4 home  0 ...
    slot 7 child  state 0 home -1 ...
    chooser: n_threads 11, current is slot 1, next_runnable says child
```

**It says `child`.** A runnable thread, found, named — and no switch happens.

That is worth more than three theories: the defect is not in choosing. It is
between the choice and `cpu_switch`, or in `cpu_switch` itself, or in the
`switches` counter that reports it — and it is nowhere near the disk driver
that three entries have now been written about.

Asking the loop instead of reasoning about it took one run. Reasoning about it
took three (D-086, D-087, D-088), and each was wrong in a way that looked
plausible. This project's own note says the cheapest next step when a thing does
not work is almost never to test one explanation — it is to make the program say
where it got to (D-071, D-081).

The disk sleep stays out. `make test` is 253 checks and green without it; the
driver polls with interrupts enabled, which is where D-086 left it.

### What the next session should do

Print, from inside `schedule_locked` on the path where it does *not* switch,
which of its four early returns it took. There are exactly four and one of them
is being taken; naming it is a line of code and ends this.

## D-090 — the chooser's answer, and where this stops for now

**2026-07-28.** D-089 promised one line: record which of `schedule_locked`'s
four exits is taken on a machine that has stopped. It was written, and the
answer was **0** — none of them. The variable was never assigned, which on a
machine that has booted and switched threads thousands of times cannot be true
of the function they all live in.

So either `schedule()` returns before reaching `schedule_locked`, or the
variable being read is not the one being written. Both are testable and neither
was tested: the instrumentation itself made `make test` unstable — two runs in a
row failed different SMP checks — and a diagnostic that changes what it
diagnoses is the second time that happened today (D-088). It was reverted.

### Where this stands, honestly

Four entries have now been written about a disk sleep that does not work on one
processor, and the useful part of all four is that the problem is **not in the
driver**:

| | |
|---|---|
| D-086 | the sleep hangs a single-processor boot; no timeout to bound it |
| D-087 | a bounded sleep did not help — so the thread is not in the sleep |
| D-088 | no thread switch at all, with two threads runnable |
| D-089 | `next_runnable` *does* name a runnable thread |
| D-090 | and `schedule_locked` appears never to reach any of its exits |

Each step was a real narrowing and each was bought by asking the machine rather
than reasoning. What is left is one question with two candidate answers, and it
belongs in a session with room to test them rather than at the end of one.

The disk driver polls, with interrupts enabled. `make test` is 253 checks and
green. Nothing in the tree depends on the sleep.

## D-091 — `started` is being cleared, and nothing writes it

**2026-07-28.** D-090 ended with `last_exit == 0`: `schedule_locked` appeared
never to run. The disassembly settles it, and the answer is not about the
scheduler.

`sched_idle_loop` inlines `schedule()`, and there is exactly one path through it
that reaches `wfi` again without calling `schedule_locked`:

```
40085c88:  ldr  w0, [x27, #12]
40085c8c:  cbz  w0, 40085c70      <- back to wfi, schedule_locked skipped
40085c90:  mrs  x21, daif          <- otherwise: take the lock and schedule
```

And `sched_init` writes that same word:

```
40085b34:  str  w1, [x6, #12]      x6 == x27
```

So `[x27, #12]` is `started`, and **`started` is zero** on a machine that has
booted and run thousands of threads. Nothing in the kernel writes it except
`sched_init`, and that writes 1.

### Which makes this a memory bug, not a scheduler bug

`started` is a `static int` in `sched.c` with `switches` four bytes after it.
Something is writing over them. That is a much better explanation of today than
five separate scheduler theories were:

- a disk sleep that hangs on one processor and not two (D-086)
- a bounded sleep that changed nothing (D-087)
- a machine with runnable threads and no switch (D-088)
- a chooser that names a thread nobody takes (D-089)
- and a function that appears never to run (D-090)

All five are what a cleared `started` looks like from five angles.

### What to do with it

Two checks already exist and neither covers this: the kernel stack canary
(D-084) has never fired, and the freed-page marker (D-084) has never fired. So
the writer is neither a stack overflow nor a use-after-free of a whole page — it
is an out-of-bounds write into `sched.c`'s statics, most likely past the end of
an array near them.

The cheapest next step is the same one that worked five times today: **make the
program say it.** A canary word placed either side of `started` in the same
translation unit, checked where `started` is read, names the moment. After that
the writer is found by which arrays neighbour it in the object file.

The disk sleep is not the bug and never was. It is the workload that made the
corruption reachable.

## D-092 — D-091 was wrong, and the guards say so

**2026-07-28.** D-091 concluded that `started` was being cleared by an
out-of-bounds write, and named that as the explanation for five earlier entries.
The recommended next step was to put a guard word on each side of it and see.

That was done. **Neither guard was ever disturbed**, and the machine still
stopped in the same place. So `started` is not being overwritten and D-091's
conclusion is withdrawn.

What it was really built on was `last_exit == 0`, and that reading is now the
suspect — the third diagnostic today to have been wrong about the thing it was
watching (D-088 twice, this once). The disassembly in D-091 is still correct
about *where* the only skip is; what is not established is that the skip is
being taken.

### Six entries, and what they have actually ruled out

| | |
|---|---|
| D-086 | the sleep hangs a single-processor boot |
| D-087 | a bounded sleep changes nothing — the thread is not in the sleep |
| D-088 | no thread switch at all, while two threads stand runnable |
| D-089 | `next_runnable` names one of them |
| D-090 | an exit counter reads as though `schedule_locked` never runs |
| D-091 | the only path that skips it needs `started` to be zero |
| D-092 | and `started` is not being written over |

Every one of these was bought by asking the machine. Five were useful. Two of
the answers — D-090's counter and D-091's inference from it — were the
instrument lying, and both cost more than they returned.

### The lesson that is worth more than the bug

**A diagnostic is code, and today four of them were wrong**: an idle report that
counted the wrong thing, one that changed what it measured, an exit counter that
read zero when it could not have been, and an inference built on top of it. The
project's rule is *ask the machine rather than reason about it* — and the
correction is that a machine can only answer for what the instrument actually
measures, which is itself a thing to be checked.

Nothing here is committed to the kernel. `make test` is 253 checks and green
without any of it; the driver polls with interrupts enabled. The next session
starts from a clean tree and this list of what is ruled out.

## D-093 — the instrument lies, and now it is proved

**2026-07-28.** D-092 suspected the exit counter and withdrew a conclusion built
on it. This time the instrument checks itself: a counter incremented as the
first statement of `schedule_locked`, beside the one that names which exit it
took.

```
    chooser: calls 12097 exit 0  (calls>0 with exit 0 is impossible: the instrument lies)
```

Twelve thousand calls, and the exit is never set. Every path out of that
function assigns it. **This is not an inference — it is a contradiction, and it
was measured.**

So `schedule_locked` runs, and two `static volatile` counters in the same file,
written by it and read a few lines away, disagree about what happened.

### Which makes it the fourth of its kind

D-037 found a thread starting at a link-time address. D-039 found it again in a
user program. D-076 found it as a processor started at an address that no longer
existed. D-081 found it wearing the shape of a table of function pointers. Each
was the same thing: **this kernel is position-independent and applies no
relocations, so anything routed through the GOT holds the address the image had
when it was linked.**

A `static volatile` written in one function and read in another is not obviously
that. But `-fpic` with hidden visibility does not guarantee the two accesses are
compiled the same way, and a counter that reads zero after twelve thousand
increments is exactly what two addresses for one variable looks like.

That is a hypothesis and it is the first one all day with a measured
contradiction under it rather than a plausible story. It is also checkable
without running anything: `objdump` the two accesses and compare how each
computes the address.

### What this cost, and what it bought

Eight entries about a disk sleep that was never the bug. Six theories, two of
them the instrument lying about itself. The useful residue:

- the driver's wait no longer masks interrupts, which took a boot from minutes
  to about a minute
- the scheduler has a bounded sleep, which every blocking path is safer for
- a core that has stopped prints the thread table
- and `make test` is 253 checks and green with none of the chase in it

**A diagnostic is code.** Four were wrong today, and the one that finally said
something trustworthy was the one built to contradict itself if it were lying.
That is worth more than the bug it is still chasing.

## D-094 — not the GOT either, and that is five

**2026-07-28.** D-093 proposed that `sched_calls` and `sched_exit` resolve to two
different addresses — the trap of D-037, D-039, D-076 and D-081 in a new shape.
It is checkable with `objdump` and it is wrong.

Both sides compute the same base:

```
schedule_locked:   adrp x20, 40091000 ; add x19, x20, #0xee0
                   str  w0, [x19, #8]      <- sched_calls  (0x40091ee8)
                   add  x20, x20, #0xee0
                   str  w1, [x20, #12]     <- sched_exit   (0x40091eec)
sched_idle_loop:   adrp x8,  40091000 ; add x27, x8,  #0xee0
```

`nm` agrees: `sched_calls` at `0x40091ee8`, `sched_exit` at `0x40091eec`. One
address each, computed the same way in both functions. No GOT, no second copy.

So the contradiction in D-093 — twelve thousand calls, exit never set — is real
and unexplained, and the explanation offered for it was wrong.

### That is five wrong answers in a row about one thing

D-088's report counted the wrong thing. D-088's second version changed what it
measured. D-090's counter read a value it could not have. D-091 inferred a
memory bug from it and D-092 withdrew that. D-093 inferred a relocation bug and
this withdraws that.

The measurement in D-093 is the only part still standing, and it stands because
it was built to contradict itself if it lied. Everything built *on top of* it
has been wrong, which is its own lesson: **a trustworthy measurement does not
make the next inference trustworthy.**

### Where a session with room should start

Not with another theory. With the four instructions between `sched_calls++` and
the first `sched_exit =`, read in the disassembly — there are not many, and one
of them returns. The report's own loads are at offsets 4 and 20 of the same
base, which are neither counter, so what the `kprintf` actually reads has not
been established either and should be, first.

`make test`: 253 checks, green, with none of this in the kernel.
