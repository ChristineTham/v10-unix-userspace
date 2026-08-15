# Porting notes: cpio(1)

Imported with the Wave A2 batch. **One forced change**, and — more interestingly
— **one measured decision not to change a second crash**, which is the part
worth reading.

*Named `cpio.PORTING.md` for `mkfs.PORTING.md`'s reason: upstream keeps it as a
bare `cmd/cpio.c` and `tools/import.sh` mirrors the upstream path.*

## The change: argv[1] is dereferenced before anything else happens

`main`'s third statement is

```c
	if(*argv[1] != '-')
		usage();
```

A bare `cpio` has no `argv[1]`, so this reads the NULL the kernel plants at
`argv[argc]`. On the VAX that landed on address 0, which is the first byte of
**crt0** — V8's binaries are ZMAGIC, so `N_TXTOFF` is 1024 and the a.out header
is never mapped — and that byte is `0x00`. Not `'-'`, so cpio printed its usage
and exited 2. macOS leaves page 0 unmapped, so the identical code SIGSEGVs.

The guard is `argv[1] == 0 ||` in front, which reproduces the VAX's answer
rather than merely dodging the fault. Measured after: usage on stderr, **exit
2**, and `cpio -o | cpio -i` still round-trips a file.

This is PLAN.md §4i's class, and the same shape as `ncheck`/`icheck`/`dcheck`'s
`-i` number loop and `diffh`'s option loop.

## The NON-change: `cpio -i` on unreadable input, and why a VAX faulted too

The crash probe reports **two** signal deaths for cpio. The second is not the
argv class at all, and it is deliberately left alone.

`cpio -i` with nothing readable on stdin reaches `chgreel()`, which is the
change-the-tape prompt:

```c
again:
	fprintf(stderr,"If you want to go on, type device/file name when ready\n");
	devtty = fopen("/dev/tty", "r");
	fgets(str, 20, devtty);
```

`fopen` is unchecked. **V8's `/dev/tty` is a hard link to `/dev/fd/3`**, and
opening anything in `/dev/fd` is `dup(2)` — so with no descriptor 3 the open
fails and `devtty` is NULL.

What decides the verdict is whether the page-0 access that follows is a **read**
or a **write**:

- `fgets` is a `getc` loop (`src/libc/stdio/fgets.c`).
- `getc(p)` is `(--(p)->_cnt>=0? (int)*(p)->_ptr++:_filbuf(p))`.
- `_cnt` is the **first** member of `struct _iobuf`, so this is a
  read-modify-**write** at virtual address 0.
- ZMAGIC text is read-only shared, so a VAX took a **protection fault** there.

There is therefore no VAX answer to restore, and §1 says a change to `src/` must
be forced by the target. Third member of that family, after `pr.c`'s `Ttyin` and
`troff/hc.c`'s `rcf` — both audited and left alone for the identical reason.
PLAN.md §4i has the pair; note that `fflush(NULL)` reads `_flag` and returns
harmlessly one line away in the same header, which is why the read/write
distinction is the whole question rather than a detail.

### Measured, both ways round

The fault is a property of descriptor 3, not of cpio:

```
$ cpio -i < /dev/null                    # no fd 3
errno: 0, Can't read input
If you want to go on, type device/file name when ready
Segmentation fault: 11               (rc 139)

$ sh -c 'exec 3</dev/null; cpio -i < /dev/null'   # fd 3, as v8launch.sh arranges
errno: 0, Can't read input
If you want to go on, type device/file name when ready
                                     (rc 2)
```

So under the `v8` launcher, which opens the terminal as fd 3 the way `init.c`
does, this path does not fault at all.

**It also explains an lldb result that looked like a contradiction.** Under
`lldb`, `cpio -i` exits 2 and never crashes — because the debugger leaves a
descriptor 3 open. A backtrace attempt was the wrong instrument here: it changed
the one variable that decides the outcome. (`instruments-can-measure-themselves`
again — two runs must differ in exactly one thing.)

## Guarded

`tests/wavea` asserts the bare-`cpio` answer (usage, exit 2), that the usage
text is upstream's unchanged, and that `-o`/`-i` still round-trip. It also
sweeps all 53 invocations of the four Wave A2 arrivals with **fd 3 explicitly
closed** and asserts the surviving crash list is exactly `cpio -i` — so the
non-change above is a case rather than a sentence. Mutation M6 repairs
`chgreel()` and that case fires, which is what says it is not vacuous.
