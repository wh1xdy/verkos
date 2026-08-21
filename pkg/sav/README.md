# sav — the VerkOS language

    make                       # builds savc for this host
    ./savc examples/savhello.sav -o savhello --manifest
    sh tests/run.sh            # the suite: refusals, programs run and compared, and the binary itself

`savc` turns one `.sav` file into one aarch64 ELF executable. Nothing else is
involved: no preprocessor, no assembler, no linker, no libc, no relocations.

The language is described in `../../docs/sav.md`; why it exists is D-109 in
`../../docs/DECISIONS.md`. In short, four rules taken from the defects that
have cost this project the most time:

1. `needs`/`gives` are part of a signature, the result is named so a promise can
   be about it, and a clause that would read the same whether the function
   worked or not does not compile.
2. A system call can only be reached through a capability handed to
   `entry fn main`, and is written `out!write(...)` — so a signature says what a
   program can do and `grep -n '!'` says where it does it.
3. That set is written into the executable, and stam refuses the rest.
4. `dev *T` is reached only through `uart@`: one access, the declared width,
   where it is written. And no address savc emits ever needs relocating.

    lib/           num.sav, text.sav, say.sav — what there is of a library
    examples/      savhello.sav, upper.sav — programs stam runs
    src/lex.c      tokens, and the diagnostics everything else uses
    src/types.c    types, and the capability table — the one list of calls
    src/parse.c    source to a tree
    src/check.c    types, and the rules above
    src/arm.c      aarch64 instruction encoding
    src/gen.c      the tree to machine code
    src/elf.c      the executable, and the note the kernel reads
    src/main.c     the driver

Objects are built under `build/$(uname -s)-$(uname -m)`: this repository is
built from a Mac and from inside a Linux container against the same checkout,
and one set of object files cannot serve both.
