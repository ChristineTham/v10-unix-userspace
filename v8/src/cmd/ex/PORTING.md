# ex(1) / vi(1)

`ex` is `vi`. The two shipped binaries are byte-identical (`cmp` says so) and
upstream's makefile hard-links them; the shipped tree has `usr/bin/{ex,vi,view,
edit}` at 116736 bytes each. All 61 files are at `usr/src/cmd/ex`.

It is ported, installed under all four names, and **it edits**: print,
substitute, append, delete, write, with the file contents exactly right.

**Zero source changes.** All 28 objects compile under v8cc with upstream's own
`-DCRYPT -DLISPCODE -DCHDIR -DUCVISUAL -DVFORK -DVMUNIX -DSTDIO -DTABS=8`, and
the binary links with an empty `nm -u`. Every fix this port needed was
*outside* ex.

## What CLAUDE.md and PLAN.md said, and why it was wrong

Both recorded `vi` among the programs with "no source at all", beside `more`
and `pg`. `more` and `pg` are genuinely sourceless; `vi` is not. It was filed
with them because **`usr/src/cmd` has no directory called `vi`** — the sweep
looked for the program's name, and the source is under the name of the program
it is a link to. Before recording a program as sourceless, check whether it is
a *link*: `Admin/dest` and every makefile's install arm declare them.

## Three port bugs it found, all fixed and none of them in ex

1. **`<varargs.h>` was still 1985's.** `va_arg` strode `sizeof(mode)` — 4 for
   an `int` — where v8cc spills eight-byte argument slots. `ex/printf.c` is the
   **only** file in `src`, `shim` or `compiler` using `va_alist`/`va_dcl`,
   measured, so the header had never been consumed and had never been imported.
   This made ex SIGSEGV immediately after printing the file name: a four-byte
   stride over eight-byte slots splices two halves of adjacent arguments, and
   the first argument is always right, so the name appeared and nothing after
   it did. `src/include/varargs.h` has the whole account, including why
   `((mode *)(list += SLOT))[-1]` is the wrong repair.
2. **`exit` and `_exit` shared one member of `libv8stubs.a`.** A 1980s program
   that cleans up before leaving defines its own `exit()` and finishes with
   `_exit()`; the linker then pulls the member for `_exit` and its `exit()`
   collides. V8 keeps `sys/exit.s` and `sys/_exit.s` separate for exactly this,
   and the stub file's own comment said so while the code did not. Split.
3. **`tty_ld`/`ntty_ld` were recorded as "genuinely kernel state".** They are
   24 initialised ints in `libc/gen/linedis.c`, never imported. Verified before
   trusting, because a table can be dead (`dev/conf.c`): `conf/devices` — the
   file the build reads — agrees row for row, discipline 6 being `ntty`.

## THE MEMORY-CORRUPTION BUG THAT DID NOT EXIST

This file previously recorded that ex "executes no command and exits 0", and
offered a clue: an instrumented build — the same sources plus three `write(2)`
calls — *did* execute commands, which is the signature of memory corruption or
undefined behaviour moving under a changed layout.

**Every part of that was an artefact of the test harness.** There is no
corruption. ex worked the whole time.

The harness was a deadline wrapper that ran its argument as `"$@" &`. **A
backgrounded job in a non-interactive shell gets its stdin from `/dev/null`**,
so ex read EOF immediately, executed nothing, and exited 0 — which is exactly
what a broken editor looks like. Measured afterwards with a three-line control:
`printf 'X\n' | sh -c 'cat & wait'` prints nothing.

And the "instrumented build behaves differently" comparison changed **two**
variables, not one: the instrumented build was run *directly* and the clean one
*through the wrapper*. The instrumentation was never the difference.

Three things worth keeping from it:

- **A wrapper that backgrounds is not transparent.** It is fine for a program
  that takes no input and fatal for one that does, and the failure looks like
  the program's.
- **This was the third instrument-authored false finding in one session** —
  the same harness had earlier reported a hang as a clean exit, and a `$OPTS`
  unquoted under zsh had made "27 of 28 objects compile" a measurement of
  compiling ex with no options at all. CLAUDE.md's rule that *an instrument you
  wrote is a suspect* held three times out of three.
- **The tell was available and unread.** Ten runs of the same binary produced
  the same output; genuine corruption is rarely that stable. Checking
  determinism first would have cost one command and saved the diagnosis.

The lesson is the repository's own: **verify a recorded diagnosis before
building on it.** This one was built on for four documents and a test
exclusion before it was checked.

## The carriage returns are ex's, and reachable from the user side

By default `p` output comes out `aa\n\r bb\n\r`. That is not the shim and not
a terminal-database problem: it is ex's `optimize` option. `pstart()`
(`ex_put.c:852`) clears `CRMOD` on fd 1 — `tty.sg_flags = normf &
~(ECHO|XTABS|CRMOD)` — so the kernel stops mapping `\n` to `\r\n` and ex
supplies the carriage return itself.

Nothing about it is machine-dependent, and it is the same for every terminal
type including an unset `TERM` (measured over five). It is **user-controllable**,
which is what makes it a test rather than a sentence: `set nooptimize` takes
the same early return `pstart()` takes when termcap reports `NONL`, and the CRs
disappear. `tests/wavea` asserts both.

## Still open

- **Visual mode is unexercised.** Invoked as `vi` the binary correctly answers
  `Open and visual must be used interactively` — which is itself worth having,
  because it proves the hard link reached the program and that it dispatches on
  `argv[0]`. Exercising the screen half needs a pty, which nothing in this
  suite has yet.
- **`exrecover` and `expreserve` are not built.** They are separate programs
  with their own `main()`, which is why the object list is spelled out rather
  than globbed — `ex*.c` matches them.
- **`xstr` is deliberately skipped.** Upstream compiles every file through
  `cc -E | xstr -c -`, lifting string literals into a shared table, then builds
  `strings.o` by rewriting assembler with an `ed` script — the same
  read-only-shared-text optimisation that stops `sh` and `cpp` at rung 5 and
  that arm64 Mach-O structurally cannot do. It is a pure size optimisation and
  **no source depends on it**: measured, the only references are two comments
  and the `char xstr[1];` dummies in `expreserve` and `exrecover`, which exist
  precisely because those two do *not* go through it. The cost is that ex
  cannot claim rung 5.
- **One linker warning**, and ex is the first program here big enough to
  produce it: `reducing alignment of section __DATA,__common from 0x8000 to
  0x4000`. `_incorb` is 66560 bytes, the largest common in the port. The
  section still lands 16 KB-aligned and nothing needs more, so this is
  cosmetic — but it is the first of its kind and worth knowing before the next
  large program repeats it.
- **`ex_tty.c`'s `FIOLOOKLD` arm is now reachable** because `ntty_ld` exists.
  It asks the kernel which line discipline is pushed, and this port's
  `/dev/tty` is `/dev/fd/3` with no discipline behind it, so what that ioctl
  should answer is a decision nobody has made.
