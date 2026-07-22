# Building VerkOS

This walks through building VerkOS **on your own machine** (Linux host
recommended). A full glibc+systemd build takes **hours** and needs **15–25 GB**
of free disk per architecture, so don't run it in a throwaway container.

> When Claude is helping and hits a step that needs your laptop (heavy compile,
> real hardware, `sudo`), it will stop and tell you the exact command to run.

## 1. Host requirements

You need a reasonably modern Linux host with a working toolchain. Check with:

```sh
make check          # runs scripts/00-check-host.sh
```

This verifies you have (at minimum): `gcc`, `g++`, `make`, `bash`, `ld`,
`bison`, `flex`, `gawk`, `texinfo`, `python3`, `xz`, `wget`/`curl`, `qemu`, and
enough free disk. It prints what's missing and the install command for your
distro.

For **foreign-architecture** builds (aarch64/i686 on an x86_64 host) you also
need `qemu-user-static` and `binfmt_misc` support (most distros ship this as the
`qemu-user-static` package).

## 2. Fetch sources

```sh
make fetch
```

Downloads every upstream tarball listed in `config/versions.sh` into
`work/sources/` and verifies each against its recorded SHA-256. If a checksum
mismatches, the build refuses to continue — we never build unverified source.

## 3. Build

```sh
make ARCH=x86_64            # full build for x86_64 (default)
make ARCH=aarch64           # 64-bit ARM
make ARCH=i686              # 32-bit x86
```

Or run a single stage (useful while iterating / debugging):

```sh
make toolchain ARCH=aarch64 # just the cross-toolchain
make kernel   ARCH=x86_64   # just the kernel
make system   ARCH=x86_64   # just the final userland
make image    ARCH=x86_64   # just (re)assemble the bootable image
```

Stages are ordered; later stages assume earlier ones ran. `make` tracks
per-stage stamp files under `work/$ARCH/.stamps/` so re-running skips completed
stages. Force a rebuild of a stage with e.g. `make kernel ARCH=x86_64 FORCE=1`.

## 4. Boot it

```sh
make run ARCH=x86_64        # boots work/x86_64/out in QEMU
```

Under the hood this calls `boot/qemu-$ARCH.sh`, which does a **direct kernel
boot** — QEMU loads the kernel and initramfs with no bootloader. You should land
at a VerkOS login/shell. To quit QEMU: `Ctrl-A` then `X`.

## 5. Clean up

```sh
make clean ARCH=x86_64      # remove build scratch, keep sources + toolchain
make distclean              # remove everything under work/ (including sources)
```

## Troubleshooting

- **"No space left on device"** — each arch needs 15–25 GB. `make clean` frees
  the scratch `build/` dirs without forcing a toolchain rebuild.
- **A package fails to configure** — open the matching `scripts/NN-*.sh`; every
  package's exact flags are there in order, so you can reproduce the step by hand.
- **Foreign build hangs / "exec format error"** — `binfmt_misc` isn't set up.
  Install `qemu-user-static` and re-run; `scripts/lib/common.sh` re-registers it.
- **Checksum mismatch on fetch** — upstream re-rolled a tarball or a mirror is
  bad. Verify the new hash manually, then update `config/versions.sh`.

## Bumping a package version

All versions live in one place: `config/versions.sh`. To bump one, change its
version and SHA-256 there, then `make fetch` again. Nothing else references
version numbers directly.
