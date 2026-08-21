# sav

The VerkOS language. Named for the sap that moves through `stam`, the trunk —
it is meant to reach every part of the system eventually, and it starts where
the kernel already runs programs.

    savc program.sav -o program

One file in, one aarch64 ELF executable out. No preprocessor, no assembler, no
linker, no libc: `savc` writes every byte of the executable itself, which is
the only reason the last section of this document can be believed.

Why the language exists, and what was rejected on the way, is **D-109**.

---

## The four rules

sav is a small systems language with C's semantics and none of its habits. What
makes it worth having instead of C is four rules the compiler enforces:

1. **A claim must depend on what it claims.** `needs` and `gives` are part of a
   signature, the returned value is named so that a promise can be about it,
   and a clause the compiler can decide by itself is an error.
2. **Nothing reaches the world except through a capability.** There is no inline
   assembly and no free `write`; a system call comes from a capability method,
   written `out!write(...)`, or from nowhere.
3. **The capability set survives compilation.** It is written into the
   executable, and stam refuses every call outside it.
4. **Device memory is a type with its own syntax.** `uart@` is one access of the
   declared width, and it does not look like memory because it is not.

The syntax is ordinary where the semantics are ordinary — braces, `if`, `while`,
`fn`, `let` — and its own exactly where the semantics are its own. `needs` and
`gives`, a named result, `!` at every line that reaches the world, `@` at every
line that reaches hardware. Two of those are greppable:

    grep -n '!' *.sav      every line that touches anything outside the process
    grep -n '@' *.sav      every access to a device register

---

## A program

```sav
entry fn main(out: Console) -> i32 {
    out!write("hello from sav\n");
    return 0;
}
```

`entry fn` is where the program starts. Its parameters may only be
capabilities: what a program can do is what its entry point asks for. It
returns `i32` or `i64` as the exit status, or nothing.

## Types

| | |
|---|---|
| `i8 i16 i32 i64` | signed integers |
| `u8 u16 u32 u64` | unsigned integers |
| `bool` | `true` or `false` |
| `*T` | a pointer |
| `dev *T` | a pointer into device memory |
| `[]T` | a slice: a pointer and a length that travels with it |
| `[N]T` | an array of a known size |
| `struct` | named fields, laid out in order with natural alignment |
| `enum` | a tag and, after it, whatever the chosen case carries |
| `Console Args Files Proc` | capabilities |

There are no implicit conversions. `a + b` where `a` is `u8` and `b` is `i64`
is an error naming both types; write `as`. An integer *literal* takes the type
of what it is used with, which is the one exception and the only one that
cannot lose information.

A slice knows its length: `s.len`, and `s.ptr` when a raw pointer is wanted.
`a[2..5]` is a slice of a slice or of an array; `a[2..]` runs to the end.

**Every index is checked**, against the slice's length or the array's size, and
so is every slice — both ends. What fails says which number was wrong and what
it was checked against:

    hello.sav:12: index out of range (index 10, length 8)
    hello.sav:19: a slice that runs past the end (end 9, length 8)

A constant index into an array of known length is decided while compiling
instead. And a bare pointer cannot be indexed at all — it does not know how
many there are — so `p[0..n]` is the one place in sav where a length is
asserted rather than checked, and it has to be written down.

## Declarations

```sav
const LIMIT: i64 = 100;      // known while compiling
var counter: i64;            // a global. Globals start as zero: see rule 4
struct Point { x: i64, y: i64 }

fn add(a: i64, b: i64) -> i64 { return a + b; }
```

A global may not have an initialiser. The only interesting thing to put in one
is an address, and a table of absolute addresses is what broke this project's
kernel twice; assign to it in code, where the address is formed PC-relative.

Structs and arrays are passed as pointers, never by value: `fn dx(l: *Line)`.
`.` follows one level of pointer, so `l.to.x` works on a `*Line`.

## Enums, and match

```sav
enum Reply {
    Ok(n: i64),
    Failed(code: i64, why: []u8),
    Nothing,
}

fn parse(v: i64) -> r: Reply {
    if v > 0 { return Reply.Ok(v * 2); }
    if v == 0 { return Reply.Nothing; }
    return Reply.Failed(v, "a number below zero");
}

fn report(out: Console, r: Reply) {
    match r {
        Ok(n)            -> { out!write("ok\n"); }
        Failed(code, why) -> { out!write(why); }
        Nothing          -> { out!write("nothing\n"); }
    }
}
```

A value is eight bytes of tag and then the payload of whichever case it holds,
so it is as large as its largest case and can be passed and returned by value.
A case carries numbers, pointers and slices; to carry a struct, carry a pointer
to one.

`match` must say what to do with every case, or end with an `else`. Two things
are errors, and they are the same error wearing different clothes:

- a case nobody mentions — the compiler lists which ones;
- an `else` when every case is already taken. That arm can never run, and a
  branch that can never run reads as a safety net while catching nothing.

An arm may name what its case carries, or ignore it: `Ok(n)` binds `n`, `Ok`
binds nothing. Naming *some* of what a case carries is an error, because a
reader would have to count fields to know which ones.

## Modules

```sav
use "../lib/say.sav";
```

`use` names a file **beside the one that says it**, not beside the directory
savc happens to be run from, and each file is read once however many times it
is named. Everything shares one set of names — there are no namespaces yet.
Several files may also be given to savc on one command line.

`lib/num.sav`, `lib/text.sav` and `lib/say.sav` are what there is of a standard
library: numbers as text, bytes as text, and writing to a console that was
handed in. The first two take no capability at all, so nothing in them can
reach the world.

## Statements

`let` binds, `var` binds something that can change.

```sav
let n: i64 = args.count();
var sum: i64 = 0;
for i in 0..n { sum = sum + i; }
while sum > 10 { sum = sum - 10; }
if sum == 0 { ... } else if sum == 1 { ... } else { ... }
assert sum < 10;
return sum;
```

`break` and `continue` do what they look like. `assert` is held to rule 1: a
check the compiler can decide is an error, not a warning.

## Contracts

```sav
fn digits(v: i64) -> n: i64
    needs v >= 0
    gives n >= 1
{
    ...
}
```

`-> n: i64` names the value the function returns. `needs` is checked when the
function is entered and `gives` when it returns, and a `gives` clause talks
about the returned value by its name, the way every other expression in the
language talks about things. A function with a `gives` clause **must** name its
result: you may only promise about something you have named.

A broken contract prints the file, the line and the source of the clause, and
stops the program with status 70:

    hello.sav:12: requirement broken: v >= 0

The compiler refuses, always and regardless of flags:

- `gives true`, `needs 2 * 3 == 6` — decided before the program runs.
- `gives a > 0` on a function whose result is named `n` — never mentions `n`,
  so it says the same thing whether the function worked or not.
- `assert 2 > 1` — checks nothing.
- `gives` on a function whose result has no name — nothing to promise about.

`--no-contracts` removes the *runtime* checks. It does not remove the refusals;
those are the language.

The compiler does not prove contracts. `gives n >= 0` on a function that returns
`-1` compiles and fails when it runs. Vacuity is decided at compile
time because it is cheap and total; truth is not.

## Capabilities

A capability is a type whose values only the compiler makes, handed to
`entry fn main` and passed down as arguments.

| capability | methods | what it costs |
|---|---|---|
| `Console` | `write(s)`, `error(s)`, `read(buf)` | `write`, `read` |
| `Args` | `count()`, `get(i)` | nothing: the kernel already put them on the stack |
| `Files` | `open(path, flags)`, `read(fd, buf)`, `write(fd, buf)`, `close(fd)` | `openat`, `read`, `write`, `close` |
| `Proc` | `pid()` | `getpid` |

```sav
fn report(out: Console, s: []u8) { out!write(s); }

entry fn main(out: Console, args: Args) -> i32 {
    report(out, "a function can only write because it was given a Console\n");
    return 0;
}
```

The `!` is required, and it is not decoration: a capability method is the only
thing in the language that reaches outside the process, and the mark says where
that happens. `out.write(...)` does not compile, and the message says so.

A capability cannot be a global, cannot be cast to or from a number, and cannot
be reassigned. `Args.get(i)` past the end is an empty slice, not whatever was
on the stack.

## Device memory

```sav
let uart: dev *u32 = 0x9000000 as dev *u32;
uart@ = 65;                     // one 32-bit store, exactly here
let ch: u32 = uart@;            // one 32-bit load
```

`@` is how device memory is reached and the only way it can be: `*uart` does not
compile, and neither does `uart[1]` or a field through it. Every access through
a `dev` pointer is one instruction of the declared width — nothing merges two of
them, widens one, elides one or moves one past another — and `dev` cannot be
cast away, because that cast would be the laundering step.

The reverse holds too: `p@` on an ordinary pointer is an error. Memory is `*p`.

## What the executable carries

`savc --manifest` prints what goes into the binary:

```
  capabilities: Console Args
  system calls it may make: 64 93
  relocations: 0
```

The system calls are the ones the code generator actually emitted, collected in
the one place that emits `svc`. They travel in a `PT_NOTE` section. stam reads
it when it loads the program, inherits it across `fork`, replaces it at `exec`,
and refuses anything else — the process dies of SIGSYS and the console says
which call it asked for. A binary with no note is an ordinary program and is
not restricted.

`--omit-call N` builds a binary whose note leaves out a call the code makes. It
exists so that the kernel's refusal can be watched happening; `make test` uses
it and nothing else should.

## What sav does not have yet

No namespaces: `use` shares one set of names. No floating point. No struct
arguments by value — pass a pointer; a struct or an enum *returned* by value is
fine, and goes into space the caller owns. Nothing can yet be said about an
enum in a `needs` or `gives` clause, so a function that returns one may not
have contracts about its result. No self-hosting: savc is written in C, and sav
cannot yet express savc. aarch64 only.

`kvist` is reserved for something later.
