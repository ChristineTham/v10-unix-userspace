# Porting notes: tar(1)

Imported with the Wave A2 batch. **One forced change**, the address-0 class.

*Named `tar.PORTING.md` for `mkfs.PORTING.md`'s reason: upstream keeps it as a
bare `cmd/tar.c` and `tools/import.sh` mirrors the upstream path.*

## `-b` with no number

V7 tar takes its options as **one bundled argument** — `tar cbf 2 arch f1` —
which is why the crash probe found only **one** invocation of 53: the probe
passes `-b`, and every other letter falls to `default:` and exits through
`usage()`. `f` consumes an argument too but never dereferences it.

```c
	case 'b':
		nblock = atoi(*argv++);
```

With `b` in the last bundle and nothing after it, `*argv` is the vector's NULL
terminator — and `main` sets `argv[argc] = 0` itself, so it is null whatever
`execve` left there. `atoi` then dereferences it.

On the VAX that read address 0, which is crt0's first byte (`0x00`, since V8's
binaries are ZMAGIC and the a.out header is never mapped), so `atoi` saw an
empty string and returned 0 — and **the very next line rejects 0**:

```c
		if (nblock > NBLOCK || nblock <= 0) {
			fprintf(stderr, "Invalid blocksize. (Max %d)\n", NBLOCK);
			done(1);
		}
```

So a VAX printed `Invalid blocksize. (Max 40)` and exited 1. macOS leaves page 0
unmapped, so the same code SIGSEGVs. Restored as:

```c
		nblock = atoi(*argv++? argv[-1]: "");
```

`argv[-1]` rather than a plain guard so that **the increment still happens on
the null path**, as it did on the VAX. That is unobservable today — `nblock` is
0, so `done(1)` runs two lines below and `argv` is never read again — but
"unobservable today" is exactly the sort of qualifier this port has been bitten
by (see `recorded-diagnoses-are-hypotheses`), and the exact form costs nothing.

Fourth instance of this loop shape after `ncheck`, `icheck` and `dcheck`, and a
fifth counting `cb -l`.

## Measured

```
$ tar b                  Invalid blocksize. (Max 40)      rc 1
$ tar tbf                Invalid blocksize. (Max 40)      rc 1
$ tar cbf 2 a.tar f1 f2  (creates, 4096 bytes)            rc 0
$ tar tbf 2 a.tar        f1 f2                            rc 0
$ tar xbf 2 a.tar        (extracts both, contents exact)  rc 0
```

The last three are the paired cases: a guard that merely returned early would
pass the first two and break `-b` entirely.

**One instrument error worth recording.** The first attempt to verify this used
`tar -c -b 2 -f arch.tar f1`, GNU-style, and reported `tar: -b: cannot open
file` — which reads as a port defect and is not one. V7's option loop is
`for (cp = *argv++; *cp; cp++)` over a **single** string, so separate `-x`
arguments are filenames. The invocation was wrong, not the program.

## Guarded

`tests/wavea` asserts survival and the `Invalid blocksize` text for `tar b`, and
that `tar cbf 2` still creates, lists and extracts. Mutation M4 fires 3 cases.
