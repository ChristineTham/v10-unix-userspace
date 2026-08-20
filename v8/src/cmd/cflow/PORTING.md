# Porting cflow(1)

**Zero source changes.** All twelve imported files are byte-identical to
pristine V8, verified against `PROVENANCE`.

`cflow` prints a program's call graph. It is four small programs -- `dag`,
`lpfx`, `nmf`, `flip`, 806 lines all told -- plus a shell script, and the only
reason it was not here years ago is a **run-time** dependency: `cflow.sh:9` is
`LINT1=/usr/lib/lint/lint1`, and the script pipes `cc -E` through lint1 into
`lpfx`. See `src/cmd/lint/PORTING.md` for why lint1 was recorded as
unportable and why that was wrong.

Its own build needs nothing of pcc1 but a **header**: `lpfx.c:5` includes
`"manifest"` for the operator numbers. Three further pcc includes --
`lerror.h`, `lmanifest`, `lpass2.h` -- are **commented out in upstream's
source** although `cflow.mk` still lists them as prerequisites, which is a
build description that outlived what it describes.

## Two upstream copies, and the shipped artefact settles it

`cflow.sh` exists twice: `usr/src/cmd/cflow.sh` at 1610 bytes and
`usr/src/cmd/cflow/cflow.sh` at 1601. The second is **byte-identical to the
shipped `/usr/bin/cflow`**. That is `libtermlib`'s fingerprint rule, and
`tests/wavea` asserts it rather than leaving it to this sentence -- building
the wrong copy should be a failure, not a discovery.

## `-DUNIX5` is load-bearing and fails LOUDLY

`nmf.c:2` is `#if defined(UNIX5) && !defined(pdp11)` and it wraps the
**entire file**, so without the flag `nmf.o` is an empty translation unit and
the link fails for want of `main`. That is the `-DCM_N` shape with the good
outcome: a missing `-D` usually selects an arm silently, and here it cannot.
No case is aimed at it because `make` failing is stronger than a case.

## Audited and unchanged

- `dag.c:216` `char *calloc();` and `lpfx.c:189` `char *malloc();` are both
  **declared**, which is the safe half of the truncated-pointer-return class.
  The rootfs-wide sweep in `tests/v8ccom` agrees.
- `dag.c:72` is `dfs(getnode(*++argv), 0)`, which matches the argv-exhaustion
  shape this port has met ten times -- and it is guarded, by `while (--argc >
  0)` on `dag.c:71`. **A shape that matches a known class still has to be
  read.**
- `cflow/lint.h` is a third copy of `lint.h` and is **byte-identical** to
  `lint/lint.h`, so the quoted include finding cflow's own costs nothing.
  Worth knowing before either is edited: they are two files with one content.

## Recorded rather than fixed, per S1

`flip.c` scans for a colon with `while (*pl != ':') ++pl;` and has no bound, so
a line without one runs off the end of `line[BUFSIZ]`. Upstream's, on
upstream's hardware -- a VAX ran off the same array -- so there is no VAX
answer to restore and the change is not forced by the target. `flip` is fed by
`lpfx`, which emits colons.

## The rest of the pipeline

`nmf` reads `nm` output, and `nm` is the **host's** by the documented
object-format exception. That path is not exercised here; the ordinary
invocation (`cflow file.c`) does not reach it.
