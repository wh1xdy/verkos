# VerkOS build architecture

VerkOS is built in ordered **stages**, mirroring the Linux From Scratch method but
generalised to three architectures. Each stage is an idempotent-ish script under
`scripts/`, numbered so the order is obvious. The orchestrator (`scripts/build.sh`,
or `make`) runs them in sequence for a chosen target.

## The big picture

```
        host tools (your laptop: gcc, make, bash, ...)
                          │
                          ▼
 ┌────────────────────────────────────────────────────────────┐
 │  Stage 0  fetch-sources     download + verify all tarballs   │
 └────────────────────────────────────────────────────────────┘
                          │
                          ▼
 ┌────────────────────────────────────────────────────────────┐
 │  Stage 1  cross-toolchain   binutils → gcc(pass1) →          │
 │                             glibc → libstdc++ → gcc(pass2)   │
 │           output: $TOOLS/  a compiler that targets $ARCH     │
 └────────────────────────────────────────────────────────────┘
                          │
                          ▼
 ┌────────────────────────────────────────────────────────────┐
 │  Stage 2  temp-tools        cross-build a minimal set of     │
 │                             utilities into the target rootfs │
 │                             (m4, ncurses, bash, coreutils…)  │
 └────────────────────────────────────────────────────────────┘
                          │
                          ▼
 ┌────────────────────────────────────────────────────────────┐
 │  Stage 3  chroot / qemu     enter the target rootfs.         │
 │                             native arch → chroot             │
 │                             foreign arch → qemu-user + chroot │
 └────────────────────────────────────────────────────────────┘
                          │
                          ▼
 ┌────────────────────────────────────────────────────────────┐
 │  Stage 4  final-system      build the real system natively:  │
 │                             glibc, gcc, coreutils, bash,     │
 │                             util-linux, systemd, D-Bus, …    │
 └────────────────────────────────────────────────────────────┘
                          │
                          ▼
 ┌────────────────────────────────────────────────────────────┐
 │  Stage 5  kernel            configure + build Linux for $ARCH │
 └────────────────────────────────────────────────────────────┘
                          │
                          ▼
 ┌────────────────────────────────────────────────────────────┐
 │  Stage 6  image             assemble initramfs + (later)     │
 │                             GRUB disk image                  │
 └────────────────────────────────────────────────────────────┘
                          │
                          ▼
                     boot in QEMU  (boot/qemu-$ARCH.sh)
```

## Directory contract

Everything a build produces lives under a per-target work directory so builds for
different arches never collide:

```
$WORK/$ARCH/
├── sources/      # downloaded tarballs (shared via symlink where possible)
├── tools/        # the cross-toolchain for this arch
├── rootfs/       # the target system being assembled
├── build/        # scratch build directories (deleted as we go)
└── out/          # final artifacts: kernel image, initramfs, disk image
```

`$WORK` defaults to `./work` (git-ignored). Override with `WORK=/path make ...`.

## Per-architecture settings

`config/targets/<arch>.sh` defines the moving parts for each architecture:

| Variable         | x86_64            | i686              | aarch64             |
|------------------|-------------------|-------------------|---------------------|
| `VERK_TARGET`    | `x86_64-verk-linux-gnu` | `i686-verk-linux-gnu` | `aarch64-verk-linux-gnu` |
| `VERK_ARCH`      | `x86_64`          | `i686`            | `aarch64`           |
| `LINUX_ARCH`     | `x86`             | `x86`             | `arm64`             |
| `KERNEL_IMAGE`   | `arch/x86/boot/bzImage` | `arch/x86/boot/bzImage` | `arch/arm64/boot/Image` |
| `QEMU_BIN`       | `qemu-system-x86_64` | `qemu-system-i386` | `qemu-system-aarch64` |
| `QEMU_USER`      | `qemu-x86_64`     | `qemu-i386`       | `qemu-aarch64`      |
| `GCC_FLAGS`      | (baseline)        | `--with-arch=i686` | `--with-arch=armv8-a` |

The `-verk-` vendor field in the target triplet is a deliberate marker: it makes
"is this compiler ours?" trivially greppable and forces the toolchain to use our
sysroot rather than the host's.

## Native vs. foreign builds

- **Native** (building x86_64 on an x86_64 host): Stage 3 is a plain `chroot`.
- **Foreign** (building aarch64 on an x86_64 host): Stage 3 registers the target
  with `binfmt_misc` and copies a static `qemu-user` binary into the rootfs, so
  `chroot` transparently runs foreign binaries. Slower, but one host builds all
  three arches.

`scripts/lib/common.sh` detects host arch and picks the right mode automatically.

## Why numbered scripts instead of one big Makefile

The Makefile is a thin convenience layer. The real logic lives in readable,
sequential shell scripts so the build reads like the LFS book: you can open
`scripts/20-cross-toolchain.sh` and see exactly which package is configured with
which flags, in order. That transparency is the whole point of building this way.
