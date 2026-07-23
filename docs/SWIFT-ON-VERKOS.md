# Running Swift on VerkOS

Reference for packaging the official **swift.org** toolchain as a vpk package,
and the path toward running Swift workloads (incl. the reactorsim simulation
core) on VerkOS. Researched 2026-07-23 (Swift **6.3.3**, URLs HEAD-verified live).

> **Note on reactorsim:** reactorsim is a **SwiftUI/AppKit macOS app**. SwiftUI
> and AppKit do **not** exist on Linux — the `.app` cannot run and the GUI cannot
> be recompiled as-is. The realistic path is to extract reactorsim's **portable
> simulation core** (Foundation/compute Swift) and build it headless on VerkOS
> with this toolchain, or re-port the UI to a Linux GUI toolkit. This doc is the
> toolchain enabler for that.

## Toolchain source (swift.org, Ubuntu 22.04 tarball)

VerkOS is glibc 2.40 + newer gcc, i.e. **newer** than the Ubuntu 22.04 the
toolchain targets (glibc 2.35) — so the GLIBC/GLIBCXX floor is satisfied (the
rule only breaks the other way; symlinks can't fix a too-new toolchain on a
too-old system).

- URL pattern: `https://download.swift.org/swift-{V}-release/{DIR}/swift-{V}-RELEASE/swift-{V}-RELEASE-{SUFFIX}.tar.gz`
  - x86_64:  `DIR=ubuntu2204`          `SUFFIX=ubuntu22.04`
  - aarch64: `DIR=ubuntu2204-aarch64`  `SUFFIX=ubuntu22.04-aarch64`
  - **Path segment has no dot (`ubuntu2204`); filename has a dot (`ubuntu22.04`).**
- Checksums: swift.org publishes a **GPG `.sig` only** (no `.sha256` — that URL
  302s to a 404). Verify via `curl https://swift.org/keys/all-keys.asc | gpg
  --import -` then `gpg --verify <file>.sig <file>`, or self-compute a sha256
  after first download and pin it in the recipe.

## Dependency split (the crux)

- **Bundled in the tarball** (distro does NOT provide): the Swift stdlib
  (`libswiftCore`), Concurrency/StringProcessing runtimes, `libdispatch` +
  Blocks, **all of Foundation**, and **ICU** (Swift 6 statically links
  FoundationICU) — so **do not** build a system libicu.
- **Distro must provide, needed by compiled programs at runtime:** glibc,
  `libstdc++.so.6`, `libgcc_s.so.1`; and for Foundation programs additionally
  `libxml2.so.2` (FoundationXML), `libcurl.so.4` (FoundationNetworking),
  `libz.so.1`, `libsqlite3.so.0`, plus `tzdata`.
- **Distro must provide, toolchain-only** (a shipped binary doesn't need them):
  `libedit.so.2` + `libtinfo.so.6`/`libncursesw.so.6` (REPL/lldb),
  `libpython3.x` (lldb), `libz3.so.4` (clang), binutils (link-time ld/gold),
  `libc6-dev` headers/CRT.

**VerkOS already has:** glibc, gcc, binutils(ld/gold), ncurses(libncursesw.so.6),
zlib, openssl, curl(libcurl.so.4), python 3.12, perl, pkg-config, git.
**VerkOS lacks:** `libxml2` (the ONE required for a plain `swiftc`), and libedit /
sqlite3 (only for REPL / SwiftPM respectively). ICU: not needed (bundled).

## The recipe (`pkg/recipes/swift/recipe`, aarch64 — swap URL for x86_64)

Installs into a self-contained `/opt/swift` (avoids colliding with base
gcc/binutils in /usr), adds a PATH shim + an ld.so.conf entry for the bundled
runtime/ICU. `depends="libxml2"` so vpk auto-builds libxml2 first.

```
name=swift
version=6.3.3
source=https://download.swift.org/swift-6.3.3-release/ubuntu2204-aarch64/swift-6.3.3-RELEASE/swift-6.3.3-RELEASE-ubuntu22.04-aarch64.tar.gz
sha256=
depends="libxml2"
build() {
    mkdir -p "$DESTDIR/opt"
    TB=/var/cache/vpk/swift-6.3.3-RELEASE-ubuntu22.04-aarch64.tar.gz
    if command -v tar >/dev/null 2>&1 && [ -f "$TB" ]; then
        tar -xzf "$TB" -C "$DESTDIR/opt"
        mv "$DESTDIR/opt/swift-6.3.3-RELEASE-ubuntu22.04-aarch64/usr" "$DESTDIR/opt/swift"
        rm -rf "$DESTDIR/opt/swift-6.3.3-RELEASE-ubuntu22.04-aarch64"
    else
        cp -a usr "$DESTDIR/opt/swift"
    fi
    mkdir -p "$DESTDIR/etc/profile.d"
    printf 'export PATH=/opt/swift/bin:$PATH\n' > "$DESTDIR/etc/profile.d/swift.sh"
    mkdir -p "$DESTDIR/etc/ld.so.conf.d"
    printf '/opt/swift/lib/swift/linux\n' > "$DESTDIR/etc/ld.so.conf.d/swift.conf"
}
```

A minimal `libxml2` recipe is needed too: `./configure --prefix=/usr
--without-python --without-lzma --without-icu && make && make DESTDIR=... install`.

## Post-install fixups

- **`ldconfig` (mandatory)** after install — activates the swift.conf so the
  loader finds `/opt/swift/lib/swift/linux` (libswiftCore + bundled ICU).
  vpk has no postinstall hook, so this is manual.
- Only if `ldd` shows them missing: `ln -sf /usr/lib/libncursesw.so.6
  /usr/lib/libtinfo.so.6` (REPL/lldb), `ln -sf /usr/lib/libcurl.so.4
  /usr/lib/libcurl-gnutls.so.4` (FoundationNetworking), `swiftc -use-ld=lld`
  (if clang can't auto-find VerkOS's gcc crt objects).

## Test plan (booted VerkOS)

```
vpk install swift            # builds libxml2 first, unpacks toolchain to /opt/swift
ldconfig                     # activate the runtime lib path
. /etc/profile.d/swift.sh
swiftc --version
printf 'print("Hello from Swift on VerkOS")\n' > /tmp/h.swift
swiftc /tmp/h.swift -o /tmp/h && /tmp/h    # -> Hello from Swift on VerkOS
```

## Known vpk bug surfaced by this research

vpk's built-in `untar` silently **drops tar hardlink entries (type `1`)** — the
swift.org tarball uses symlinks (which vpk handles) but any hardlinked payload
would be lost. The recipe works around it by re-extracting with system `tar`.
**TODO:** teach vpk's untar to handle hardlinks (treat type `1` like a symlink
or a copy of the target).
