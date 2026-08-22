# v10-unix-userspace

Porting Research Unix userspace to macOS (Apple Silicon) and Linux/ARM64.

Starting with **V8** (Eighth Edition, 1985) as the beachhead, then V9, then
**V10** — the last Research Edition, and the original goal of the project.

The point is not a compatibility layer that runs old binaries. It is to rebuild
the V8 world from source with the *authentic* Bell Labs C compiler, running
against the *authentic* V8 C library, on a machine that did not exist in 1985.

See **[PLAN.md](PLAN.md)** for the full scope: fidelity contract, target model,
phase breakdown, per-program port policy, and risk register.
**[ARTICLE.md](ARTICLE.md)** is the narrative account of how it was built and
what went wrong on the way.

## Quick start

```bash
make -j8
```

```bash
make test
```

```bash
make install
```

```bash
v8
```

That last command drops you into a 1985 Unix:

```
Research Unix, Eighth Edition.
You are christie.  Your files are in /usr/christie and they persist.
$ ls /bin | wc -l
      40
$ cc -o hello hello.c && ./hello
```

The `cc` there is Bell Labs' compiler, compiling against Bell Labs' C library,
producing an ARM64 Mach-O binary.

## Requirements

- **macOS on Apple Silicon.** The compiler backend targets ARM64/AAPCS64 and the
  object format is Mach-O. An x86 host will not do.
- **Xcode command line tools** — `clang` and `ld` are used deliberately; see
  *The deliberate exceptions* below.
- Nothing else. No Homebrew, no package manager, no network.

## Building and testing

```bash
make -j8
```

A clean build takes about ten seconds (measured: 11s, `-j8`). The root
`Makefile` builds nothing
itself — it dispatches to `v8/`, and `make -C v8 <target>` is the same thing
said explicitly.

```bash
make test
```

**2876 cases across 17 suites.** Run it serially — `make -j8 test` is *not*
`make -j8` followed by `make test`, because under `-j` the suites run
concurrently with each other's prerequisite builds and read objects that are
midway through being written. It fails in the shape of a code bug.

A single suite, which requires `-C v8` because the root Makefile forwards only
`all`, `test`, `clean`, `distclean` and `install`:

```bash
make -C v8 test-wavea
```

The suites are `deps jail selfhost cpp v8ccom v8cc v8sys freestanding libv8c
wavea waveb sh wavec kmemu streams mkfs hooks`. Each is a shell script that
prints `name: N passed, M failed`; there is no per-case filter.

There is also a crash probe, deliberately **not** part of `make test` because it
takes about fifteen minutes:

```bash
v8/tests/crash-probe.sh "$PWD/v8/rootfs" /tmp/probework
```

It runs every installed binary against every single-letter option — 9434
invocations over 178 programs — and compares the signal deaths against
`v8/tests/crash-probe.floor`. That file is a *list*, checked in both directions,
so a crash that has been fixed fails the run too and must be removed in the same
commit.

## Installing

```bash
make install
```

| | default | what it is |
|---|---|---|
| `PREFIX` | `~/.local/share/v8` | the **golden image** — pristine, written once, never touched again |
| `BINDIR_HOST` | `~/.local/bin` | where the `v8` launcher script goes |

**Why `~/.local` and not `/usr/local`.** The default used to be `/usr/local`,
which was never argued for anywhere — it was taken as given, and it costs
`sudo` for a tool that is personal by construction. The golden image is
read-only after installation, and the world anybody actually uses is a working
copy in `$HOME/.v8`. On a stock Mac `/usr/local` is not writable and
`~/.local/bin` is already on `PATH`, so the home-directory default needs no
privilege and no PATH edit.

What `/usr/local` buys is one golden image shared by every account on the
machine. That is a real thing to want and is still one variable away:

```bash
sudo make install PREFIX=/usr/local/v8 BINDIR_HOST=/usr/local/bin
```

The launcher already handles that case: `/etc/passwd` is synthesised at first
run rather than at build time, precisely so an image installed by one account
can be run by another.

**Two things to know.** `make install` begins with `make clean` and rebuilds
from scratch — not out of caution, but because the world's root directory is
baked into every binary by a compiler flag, and `make` has no way to know a flag
changed. And for the same reason it leaves the *build tree* carrying the
installed path and no test stamp, so run this before believing any suite
afterwards:

```bash
make clean && make -j8 && make test
```

## Using the world

```
usage: v8 [--pure] [--golden] [--reset] [--help] [directory]
```

| | |
|---|---|
| `v8` | enter your world at `$HOME/.v8`, as yourself, in `/usr/<you>` |
| `v8 /usr/src` | land somewhere else. The argument is a **directory**, not a command — `v8(1)` takes the landing directory as its argument and always execs `/bin/sh` |
| `v8 --pure` | `V8JAIL=strict`: refuse every host binary but the documented toolchain exception. Nothing that is not V8 can run, including `python` and `git` |
| `v8 --golden` | enter the pristine image read-only, to see what a fresh world looks like |
| `v8 --reset` | delete the working copy and rebuild it from the golden image. Confirms first, and requires the literal word `RESET` |

**The world is a working copy, and that is what makes it usable.** A file
written in `/usr/<you>`, a program built and installed into `/bin`, an edited
`/etc/motd` — all survive the next launch, the next `make install`, and a
`make clean` in the build tree. `v8 --reset` is the only thing that destroys it.

`$V8WORK` overrides `$HOME/.v8` if you want a throwaway world:

```bash
V8WORK=/tmp/scratch-v8 v8
```

**Getting back out.** 115 of this world's 198 commands share a name with one of
the Mac's — `make`, `cc`, `sed`, `sh`, `grep`, `sort`, `ls`, `cp`, `test` among
them — so a native build started from inside would find V8's `make` and V8's
`cc`. `macos(1)` restores the host `PATH` and execs:

```
$ macos python3 --version
```

The useful formulation: **native apps work, native builds do not.** Running an
app is one exec and `python3`, `git` and `node` do not collide at all; building
software is hundreds of PATH lookups against exactly the names V8 owns. So build
on the Mac, work in V8, and `macos CMD` for the rest. `v8 --pure` refuses
`macos`, which is that mode working rather than a gap.

## What is in it

**198 of the 285 executable commands V8 shipped** across `/bin`, `/usr/bin` and
`/etc`. The compiler, the shell, the editors (`ed` and its link `e`, `qed`, and
`ex` under all four of its names — `vi`, `view`, `edit`), the text tools, `troff` and its preprocessors (`eqn`, `tbl`, `pic`,
`grap`, `refer`), `awk`, `f77`, `efl`, `ratfor`, `struct`, `lint`, `cflow`,
`cyntax`, `bc`/`dc`, and the filesystem tools.

Of the 87 not present, every one has a measured reason: 45 shipped without
source at all, 8 are PDP-11 cross-tools, 7 are the toolchain exception below, 4
are caught in a C++ bootstrap cycle (`cfront` is written in a dialect no modern
compiler will read, and the only compiler that can read it *is* `cfront`), and
the rest need a machine, a daemon, a network or a Blit terminal that this port
does not have. `shutdown` is portable and deliberately absent: inside the jail
its `kill` would signal **host** processes.

## The deliberate exceptions

`as`, `ld`, `ar`, `strip` and `nm` are the **host's**, because the object format
is Mach-O; porting V8's a.out assembler and link editor is out of scope. The
`cc` driver execs `clang` for assembly and linking. This is a decision, not a
gap — do not "fix" it.

The *enforced* list is narrower than that prose, deliberately. What a jailed
program may actually exec is `/usr/bin/clang`, `/usr/bin/as` and
`/usr/bin/strip` — nothing more, because those are the three that something in
the tree genuinely runs. `ld` is reached through `clang`; `nm` is **refused**,
and `tests/jail` asserts the refusal, so the list cannot quietly grow into
"whatever was on PATH". Everything else is ported rather than passed through,
and `v8 --pure` is what makes that checkable.

## Layout

The repository holds a **series** of ports, so it is split by what varies per
release and what does not.

| Path | What |
|---|---|
| `v8/` | The Eighth Edition port — everything below, plus its own `Makefile`. `v9/` and `v10/` become siblings. |
| `third_party/` | Vendored upstream sources. Read-only, and versioned inside themselves. See `third_party/PROVENANCE`. |
| `tools/` | Import script and the launcher. Shared. |
| `Makefile` | Dispatches to a release; builds nothing itself. `make`, `make test` and `make install` work from here. |

Inside a release:

| Path | What |
|---|---|
| `src/` | Ported sources — copied from upstream via `tools/import.sh`, then patched. |
| `shim/` | `libv8sys`: modern C standing in for the VAX kernel (54 syscalls), plus `libkmemu` and the kernel-side machine facts. |
| `compiler/` | Only *new* compiler code: the ARM64 backend for `ccom`, `crt0`, `setjmp`. |
| `tests/` | Golden-output fixtures, compiler bootstrap checks, and the build-graph and jail suites. |
| `build/` | Intermediate build output. Not checked in. |
| `rootfs/` | Build output: the V8-shaped tree `$V8ROOT` points at. Nothing runs without it. |

Which directory a command lands in is not this project's choice. V8's `/bin` is
a 56-entry root-filesystem set and most of the world is in `/usr/bin`, so the
Makefile reads the destination out of upstream's own
`usr/src/cmd/Admin/{binfiles,etcfiles,libfiles}` at build time and `tests/wavea`
checks the result against the distribution's shipped directories.

Note what is **not** per-release even though it sits inside one.
`compiler/ccom-arm64/` is about arm64, Mach-O and AAPCS64; `shim/kern/` and
`shim/libkmemu/` are about macOS. A V9 tree inherits their content. The split is
by what varies, and those vary with the *host*, not with the edition.

Phase 5 — a Swift Blit/5620 terminal — **is dropped, and nothing is lost with
it**, for two different reasons PLAN.md §8 keeps apart. The *editing* half is
redundant: `sam` and `acme` reached macOS natively through Plan 9 from User
Space. The *terminal* half is not answered by plan9port at all, and is solved in
a sibling project, `ipad-v8`.

## Importing upstream files

Never edit `third_party/` in place — a hook refuses writes to it, because that
is where the provenance hashes live. To bring a file or directory into a
release:

```bash
tools/import.sh v8/usr/src/cmd/cpp
```

The release is already in the argument — that leading `v8/` — so this lands at
`v8/src/cmd/cpp`, mirroring the upstream path, and records the upstream path and
git blob hash in a `PROVENANCE` file so the diff against pristine V8 is always
reconstructible:

```bash
git hash-object v8/src/cmd/cat.c
```

Every ported program carries a `PORTING.md` recording what changed and **why**,
what was eliminated by measurement, and what is still open. Those are the
project's memory; read the relevant one before touching a program.

## License

Upstream V8 sources are under the Alcatel-Lucent / Nokia Bell Labs
**non-commercial** grant (`third_party/Research-Unix-v8/COPYING.pdf`). This
repository inherits that restriction.
