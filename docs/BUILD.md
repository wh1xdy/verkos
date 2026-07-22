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

You also need a host **`pip`** (`pip3`): `make fetch` uses it to download the
sdists for systemd's build tools (meson, jinja2, markupsafe) into
`sources/pip/`, so the chroot can install them offline. See decision D-008.

### Building on macOS (Colima / Docker) — read this

You cannot build VerkOS *on* macOS (no Linux kernel to chroot into, no
`binfmt_misc`, Darwin's clang/ld aren't the GNU/ELF toolchain). Run the build
inside a Linux VM. On Apple Silicon, Colima gives you a native `arm64` Linux VM,
so `ARCH=aarch64` builds with no emulation:

```sh
colima start --arch aarch64 --cpu 12 --memory 16 --disk 80
docker run -d --privileged --name verk -v "$PWD":/os -w /os \
    -v verkwork:/os/work debian:bookworm sleep infinity   # NOTE the volume ↓
```

> ⚠️ **`work/` MUST live on a case-sensitive Linux filesystem.** macOS's APFS is
> case-*insensitive*, and the Linux kernel source contains files that differ
> only in case (e.g. `xt_CONNMARK.h` vs `xt_connmark.h`) — extracting it onto a
> macOS-backed bind mount fails with `tar: Cannot open: Permission denied` and
> `Documentation/Kbuild: Is a directory`. Mount a Docker **volume** at
> `/os/work` (as above) so the build's scratch/extract tree is on the VM's ext4,
> not the bind-mounted host path. The repo itself can stay bind-mounted; only
> `work/` is sensitive.

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

## Pinning checksums

`config/versions.sh` records a SHA-256 for every source tarball. When one is
empty, `make fetch` downloads the file, records the computed hash into
`config/versions.lock`, and warns; a pinned hash makes the build *refuse* any
tarball that doesn't match.

To fill in (or refresh) the hashes automatically:

```sh
scripts/pin-hashes.sh                 # download each source, compute + write its hash
scripts/pin-hashes.sh --dry-run       # show hashes without editing versions.sh
scripts/pin-hashes.sh --only gcc,zlib # re-pin just these packages
```

The tool doubles as a version/URL validator — a failed download means the
version or URL in `versions.sh` is wrong. It runs on Linux or macOS and retries
with backoff to ride out mirror rate limiting.

> **Status:** all 35 source tarballs are pinned with verified SHA-256 checksums.
> `MESON` is intentionally unpinned — it's installed as a pip sdist, not fetched
> as a tarball. To bump a package, change its version and clear its `*_SHA256`,
> then re-run `pin-hashes.sh --only <pkg>`.

## Bumping a package version

All versions live in one place: `config/versions.sh`. To bump one, change its
version and SHA-256 there (or clear the SHA and re-run `pin-hashes.sh`), then
`make fetch` again. Nothing else references version numbers directly.
