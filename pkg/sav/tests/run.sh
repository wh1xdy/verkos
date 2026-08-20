#!/bin/sh
# The sav test suite.
#
# Two halves, and the first is the one that matters most: every file under
# tests/refuse/ must be *refused*, with the reason the file names on its first
# line. A compiler whose refusals are untested is a compiler whose rules are a
# comment. The second half compiles the programs under tests/run/ and, where
# the host can run an aarch64 Linux executable, runs them and compares what
# they printed and what they exited with.
#
# Prints one PASS or FAIL line per case, so that the repository's own suite can
# report them individually rather than as one opaque verdict.
cd "$(dirname "$0")/.." || exit 2
# Always rebuild, never trust the file that is there: this repository is built
# from a Mac and from inside a Linux container against the same checkout, and
# `savc` in the working directory belongs to whichever built last. The `savc`
# target is phony and cheap, and it copies this host's binary into place.
SAVC=./savc
make -s savc >/dev/null 2>&1 || { echo "FAIL savc does not build"; exit 1; }

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
pass=0; fail=0
p() { echo "PASS $*"; pass=$((pass+1)); }
f() { echo "FAIL $*"; fail=$((fail+1)); }

# Can this host run what savc writes? The compiler is a host program and its
# output is aarch64 Linux, which is the same machine only inside the build
# container. Elsewhere the compile-time half still runs, and says so.
if [ "$(uname -s)" = Linux ] && [ "$(uname -m)" = aarch64 ]; then CAN_RUN=1; else CAN_RUN=0; fi

# ---- refusals ------------------------------------------------------------
for src in tests/refuse/*.sav; do
    name=refuse/$(basename "$src" .sav)
    want=$(sed -n '1s|^// expect: ||p' "$src")
    if [ -z "$want" ]; then f "$name (the file does not say what it expects)"; continue; fi
    if "$SAVC" "$src" -o "$T/out" >"$T/log" 2>&1; then
        f "$name compiled, and should not have"
    elif grep -qF "$want" "$T/log"; then
        p "$name is refused: $want"
    else
        f "$name is refused for the wrong reason: $(head -1 "$T/log")"
    fi
done

# ---- programs ------------------------------------------------------------
for src in tests/run/*.sav; do
    base=$(basename "$src" .sav)
    name=run/$base
    want_out=tests/run/$base.out
    want_status=$(sed -n 's|^// status: ||p' "$src" | head -1)
    [ -n "$want_status" ] || want_status=0
    args=$(sed -n 's|^// args: ||p' "$src" | head -1)

    if ! "$SAVC" "$src" -o "$T/PROG" >"$T/log" 2>&1; then
        f "$name does not compile: $(head -1 "$T/log")"
        continue
    fi
    p "$name compiles"
    [ "$CAN_RUN" = 1 ] || continue

    # shellcheck disable=SC2086
    ( cd "$T" && ./PROG $args ) >"$T/got" 2>&1
    got_status=$?
    if [ "$got_status" != "$want_status" ]; then
        f "$name exits $got_status, expected $want_status"
    elif diff -u "$want_out" "$T/got" >"$T/diff" 2>&1; then
        p "$name prints what it should"
    else
        f "$name printed something else"
        sed 's/^/      /' "$T/diff" | head -12
    fi
done

# ---- what the binary itself says -----------------------------------------
# The manifest is not a comment in the source: it is bytes in the executable,
# and it must hold exactly the calls the program can make. hello uses Console
# and nothing else, so 172 (getpid) being absent is the whole point.
"$SAVC" tests/run/hello.sav -o "$T/hello" --manifest >"$T/m" 2>&1
if grep -q 'system calls it may make: 64 93$' "$T/m"; then
    p "manifest: a program that only writes carries write and exit, and nothing else"
else
    f "manifest: wrong for hello ($(grep 'system calls' "$T/m"))"
fi
if grep -q 'capabilities: Console$' "$T/m"; then
    p "manifest: the capabilities are the ones the entry point asked for"
else
    f "manifest: wrong capability list ($(grep capabilities "$T/m"))"
fi

cat > "$T/proc.sav" <<'EOF'
entry fn main(out: Console, me: Proc) -> i32 {
    let p: i64 = me!pid();
    assert p > 0;
    out!write("x");
    return 0;
}
EOF
"$SAVC" "$T/proc.sav" -o "$T/proc" --manifest >"$T/m2" 2>&1
if grep -q 'may make: .*172' "$T/m2"; then
    p "manifest: asking for Proc adds getpid, and only because it was used"
else
    f "manifest: Proc did not add 172"
fi

# Rule 3 in the file rather than in the language: no relocation anywhere, and
# no absolute address in the loaded image. The kernel in this repository was
# broken twice by exactly that.
if command -v readelf >/dev/null 2>&1; then
    if readelf -r "$T/hello" 2>/dev/null | grep -q 'no relocations'; then
        p "the executable contains no relocations"
    else
        f "the executable has relocations: $(readelf -r "$T/hello" | head -3)"
    fi
    if readelf -l "$T/hello" 2>/dev/null | grep -q 'NOTE'; then
        p "the manifest travels as a note in the executable"
    else
        f "there is no PT_NOTE in the executable"
    fi
fi

# Rule 4, checked in the instructions themselves: a `dev` store is one store of
# the declared width. Two u32 writes to adjacent registers must stay two.
cat > "$T/dev.sav" <<'EOF'
entry fn main(out: Console) -> i32 {
    let a: dev *u32 = 0x9000000 as dev *u32;
    let b: dev *u32 = 0x9000004 as dev *u32;
    a@ = 1;
    b@ = 2;
    out!write("x");
    return 0;
}
EOF
if command -v objdump >/dev/null 2>&1 && "$SAVC" "$T/dev.sav" -o "$T/dev" >/dev/null 2>&1; then
    n=$(objdump -d "$T/dev" 2>/dev/null | grep -cE '\sstr\s+w[0-9]+, \[x[0-9]+\]')
    if [ "$n" -eq 2 ]; then
        p "two device writes stay two 32-bit stores, neither merged nor widened"
    else
        f "device writes came out as $n 32-bit stores, expected 2"
    fi
fi

echo "sav: $pass passed, $fail failed" >&2
[ "$fail" -eq 0 ]
