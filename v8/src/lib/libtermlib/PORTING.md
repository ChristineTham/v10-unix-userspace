# libtermlib — the termcap library

The first thing imported out of `usr/src/lib` rather than `usr/src/cmd`.
Installed as `/usr/lib/libtermcap.a`, with `/usr/lib/libtermlib.a` a hard link
to it, and it needs `/etc/termcap` — 44669 bytes of authentic database, which
is imported alongside it as data.

**One source change**, and it is the same one `ls.c` already carries.

## 1. There are TWO upstream copies, and the build reads the one with the Blit

`usr/src/lib/libtermlib/termcap.c` and `usr/src/cmd/ex/termlib/termcap.c` are
the same file apart from **eleven lines** — a block in `tgetnum()` that asks a
jerq (Blit) terminal its window size before consulting the database.

CLAUDE.md's rule for two disagreeing upstream files is to find out which one
the *build* reads. Two independent measurements, and they agree:

- **`ioctl` occurs in the whole library only inside those eleven lines.**
  Nothing in `tgoto.c` or `tputs.c` calls it, and the ex copy has no
  occurrence at all. So an undefined `_ioctl` in a built archive is a
  *fingerprint* of that source — and the shipped `usr/lib/libtermcap.a` has
  one. `tests/wavea` asserts our archive carries it too.
- **`cmd/ex/termlib` installs somewhere that is not in the shipped tree.** Its
  makefile opens `UCB=/lusr/ucb` and installs to `$(UCB)/lib/libtermcap.a`;
  there is no `/lusr` anywhere in V8. `lib/libtermlib` installs to
  `${DESTDIR}/usr/lib/libtermcap.a`, which is where the shipped 6556-byte
  archive is.

So `cmd/ex/termlib` is the Berkeley import staging area and `lib/libtermlib`
is the library V8 shipped.

## 2. The one change: an absolute-path include, inside a function body

Upstream `termcap.c:217` -- ours at `:230` after the comment -- was

```c
	if (strcmp(id, "co")==0 || strcmp(id, "li")==0) {
#include "/usr/jerq/include/jioctl.h"
		struct winsize jwin;
```

An absolute path is the one form of include no `-I` can redirect, so it cannot
resolve on any machine that is not a 1985 Bell Labs VAX with the jerq software
installed. It is now `#include <jioctl.h>`.

**The header itself is authentic and unchanged.** It was *not* missing from the
distribution — a first reading of this concluded it was, and that was wrong in
the way this repository keeps recording. `jioctl.h` is at `jerq/include/` in
the same archive, `tools/import.sh` already has a `blit/*|jerq/*` case for it,
and the Makefile already copies it to `rootfs/usr/include/jioctl.h` because
**`src/cmd/ls.c:11` needed exactly this line changed for exactly this reason**.
The precedent was in the tree before this import began.

Two details worth keeping:

- **It stays inside the function body**, where Bell Labs put it. That is legal
  — the preprocessor is line-based, and the header declares only macros and a
  struct, so the tag scopes to the block.
- **The two copies of `jioctl.h` are not identical.** `jerq/include` says
  `JMUX`, `blit/include` says `JMPX`; everything else agrees, `JWINSIZE`
  included. The rootfs carries the jerq one, which is what `/usr/jerq/include`
  named.

## 3. The four `-D` flags are upstream's and are not decoration

Its makefile opens `CFLAGS= -DCM_N -DCM_GT -DCM_B -DCM_D`, and each guards one
case in `tgoto`'s cursor-addressing interpreter: `%n`, `%>xy`, `%B` (BCD) and
`%D` (Delta Data). **A missing one is silent**: the case is not a compile error,
it falls through to `default: goto toohard` and `tgoto` returns the string
`"OOPS"` for every terminal whose `cm=` uses that escape. A cursor that never
moves reads as a broken terminal rather than as a missing `-D`.

Measured: dropping all four takes `tgoto.o` from 2376 to 2096 bytes.

## 4. The jerq window-size block is DEAD here, deliberately, and so is ls's

The shim implements no `JWINSIZE`, so `ioctl(0, JWINSIZE, &jwin)` fails and
`tgetnum` falls through to the database. `ls.c` is in exactly the same state
and has been for as long as it has been ported — its block is additionally
guarded by an `ioctl(1, JMUX, 0)` that fails first.

This is left alone rather than implemented, for two reasons that should be
read together before anyone changes it:

- Implementing it would change `ls`'s column output, which several existing
  cases depend on. It deserves its own measured step rather than a side effect
  of importing a library.
- **`struct winsize` is `{ char bytesx, bytesy; short bitsx, bitsy; }`** — the
  columns field is a `char`. So a terminal wider than 127 columns reads
  **negative** and one wider than 255 wraps. That is this port's 16-bit-range
  class (`DIRSIZ`, `d_ino`, `p_pid`, `FSNMLG`, `u_uid`) arriving in a field
  narrower still, and a Terminal.app window crosses 127 columns routinely. So
  the honest implementation clamps, and the clamp is the design decision that
  makes it a step of its own.

## Still open

- `tc1.c`, `tc2.c` and `tc3.c` are upstream's own test drivers. They are
  imported (the directory came in whole) but not built: upstream's `termcap.a`
  target does not build them either.
- `tgoto`'s `%.` and `%+` arms use the `UP`/`BC` backspace hack, which
  `tests/wavea/tgotoprobe.c` does not reach. They need a terminal whose `cm=`
  produces a null, tab or newline coordinate, which is what the hack exists to
  avoid sending.
- `tputs`'s padding arm is unexercised: `ospeed` is a global that the *caller*
  sets, `ul` never does, and `tputs` returns before emitting pad characters
  when it is 0. The `tmspc10[]` table and the `PC` character are therefore
  untested. A consumer that calls `gtty` and sets `ospeed` — `ex` does — is
  what will reach it.
