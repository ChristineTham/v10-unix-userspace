# v10-unix-userspace

Research Unix userspace, rebuilt from source on macOS (Apple Silicon).

Bell Labs' own C compiler compiles Bell Labs' own C library and programs, on
hardware that did not exist in 1985. Not emulation, and not a compatibility
layer for old binaries — everything is compiled from the authentic sources.

Currently **V8** (Eighth Edition, 1985). V9 and V10 follow.

## Requirements

- macOS on Apple Silicon (the compiler backend targets ARM64/AAPCS64 and Mach-O)
- Xcode command line tools

Nothing else — no package manager, no network.

## Install

```bash
make -j8 && make install
```

Installs to `~/.local/share/v8`, with a launcher at `~/.local/bin/v8`. No `sudo`.

To share one image across accounts instead:

```bash
sudo make install PREFIX=/usr/local/v8 BINDIR_HOST=/usr/local/bin
```

## Use

```bash
v8
```

```
Research Unix, Eighth Edition.
You are christie.  Your files are in /usr/christie and they persist.
$ cc -o hello hello.c && ./hello
```

| command | effect |
|---|---|
| `v8` | enter your world, in your home directory |
| `v8 DIR` | start in `DIR` instead |
| `v8 --pure` | refuse every host binary except the toolchain exception |
| `v8 --golden` | enter the pristine image, read-only |
| `v8 --reset` | discard your world and rebuild it from the image |
| `macos CMD` | run a Mac command from inside |

Your world lives in `~/.v8` and persists: files you write, programs you build
and install, `/etc` files you edit all survive relaunching, reinstalling, and
`make clean`. Only `v8 --reset` destroys it. Set `$V8WORK` for a throwaway
world elsewhere.

`macos` exists because 115 of the world's 198 commands share a name with one of
the Mac's, including `make`, `cc`, `sh` and `ls`. Running a Mac *application*
from inside is fine; running a Mac *build* is not, because it would find V8's
compiler. Build on the Mac, work in V8.

## What you get

198 of the 285 commands V8 shipped, including:

- **Languages** — `cc` (Bell Labs' own), `f77`, `efl`, `ratfor`, `awk`, `bc`/`dc`, `yacc`, `lex`
- **Editors** — `ed`, `ex`/`vi`, `qed`
- **Typesetting** — `troff`, `nroff`, `eqn`, `tbl`, `pic`, `grap`, `refer`
- **Analysis** — `lint`, `cflow`, `cyntax`, `struct`, `spell`, `diff`
- **Filesystems** — `mkfs`, `fsck`, `icheck`, `dcheck`, `dump`, `restor`
- the shell and the usual filters

## Limitations

**The assembler and linker are the host's.** `as`, `ld`, `ar`, `strip` and `nm`
are not ported, because the object format is Mach-O; `cc` execs `clang` to
assemble and link.

**No terminal graphics.** There is no Blit/5620 emulator, so the graphical
editors and browsers are absent.

**No networking, mail or daemons.** `uucp`, `mail`, `cron` and `at` need a
spool, a network or a running daemon.

**No C++.** `cfront` is written in a 1985 dialect no current compiler accepts,
and the only compiler that can read it is `cfront` itself.

**No machine-level tools.** `shutdown`, `mount` and `init` would act on the
host rather than on the world.

Of the 87 commands not present, 45 shipped with no source at all. Each of the
rest has a recorded reason; see [PLAN.md](PLAN.md).

## Development

```bash
make -j8              # build (about 10s from clean)
make test             # 2876 cases across 17 suites
make -C v8 test-deps  # a single suite
```

Run `make test` on its own — `make -j8 test` races the suites against their own
builds.

After `make install` the build tree carries the installed path and has no test
stamp; restore it with `make clean && make -j8 && make test`.

A longer crash probe sits outside `make test`:

```bash
v8/tests/crash-probe.sh "$PWD/v8/rootfs" /tmp/probework
```

## Layout

| Path | What |
|---|---|
| `v8/` | the Eighth Edition port; `v9/` and `v10/` become siblings |
| `v8/src/` | ported sources — imported from upstream, then patched |
| `v8/shim/` | modern C standing in for the VAX kernel, plus `/proc` and kernel machine facts |
| `v8/compiler/` | new code only: the ARM64 backend for `ccom`, `crt0`, `setjmp` |
| `v8/tests/` | the 17 suites |
| `v8/rootfs/` | build output — the tree `$V8ROOT` points at |
| `third_party/` | vendored upstream sources, read-only |
| `tools/` | import script and launcher |

Where each command installs is read from upstream's own
`usr/src/cmd/Admin/{binfiles,etcfiles,libfiles}` at build time, so `/bin` and
`/usr/bin` hold what Bell Labs shipped.

To bring in a file from upstream — never edit `third_party/` in place:

```bash
tools/import.sh v8/usr/src/cmd/cpp
```

That records the upstream path and git blob hash in a `PROVENANCE` file, so the
diff against pristine V8 stays reconstructible. Each ported program has a
`PORTING.md` recording what changed and why.

## Documentation

- **[PLAN.md](PLAN.md)** — scope, fidelity contract, phases, port policy
- **[ARTICLE.md](ARTICLE.md)** — how it was built, and what went wrong
- **[CLAUDE.md](CLAUDE.md)** — working notes and the bug classes that recur

## License

Upstream V8 sources are under the Alcatel-Lucent / Nokia Bell Labs
**non-commercial** grant (`third_party/Research-Unix-v8/COPYING.pdf`). This
repository inherits that restriction.
