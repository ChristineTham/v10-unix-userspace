# v10-unix-userspace

Porting Research Unix userspace to macOS (Apple Silicon) and Linux/ARM64.

Starting with **V8** (Eighth Edition, 1985) as the beachhead, then V9, then
**V10** — the last Research Edition, and the original goal of the project.

The point is not a compatibility layer that runs old binaries. It is to rebuild
the V8 world from source with the *authentic* Bell Labs C compiler, running
against the *authentic* V8 C library, on a machine that did not exist in 1985.

See **[PLAN.md](PLAN.md)** for the full scope: fidelity contract, target model,
phase breakdown, per-program port policy, and risk register.

## Layout

The repository holds a **series** of ports, so it is split by what varies per
release and what does not.

| Path | What |
|---|---|
| `v8/` | The Eighth Edition port — everything below, plus its own `Makefile`. `v9/` and `v10/` become siblings. |
| `third_party/` | Vendored upstream sources. Read-only, and versioned inside themselves. See `third_party/PROVENANCE`. |
| `tools/` | Import script and build glue. Shared. |
| `Makefile` | Dispatches to a release; builds nothing itself. `make`, `make test` and `make install` work from here as before. |

Inside a release:

| Path | What |
|---|---|
| `src/` | Ported sources — copied from upstream via `tools/import.sh`, then patched. |
| `shim/` | `libv8sys`: modern C standing in for the VAX kernel (63 syscalls), plus `libkmemu` and the kernel-side machine facts. |
| `compiler/` | Only *new* compiler code: the ARM64 backend for `ccom`, `crt0`, `setjmp`. |
| `tests/` | Golden-output fixtures, compiler bootstrap checks, and the build-graph and jail suites. |
| `build/` | Intermediate build output. Not checked in. |
| `rootfs/` | Build output: the V8-shaped tree `$V8ROOT` points at — `bin`, `etc`, `lib`, `usr/bin`, `usr/include`, `usr/lib`. Nothing runs without it. |

Which of those a command lands in is not this project's choice. V8's `/bin` is a
56-entry root-filesystem set and most of the world is in `/usr/bin`, so the
Makefile reads the destination out of upstream's own
`usr/src/cmd/Admin/{binfiles,etcfiles,libfiles}` at build time and `tests/wavea`
checks the result against the distribution's shipped directories.

Note what is **not** per-release even though it sits inside one.
`compiler/ccom-arm64/` is about arm64, Mach-O and AAPCS64; `shim/kern/` and
`shim/libkmemu/` are about macOS. A V9 tree inherits their content. The split is
by what varies, and those vary with the *host*, not with the edition.

This listing used to end "planned, not yet present: `blitterm/` (Swift
Blit/5620 terminal app, Phase 5)". **Phase 5 is dropped, and nothing is lost
with it** — for two different reasons, which PLAN.md §8 keeps apart. The
*editing* half is redundant: `sam` and `acme` reached macOS natively through
Plan 9 from User Space. The *terminal* half is not answered by plan9port at all
— it is no 5620 emulator — and is solved in a sibling project, `ipad-v8`.

## Importing upstream files

Never edit `third_party/` in place. To bring a file or directory into a release:

```bash
tools/import.sh v8/usr/src/cmd/cpp
```

The release is already in the argument — that leading `v8/` — so this lands at
`v8/src/cmd/cpp`, mirroring the upstream path, and records the upstream path and
git blob hash in a `PROVENANCE` file so the diff against pristine V8 is always
reconstructible.

## License

Upstream V8 sources are under the Alcatel-Lucent / Nokia Bell Labs
**non-commercial** grant (`third_party/Research-Unix-v8/COPYING.pdf`). This
repository inherits that restriction.
