# load

Runs, unchanged. **Not one line of `load.c` was edited** — the whole port is in
the shim, which is the right place for it.

```
$ load
    1m    5m   15m
   4.9   3.3   2.6
$ uptime
 7:12  up 13:41, 1 user, load averages: 4.91 3.34 2.64
```

## What it actually does, and why that needed a manufactured kernel

`load` does not ask the system for the load average. It looks up the **address**
of the kernel's `_avenrun` in the kernel's own symbol table, then seeks to that
address in `/dev/kmem` and reads three doubles out of it:

```c
nlist(dunix, nl);                       /* dunix = "/unix"     */
if ((kmem=open(dkmem,0))<0) ...         /* dkmem = "/dev/kmem" */
kseek(kmem,nl[0].n_value,0,maddr);
kread(kmem,avenrun,sizeof(avenrun));
```

That is how every groveler on a V7-shaped system worked, and `w` and `ps` do the
same with more symbols. So the shim manufactures a kernel: a namelist at `/unix`
saying where things are, and a `/dev/kmem` in which they are there.

**The addresses are ours to choose** — nothing else in this world has an opinion
about them — so `shim/libkmemu/kmem.c` has one table that assigns them and
generates *both* files from it. Two lists agreeing by hand would be a standing
invitation: get them out of step and the program reads the wrong bytes and
prints them without complaint. `tests/kmemu` mutates the namelist into lying
about the address and checks that the output goes empty.

A symbol with no honest source deliberately gets **no row**. It is then absent
from `/unix`, `nlist` leaves `n_type` zero, and the program says so in its own
words — PLAN.md §7's sentinel rule, one level down. Better a groveler that
reports it cannot find a symbol than one handed a fabricated value.

## `nlist(3)` wanted a real a.out, so it got one

`src/libc/gen/nlist.c` is authentic V8 libc and this port compiles it unchanged,
so `/unix` has to be the format it expects rather than something convenient: a
`struct exec` that passes `N_BADMAG`, a symbol table at `N_SYMOFF`, and a string
table after it.

**Under LP64 those are not their 1985 sizes.** Every field of `struct exec` is a
`long`, so the header is 64 bytes where the VAX had 32; `struct nlist` is 24, its
8-byte union followed by type/other/desc and four bytes of padding before the
8-byte value. Nothing has to interoperate with a 1985 file — the only reader is a
V8 program compiled against the same headers — but the two ends must agree, and
a disagreement yields a plausible number rather than a failure. `tests/kmemu`
asserts both sizes *from the V8 side*, which is the end that has to be right.

One detail that is easy to lose: the string table starts with four bytes of
filler, because `nlist` **skips any symbol whose `n_strx` is 0** — it uses zero
to mean "no name". Real linkers put the table's own length there. Remove the
filler and the first symbol silently vanishes; that is mutation-tested.

## Two additions to the jail

`/dev/` joined the redirect list, and `/unix` needed something new: an
**exact-match** list beside the directory prefixes. There is no way to spell a
bare file in the prefix list — an entry of `"/unix"` without a trailing slash
matches by prefix, so it would also claim `/unixfoo`. Cheap to do properly, and
the alternative is a rule that is wrong only for names nobody has created yet.
Tested by creating `rootfs/unixfoo` and checking the V8 world cannot see it.

`/dev/` does **not** capture `/dev/null` or `/dev/tty`, by the same mechanism
that protects every other entry: a path whose rootfs copy does not exist falls
through to the host, and the rootfs has neither. Worth knowing rather than
assuming — create `rootfs/dev/tty` and the V8 world would stop seeing the real
terminal. `tests/kmemu` asserts those three names stay absent.

`/dev` did not exist at all, and nothing else had a reason to create it, so
`kmemu_replace` makes the parent directory it needs. That is the right owner:
the library is manufacturing the file, and a missing parent is part of that job
rather than a build step someone has to remember.

## Known limits

- **`/dev/kmem` is a snapshot per `open(2)`, not a live window.** The shim
  regenerates it when a reader opens it, so plain `load` — the default — reads
  the current average. `load 5` loops on the *same* descriptor, so every
  iteration re-reads the same three numbers where a real `/dev/kmem` would show
  them moving. Not worked around, because the workaround is a device driver.
- **`kseek`'s `maddr` is an undeclared K&R parameter**, so it is `int` while the
  caller passes a `long` (`0` or `0x80000000`). At `maddr=0` — the `/dev/kmem`
  path, and the only one this port takes — `offset & ~0` is the offset and
  nothing is lost. The `/dev/mem` variant would sign-extend. Authentic, left
  alone, noted so the next reader does not mistake it for something this port
  introduced.
- The file is 4120 bytes for one symbol at `0x1000` — mostly zero. That is
  honest: everything between the symbols is memory this port cannot speak for,
  and reading it gets zeroes rather than a guess.
