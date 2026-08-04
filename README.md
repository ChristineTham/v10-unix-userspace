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

| Path | What |
|---|---|
| `third_party/` | Vendored upstream sources. Read-only. See `third_party/PROVENANCE`. |
| `src/` | Ported sources — copied from upstream via `tools/import.sh`, then patched. |
| `shim/` | `libv8sys`: modern C standing in for the VAX kernel (63 syscalls). |
| `compiler/` | Only *new* compiler code: the ARM64 backend for `ccom`, `crt0`, `setjmp`. |
| `tools/` | Import script and build glue. |
| `tests/` | Golden-output fixtures, compiler bootstrap checks, and the build-graph and jail suites. |
| `build/` | Intermediate build output. Not checked in. |
| `rootfs/` | Build output: the V8-shaped tree `$V8ROOT` points at — `bin`, `lib`, `usr/include`, `usr/lib`. Nothing runs without it. |

Planned, not yet present: `blitterm/` (Swift Blit/5620 terminal app, Phase 5).

## Importing upstream files

Never edit `third_party/` in place. To bring a file or directory into `src/`:

```bash
tools/import.sh v8/usr/src/cmd/cpp
```

This copies it to the mirrored path under `src/` and records the upstream path
and git blob hash in a `PROVENANCE` file, so the diff against pristine V8 is
always reconstructible.

## License

Upstream V8 sources are under the Alcatel-Lucent / Nokia Bell Labs
**non-commercial** grant (`third_party/Research-Unix-v8/COPYING.pdf`). This
repository inherits that restriction.
