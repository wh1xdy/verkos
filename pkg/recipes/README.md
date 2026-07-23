# Verk recipes

A **recipe** tells `vpk` (the Verk package manager) how to build and install a
package from source. One directory per package under `/etc/vpk/recipes/<name>/`
containing a file named `recipe`:

```sh
name=nano
version=8.2
source=https://www.nano-editor.org/dist/v8/nano-8.2.tar.xz
sha256=d5ad07dd862facae03051c54c6535e54c7ed7407318783fcad1ad2d7076fffeb
depends="ncurses"          # space-separated recipe names, or empty
build() {
    ./configure --prefix=/usr --sysconfdir=/etc
    make
    make DESTDIR="$DESTDIR" install
}
```

## Fields

| Field     | Meaning |
|-----------|---------|
| `name`    | Package name (matches the directory). |
| `version` | Upstream version. |
| `source`  | URL of the source tarball (`.tar.xz` / `.tar.gz`). |
| `sha256`  | SHA-256 of the tarball — vpk refuses to build a mismatch. |
| `depends` | Recipes to install first (vpk resolves these recursively). |
| `build()` | Shell function run in the unpacked source dir. It **must** install into `$DESTDIR` (vpk then merges that into `/` and records the file list). |

## How `vpk install <name>` works

1. Resolve `depends` recursively (topological order, cycle-detected).
2. For each not-yet-installed package: fetch the source, **verify the sha256**,
   decompress + unpack, run `build()` into a private `$DESTDIR`, then merge the
   result into `/` and record every file in `/var/lib/vpk/db/<name>/files`.

`vpk remove <name>` deletes exactly those recorded files. `vpk list` shows what's
installed; `vpk info <name>` shows a recipe.

Everything (download, SHA-256, decompression, tar extraction, dependency graph,
database) is implemented in `vpk` itself — the only child process is your
`build()`.
