# ex(1) / vi(1) — IMPORTED, BUILDS, NOT YET CORRECT

`ex` is `vi`. The two shipped binaries are byte-identical (`cmp` says so) and
upstream's makefile hard-links `vi`, `view`, `edit` and `e` to `ex`. All 61
files are at `usr/src/cmd/ex`.

**This document is a record of a port in progress, not a finished one.** It is
here because the state is worth having written down rather than rediscovered:
what works, what does not, and the one clue that says what kind of bug is left.

## What CLAUDE.md and PLAN.md said, and why it was wrong

Both recorded `vi` among the programs with "no source at all", beside `more`
and `pg`. `more` and `pg` are genuinely sourceless; `vi` is not. It was filed
with them because **`usr/src/cmd` has no directory called `vi`** — the sweep
looked for the program's name, and the source is under the name of the program
it is a link to. Before recording a program as sourceless, check whether it is
a *link*: `Admin/dest` and every makefile's install arm declare them.

## What works

- **All 28 objects compile under v8cc with NO source change**, with
  upstream's own `-DCRYPT -DLISPCODE -DCHDIR -DUCVISUAL -DVMUNIX -DSTDIO
  -DTABS=8`. Two warnings, both benign: `ex_tune.h` deliberately redefines
  `NCARGS` over `<sys/param.h>`'s, and one file redefines `BUFSIZ`.
- **It links with an empty `nm -u`** — nothing from the host libc.
- **It opens a file and reports it truthfully**: `"f.txt" 2 lines, 12
  characters`.
- The command loop runs and reads standard input, measured by instrumentation
  rather than inferred.

## Three port bugs it found, all fixed and all outside ex

None of these are in ex, and all three were latent before it arrived:

1. **`<varargs.h>` was still 1985's** — `va_arg` strode `sizeof(mode)` where
   v8cc spills eight-byte argument slots. `ex/printf.c` is the **only** file in
   `src`, `shim` or `compiler` using `va_alist`/`va_dcl`, measured, so the
   header had never been consumed and never been imported. This is what made
   ex SIGSEGV immediately after printing the file name: a four-byte stride over
   eight-byte slots splices two halves of adjacent arguments, and the first
   argument is always right, so the name appeared and nothing after it did.
   `src/include/varargs.h`.
2. **`exit` and `_exit` shared one member of `libv8stubs.a`.** A 1980s program
   that cleans up before leaving defines its own `exit()` and finishes with
   `_exit()`; the linker then pulls the member for `_exit` and its `exit()`
   collides. V8 keeps `sys/exit.s` and `sys/_exit.s` separate for exactly this,
   and the stub file's own comment said so while the code did not. Split.
3. **`tty_ld`/`ntty_ld` were recorded as "genuinely kernel state".** They are
   24 initialised ints in `libc/gen/linedis.c`, which had simply never been
   imported. Verified against `conf/devices` — the file the build reads — which
   agrees row for row, including discipline 6 = `ntty`.

## What does NOT work, and the clue

`ex` executes **no command**. `p`, `1,$p`, a substitution followed by `w` — all
produce the status line, no output, no change to the file, and exit 0.

**The clue is that an instrumented build works.** The same sources plus three
`write(2)` calls — one at the top of `commands()`, one in its loop, one at the
`read(0, ...)` in `getach()` — *does* execute `p` and print the line. Behaviour
that changes when debug output is added is not a missing feature; it is memory
corruption or undefined behaviour moving under a changed layout. That is where
the next session should start, and it is why this is not a "finish the port"
task but a "find the bug" one.

Two more observations worth keeping:

- **`TERM` matters.** With `TERM=dumb` the instrumented build stops earlier
  than with `TERM=vt100`, and `dumb` is in `/etc/termcap` (line 3,
  `su|dumb|un|unknown:co#80:os:am:`) carrying `os` — overstrike. ex has whole
  code paths for overstriking terminals. Do not read "unknown terminal type"
  as a termcap failure without checking that first.
- **`xstr` is deliberately skipped.** Upstream compiles every file through
  `cc -E | xstr -c -`, which lifts string literals into a shared `strings`
  file, and then builds `strings.o` by rewriting assembler with an `ed`
  script — the same read-only-shared-text optimisation that stops `sh` and
  `cpp` at rung 5, and that arm64 Mach-O structurally cannot do. It is a pure
  size optimisation: **no source depends on it**, measured — the only
  references are two comments and the `char xstr[1];` dummies in `expreserve`
  and `exrecover`, which exist precisely because those two programs do *not*
  go through it. Skipping it costs nothing but a rung-5 claim.

## Still open beyond the bug

- `exrecover` and `expreserve` are separate programs and are not built.
- `-DVFORK` is not passed; upstream's makefile sets it and `vfork` appears
  once. `fork` is the honest default until something measures the difference.
- `ex_tty.c`'s `FIOLOOKLD` arm is now reachable because `ntty_ld` exists. It
  asks the kernel which line discipline is pushed, and this port's `/dev/tty`
  is `/dev/fd/3` with no discipline behind it, so what that ioctl should
  answer is a decision nobody has made.
