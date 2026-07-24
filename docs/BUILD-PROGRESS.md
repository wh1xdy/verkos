# Build progress — VerkOS is booting, online, and self-hosting

Live status of the VerkOS build. The first end-to-end build was done on an Apple
Silicon Mac (M-series) inside a native arm64 Linux VM, targeting **aarch64** —
that arch is now a **complete, bootable, networked, self-hosting OS**. A second
target, **x86_64**, is being built natively on a real x86 Linux server, and has
reached a fully installed system (bugs found and fixed along the way, below).

## Where things stand

- **aarch64** — done end to end: cross-toolchain → temp-tools → full system →
  kernel → ext4 image → **boots to a login shell**, comes **online over DHCP**,
  is **reachable via SSH**, runs its **own package manager (vpk)** and its **own
  core utilities (verkbox)**, and greets you with **verkfetch**.
- **x86_64** — building natively on an x86 server: toolchain, temp-tools and the
  full system are done; kernel + image in progress. Three real x86-only bugs were
  found and fixed (glibc `ld-linux` ELOOP, kernel `objtool`/libelf, image ext4
  `sudo`) — see "Multi-arch" below.

## Environment (how to resume)

**aarch64 (Apple Silicon host):**

- **Colima VM**: `colima start` (configured 16 CPU / 20 GB / 80 GB disk).
- **Build container**: `verkbuild` (Debian bookworm, privileged), image
  `verkbuild-img` (snapshot with all apt build-deps installed).
- **work/ on an ext4 Docker volume** `verkwork` mounted at `/os/work` — because
  macOS APFS is case-insensitive and the kernel source won't extract on it
  (see docs/BUILD.md → "Building on macOS"). The repo is bind-mounted at `/os`.

```sh
colima start && docker start verkbuild
docker exec verkbuild make system ARCH=aarch64   # continues; per-package stamps
                                                 # skip finished work
make run ARCH=aarch64                            # boot in QEMU on the Mac host
```

**x86_64 (native x86 Linux server):** a case-sensitive filesystem and the host
build tools (incl. `libelf`/`gelf.h` for the kernel's objtool). Runs as a normal
user; stage 60 uses `sudo` for the image steps that need it.

```sh
make ARCH=x86_64      # check → fetch → toolchain → temp → system → kernel → image
make run ARCH=x86_64  # boot in QEMU
```

## Stages (aarch64)

- [x] `make check` — host prerequisites (now also requires libelf/`gelf.h`)
- [x] `make fetch` — all sources downloaded + SHA-256 verified; pip sdists staged
- [x] `make toolchain` — cross binutils/gcc/glibc/libstdc++ (aarch64-verk-linux-gnu)
- [x] `make temp` — full LFS ch.6 temporary tools
- [x] `make system` — full boot-critical userland + systemd 256.7 as `/usr/sbin/init`
- [x] `make kernel` — Linux 6.12.9 arm64 (vmlinuz, ~44M)
- [x] `make image` — ext4 root disk image (stage 60 builds a disk image, not a
      multi-GB initramfs; the multi-GB rootfs made initramfs impractical)
- [x] 🎯🎉 **FULL BOOT** — `make run ARCH=aarch64` boots Linux 6.12.9, mounts the
      ext4 root, runs **systemd 256.7 as PID 1**, reaches Multi-User with
      `systemctl is-system-running` → **running, 0 failed**, and lands on a
      `root@verkos:~#` shell (serial-getty autologin).

## Userland, networking, and "make it ours"

- [x] **Full userland** (`VERK_FULL_USERLAND=1`): gcc, binutils, bison, flex,
      sed, grep, gawk, tar, … all built natively into the rootfs, so VerkOS can
      compile software on itself.
- [x] **Working login**: shadow 4.16.0 gives `/bin/login` + `passwd`; root
      password set (dev default `verkos`); serial-getty autologin drops boot
      straight to a root shell. Linux-PAM 1.6.1 built; systemd rebuilt with
      `-Dpam=enabled` (pam_systemd.so under `/usr/lib/security`); `/etc/pam.d`
      stack shipped.
- [x] **Networking + SSH** — VerkOS goes online and is reachable:
      - **dhcpcd 10.1.0** is the active DHCP/DNS client (chosen over
        systemd-networkd as a first step toward replacing systemd bits; networkd
        is built but left disabled as a swappable option).
      - **OpenSSL 3.4.0 + OpenSSH 9.9p1**: sshd runs; root login via password
        (verkos) and an ed25519 key. Verified: SSH'd in over the QEMU hostfwd
        (2222→22), `eth0: up`, DNS resolves (getent hosts one.one.one.one →
        Cloudflare), outbound works.
- [x] **Userland tools** — iproute2 (ip/ss), iputils (ping), curl, procps-ng
      (ps/top/free), less, nano; plus bison/flex. Verified live over SSH:
      `eth0 UP 10.0.2.15/24`, ping 0% loss, `curl -sI http://example.com` →
      200 OK, ps/free work.
- [x] **verkfetch** — VerkOS's own neofetch: pure POSIX sh + `/proc`, no deps,
      cyan→purple ASCII logo (user is refining the art). Greets interactive
      logins.

### verkbox — VerkOS's own core utilities

- [x] **verkbox** (pkg/verkbox/verkbox.c): a single BusyBox-style multi-call
      binary, plain C, no dependencies, that dispatches on `argv[0]` (or
      `verkbox <applet>`). **33 applets**: true, false, echo, cat, pwd, whoami,
      nproc, yes, basename, dirname, head, wc, seq, ls, tail, cut, rev, uniq,
      mkdir, rmdir, rm, cp, mv, ln, touch, sort, tr, nl, tac, tee, fold, comm,
      paste.
- [x] **Per-tool takeover of GNU coreutils**: applets ship alongside GNU and are
      byte-exact differential-tested against coreutils; once an applet is
      flag-complete and passes, it *replaces* GNU for that tool name. **Five now
      shadow GNU: `ls`, `cat`, `head`, `tail`, `wc`** (symlinked over
      `/usr/bin`). The rest stay callable but GNU-backed until they graduate —
      done last in the build so a bug can't break boot.

### vpk — VerkOS's own package manager

- [x] **vpk** (pkg/vpk/vpk.c): a self-contained, source-based (ports/Gentoo-style)
      package manager in C. One binary — it links libcurl (fetch), liblzma + zlib
      (decompress) and implements SHA-256, tar extraction, recipe parsing,
      recursive dependency resolution and the install DB itself. The only child
      it spawns is a recipe's own `build()`.
      Commands: `install | remove | list | info | build | update | search`
      (`verk` alias). Ships a CA bundle so HTTPS works.
- [x] **Source + binary packages**: `vpk build` produces a gzipped-ustar `.vpk`
      of the DESTDIR; `vpk install` uses a cached `.vpk` if present (verified
      against a sha256 sidecar before it's unpacked into `/` as root), else
      builds from source and caches one, reclaiming build scratch afterward.
- [x] **Remote recipes**: recipes are fetched from the GitHub repo over HTTPS
      when not shipped locally, so `vpk install <app>` works without rebuilding
      the OS; repo URL overridable via `/etc/vpk/repo`
      (default `raw.githubusercontent.com/wh1xdy/verkos/main`).
- [x] **Recipe ecosystem**: `scripts/gen-recipes.sh` generates recipes from the
      build's own data (version/source/sha from versions.sh; build()/depends from
      a table) and the `INDEX` for `vpk update`/`search`. **17 recipes shipped**
      (11 base ports + git/vim/tree/htop/wget + hello).
- [x] **Deep integration**: the **entire base system is registered in vpk's db**
      (each tagged `[base]`), so `vpk list` shows the OS, and vpk refuses to
      remove base packages.
- [x] **Verified on VerkOS itself**: `vpk install hello`, `htop`, and `wget` all
      build from source on the running system — dependency resolution skips
      already-installed base deps (ncurses/openssl/zlib), fetches over HTTPS,
      verifies sha256, builds with our gcc, installs, and runs.
- [x] **Security-hardened** (a multi-agent audit of vpk.c — which runs as root
      and extracts downloaded tarballs — confirmed 24 issues; the exploitable set
      is fixed and verified against an adversarial tarball):
      - **Path traversal** rejected: tar members that are absolute or contain a
        `..` component are refused (was arbitrary root file-write).
      - **Symlink write-through** blocked: parents built without descending
        through symlinks; regular files opened `O_NOFOLLOW`.
      - **ustar header checksum** validated; corrupt/garbage headers rejected.
      - **Hardlink** entries (type '1') now extracted (link/copy) instead of
        silently dropped.
      - **Dependency resolution** used non-reentrant `strtok()` while recursing
        (silently dropped every package's 2nd+ dep) → `strtok_r()`.
      - **Package names** validated (no `/`, no `..`) before use in cache
        paths + URLs; `download()` restricts protocols to http/https and writes
        atomically (`.part` + rename).
      - **Cached `.vpk`** verified against a sha256 sidecar before unpack; a
        tampered archive aborts the install.
      - **`is_installed()`** checks the db `meta` file (written last), so a
        half-finished install is retried, not counted done forever.
      - **`cmd_remove`** refcounts files, so removing one package can't delete a
        file another (or a base) package also owns.
      - Deferred (tracked): repo/recipe signing (recipes run as root).

### Boot-health fixes (verified live, `is-system-running` → running, 0 failed)

- [x] **logind** — the real cause was a missing D-Bus system bus, not PAM. Our
      dbus is built `-Dsystemd=disabled` (enabling it pulls glib), so logind
      looped on "Failed to connect to system bus" until we shipped a dbus system
      bus ourselves.
- [x] **dbus** — first attempt used a socket-activated unit with
      `--systemd-activation`, which *requires* systemd support dbus wasn't built
      with, so dbus-daemon exited "compiled without systemd support" and came up
      degraded. Fix: run dbus-daemon as a plain `Type=simple` service that
      creates/listens on `/run/dbus/system_bus_socket` itself; enable
      `dbus.service` via multi-user.target; create the service users its policy
      files reference. Verified: the bus answers `org.freedesktop.DBus.ListNames`.
- [x] **vconsole** — `/dev/tty0` exists in the QEMU virt console, so systemd ran
      systemd-vconsole-setup, which calls `loadkeys` from kbd; the minimal base
      ships no kbd, so the unit failed and systemd stayed degraded. Fix: a drop-in
      `ConditionPathExists=/usr/bin/loadkeys` cleanly *skips* the unit until kbd
      is installed.

## Multi-arch (x86_64) — building natively on a real x86 server

The earlier blocker was emulation: building x86_64 on Apple Silicon meant running
x86 binaries under qemu-user (cc1 segfaults) or Rosetta (configure hangs on
zombie gcc). **Decision, now acted on:** run the x86_64 build where it's native —
on a real x86_64 Linux server, where it builds reliably at full speed.

- [x] `make check ARCH=x86_64` (now also checks for libelf/`gelf.h`)
- [x] `make fetch ARCH=x86_64`
- [x] `make toolchain ARCH=x86_64` — cross binutils/gcc/glibc/libstdc++
- [x] `make temp ARCH=x86_64` — full LFS ch.6 temporary tools
- [x] `make system ARCH=x86_64` — full native system built on the server
- [ ] `make kernel ARCH=x86_64` — Linux 6.12.9 bzImage (in progress)
- [ ] `make image ARCH=x86_64` → boot

**x86-only bugs found and fixed by actually building it:**

1. **glibc `ld-linux` ELOOP** — the toolchain pre-created
   `/lib64/ld-linux-x86-64.so.2 → ../lib/…`, but in our merged-/usr layout
   `/lib64` is a symlink to `usr/lib`, so this wrote a self-referential loop and
   glibc's `make install` died with "Too many levels of symbolic links". The x86
   toolchain glibc never installed (aarch64 was unaffected — it uses `/lib`, no
   `/lib64` split). Fix: drop the manual symlink; glibc installs the real ld.so
   into `/usr/lib` and the interpreter path resolves there via the `/lib64` link.
2. **kernel objtool / libelf** — `tools/objtool` (x86 ORC/stack validation)
   compiles against `<gelf.h>`; the server lacked libelf-dev, so the kernel build
   failed. `make check` now tells a fresh host upfront (libelf-dev /
   elfutils-devel).
3. **image ext4 as non-root** — stage 60 ran `du`/`mkfs.ext4 -d` unprivileged;
   fine in the root aarch64 container, but on the server (normal user) it hit
   "Permission denied" on root-owned 0700 dirs (`/root`, `/var/lib/sshd`, …). Fix:
   use `sudo` for the du + mkfs (and rm) when not root; no-op when already root.

Then GRUB / real-hardware boot.

## Real bugs found & fixed by building (all committed)

1. `make fetch` exited 1 on the all-hashes-pinned success case.
2. Linux headers used `aarch64` instead of the kernel's `arm64`.
3. macOS APFS case-insensitivity broke kernel extraction → work/ on ext4 volume.
4. libstdc++ headers installed off the cross g++ search path (gcc pass 2 libcody).
5. Missing merged-/usr symlinks → chroot "failed to run /usr/bin/env".
6. Native glibc rebuild needed bison/python (ch7) → skip it (cross glibc suffices);
   gate the heavy full-userland rebuild behind `VERK_FULL_USERLAND=1`.
7. coreutils refuses to configure as root → `FORCE_UNSAFE_CONFIGURE=1`.
8. util-linux liblastlog2 needs sqlite3 → `--disable-liblastlog2`.
9. kmod needs pkg-config → build pkgconf; needs scdoc → `--disable-manpages`.
10. libffi installed to `/usr/lib64` (off linker path) → `--libdir=/usr/lib` + ldconfig.
11. Python 3.12 ensurepip lacks setuptools → stage setuptools/wheel sdists.
12. markupsafe wheel was cp311 (host) not cp312 (target) → build it from sdist.
13. dbus 1.16 dropped autotools → build with meson.
14. systemd needs libcrypt (glibc 2.38+ drops it) → build libxcrypt.
15. libxcrypt needs perl → build perl; 5.40.0 miscompiles → pin 5.38.2.
16. `make && make install` under `set -e` doesn't abort on make failure (masked
    perl's failure and stamped it done) → use separate statements everywhere.
17. Cross-arch stamp leak: per-package native-build stamps were shared across
    arches (`/sources/.nst`) → moved into each rootfs
    (`/var/lib/vpk-build-stamps`) so an x86_64 build can't skip packages an
    aarch64 build already stamped.
18. dbus socket-activated unit required systemd support dbus wasn't built with →
    run as a plain service (boot-health).
19. vconsole-setup failed with no kbd installed → conditional drop-in skips it.
20. **glibc ld-linux ELOOP** on x86_64 merged-/usr → drop the manual symlink.
21. **kernel objtool** needs libelf/`gelf.h` → add to `make check`.
22. **image ext4** as non-root → `sudo` the du/mkfs when not root.

Plus the vpk security-audit fixes (path traversal, symlink write-through, ustar
checksum, strtok_r deps, .vpk integrity, remove-time refcounting — see above).

## Notes / deferred

- Only the **boot-critical** subset of the final system is built by default. Set
  `VERK_FULL_USERLAND=1` to also rebuild the compiler + full GNU userland.
- The native glibc rebuild is intentionally skipped (cross-toolchain glibc is
  used). A proper LFS ch7-temp-tools + ch8-glibc pass is a future improvement.
- More verkbox applets to graduate over GNU; repo/recipe signing for vpk; the
  neofetch art (verkfetch logo) is being refined.

VerkOS is now a self-hosting distro: it boots on aarch64 to a login shell, comes
online, is SSH-accessible, builds software from source and manages its own
packages (base system included) with **vpk**, ships its own core utilities with
**verkbox**, and is being brought up natively on x86_64.
