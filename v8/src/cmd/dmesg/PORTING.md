# dmesg(8) — porting notes

Wave A2 batch 2d.  Prints the kernel message buffer.  One source, 176 lines,
`@(#)dmesg.c 4.3 (Berkeley) 2/28/81`, installs to `/etc` because
`Admin/etcfiles` says so.

Upstream: `v8/usr/src/cmd/dmesg`.

## Changes: NONE

`dmesg.c` is **byte-identical to upstream** and every header it wants —
`<nlist.h>`, `<sys/vm.h>`, `<sys/msgbuf.h>` — is already in the rootfs from
`third_party`'s pristine set.  It compiled and linked first time.

## What it does here

```
$ dmesg
Aug 16 20:19
No namelist
```

exit 1.  `nlist()`s `/unix` for `_msgbuf`, does not find it, and says so.
`shim/libkmemu/kmem.c` manufactures a namelist and a `/dev/kmem` from one table
and that table holds `_avenrun` and `_bootime`; adding `_msgbuf` would mean
manufacturing a kernel message buffer, which is a decision rather than a side
effect of importing a program.  So dmesg is **deliberately not linked against
`libkmemu`** and reports honestly, which is `load(1)` and `w(1)`'s precedent.

## What it found, and it was not in dmesg

**`nlist(3)` dereferenced the caller's list terminator.**  `dmesg` SIGSEGV'd on
its ordinary invocation.  The bug is in `src/libc/gen/nlist.c`, in a file
byte-identical to upstream, and dmesg is simply the first program in this port
to take the path that reaches it: the matching loop stops at the requested
symbol, so every caller whose symbols are *present* never walks to the
terminator, and `load` and `w` ask for exactly the two the shim manufactures.
`dmesg` asks for one that is absent — which is the honest-refusal path — and
faulted on the way to reporting it.  `src/libc/gen/PORTING.md` has the account.

## `dmesg -` RECURSES UNTIL THE STACK GOES, AND IT IS BERKELEY'S

51 of the crash probe's floor entries are this one bug.  Measured: `dmesg -a`
prints `No namelist`, then `can't open buffer` about eight thousand times, then
SIGSEGV.

```c
done(s) char *s; {
	if (s && ... ) { pdate(); printf(s); }
	if (wflg) writebuf();          /* <-- */
	exit(s!=NULL);
}
writebuf() {
	if ((f = open(BUFFER, 1)) < 0)
		done("can't open buffer\n");   /* <-- back into done */
	...
}
```

Unbounded mutual recursion whenever `wflg` is set and `/usr/adm/msgbuf` cannot
be opened.  `BUFFER` is opened `O_WRONLY` with **no `O_CREAT`**, so a missing
file is fatal in this sense rather than creatable.

**Every single-letter option except `-i` sets `wflg`**, which is why the count
is 51 and not 53:

```c
switch (argv[1][1]) {
default:  wflg++;  break;     /* -a ... -z, -A ... -Z, and bare `-'  */
case 'i':          break;     /* the one arm that does not           */
}
```

Bare `dmesg` never enters the block at all, so it is the one invocation that
reports cleanly.

**It is upstream's on upstream's hardware and therefore stays.**  The archive
ships no `/usr/adm` at all — it is runtime state, like `/etc/utmp` and `/tmp`,
created when a system is installed rather than shipped in a tarball — and nine
other programs (`log`, `ac`, `init`, `savecore`, `date`, `last`, `sa`, two troff
drivers) reference it.  On a running V8 with the directory but no `msgbuf`,
`open(BUFFER, 1)` fails identically and a VAX recursed identically.  S1 forbids
the change: this is not a defect the target introduced.

Recorded in `tests/crash-probe.floor` rather than patched, and worth saying out
loud there that 51 lines are one mechanism — the same way `lex`'s 53 are three.

## Still open

Whether `libkmemu` should manufacture `_msgbuf`.  It would make `dmesg` print
something, and everything it printed would be invented.  Note that it would
*also* not fix `dmesg -`, which fails on the buffer file rather than on the
namelist — the two are independent.
