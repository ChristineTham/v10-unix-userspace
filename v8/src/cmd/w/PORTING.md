# w (and uptime)

Two programs from one binary, and only one of them runs here.

```
$ uptime
  9:11am  up 15:41,  1 users,  load average: 2.34, 2.47, 2.49
$ /usr/bin/uptime                       # the Mac's, for comparison
 9:11  up 15:41, 1 user, load averages: 2.34 2.47 2.49
$ w
No mem
$ echo $?
1
```

The uptime line is exact — same uptime, same three averages — and the format
differences are all V8's: a 12-hour clock with `am`/`pm`, `1 users` with no
plural logic, and commas between the averages.

`w` itself refuses, in its own words, and that is the intended outcome rather
than an unfinished edge. The rest of this file is why.

## One binary, two names, and the name is the argument

Upstream's makefile installs `w` and then `ln`s `/usr/bin/uptime` to it. `w`
reads `argv[0]`:

```c
firstchar = login ? argv[0][1] : (cp==0) ? argv[0][0] : cp[1];
...
if (firstchar != 'u')
	readpr();
```

So `uptime` is `w` with a different name, and `w -u` is the same path reached by
a flag. This port keeps the hard link rather than copying, because a copy lets
the two drift and nothing would say so; `tests/deps` asserts they share an inode
even after either name is deleted.

## What runs, and what it needed

The uptime path reads exactly two kernel symbols, and `libkmemu` has an exact
answer for both:

| Symbol | Source | Used for |
|---|---|---|
| `_bootime` | `sysctl kern.boottime` | `now - bootime`, the "up 15:41" |
| `_avenrun` | `sysctl vm.loadavg` | the three load averages |

`_avenrun` was already there for `load`. `_bootime` is one new row in
`shim/libkmemu/kmem.c`'s table, and it brought its namelist entry and its
`/dev/kmem` bytes with it — which is what having one table drive both files is
for.

Everything else the uptime path needs already existed: `/dev/kmem`, `/etc/utmp`
(for the user count, the same file `who` reads), and `nlist(3)`.

**A `time_t` here is eight bytes, where the VAX had four**, so the row is sized
`sizeof(long)` rather than a literal 4. A disagreement between the two ends
would give a plausible uptime rather than an error, so `tests/kmemu` asserts
`sizeof(time_t)` from the V8 side and compares the value in `/dev/kmem` against
`kern.boottime` exactly. Boot time does not move, so unlike the load average
this can be an equality check rather than a bracket.

## Why the full form cannot run: it is 1981 Berkeley code

`w.c` opens `@(#)w.c 4.4 (Berkeley) 6/5/81`. Its `readpr()` finds each process's
u-area by walking **VAX page tables**:

```c
pte = &Usrptma[btokmx(mproc.p_p0br) + szpt-1];
lseek(kmem, (long)pte, 0);  read(kmem, &apte, sizeof(apte));
lseek(mem, ctob(apte.pg_pfnum), 0);   /* /dev/mem */
read(mem, pagetbl, sizeof(pagetbl));
for (cc=0; cc<UPAGES; cc++) { ... }   /* the u area, one page at a time */
```

and `getargs()` recovers the command line the same way, falling back to
`/dev/drum` — the swap device — via `vstodb()`, which walks a VAX swap map.

There is nothing here to walk. The seven remaining symbols — `_proc`, `_nproc`,
`_swapdev`, `_nswap`, `_ecmx`, `_Usrptmap`, `_usrpt` — describe a proc table
reached through page tables and a swap device, and none of them has an honest
source on Darwin. Under PLAN.md §7's sentinel rule they get **no row**, so
`nlist` leaves `n_type` zero.

`readpr()` then opens `/dev/mem`, finds nothing, and says `No mem`. That is the
program reporting the truth with its own diagnostic, which is exactly what the
sentinel rule is for. `tests/kmemu` asserts the message and the exit status, so
if a future change ever manufactures a `/dev/mem`, this fails and the decision
gets argued rather than discovered.

Verified by hand that the guard is sensitive rather than incidental: create an
empty `rootfs/dev/mem` and `w` moves on to `No drum`, the next honest refusal.

## The one source deviation

```c
-	if (nl[0].n_type==0) {
+	if (nl[X_AVENRUN].n_type==0 || nl[X_BOOTIME].n_type==0) {
 		fprintf(stderr, "No namelist\n");
```

`nl[0]` is `_proc`. On a VAX all nine symbols were present together or absent
together, so testing the first was a sound proxy for "is there a namelist at
all". This port's kernel answers for some and not others, so the proxy no longer
means what it meant — and it fires **before** the `firstchar` branch, so leaving
it would fail `uptime`, which reads neither of the symbols it is testing.

The replacement names the two the path below actually reads. It is forced by the
target in the same way `df`'s deviation is: there is no superblock to read
there, and there is no proc table to find here. The full-`w` path is unaffected
— it reaches `/dev/mem` next and refuses.

That is the only change to `w.c`.

## Two LP64 hazards, both measured clean

`w.c` has the two shapes this port keeps getting bitten by, and neither bites.
Both were checked by reading the emitted code rather than by reasoning about the
ABI.

**`char *fread();`** — declared at line 92, while `fread` returns an `int` count.
The caller reads eight bytes from `x0`; AAPCS64 leaves the upper 32 undefined for
an `int` return, and `if (fread(...) == NULL)` compares all eight. Garbage up
top would mean the utmp loop never terminates.

It does terminate, and structurally: v8cc ends every such return with

```
	ldrsw	x9, [x29, #-4]
	mov	x0, x9
```

`ldrsw` defines the whole register. Measured directly too — the returned value
is `1` then `0`, with nothing above bit 31.

Worth carrying forward: it is `ldrsw`, sign-extending, because the conversion is
to the function's `int` return type. A function whose value legitimately has bit
31 set would come back **negative** to a caller that declared it `char *` or
`long`. That is the `sconvert()` family again — a same-size value where the sign
is the whole question. Not a bug here, since these are counts.

**`calloc` undeclared** in `readpr()`, its result cast to a pointer — the classic
LP64 truncation. v8cc keeps the full width (`mov x9, x0`), and a probe returns a
pointer above 4 GB that round-trips. Dead code in this port anyway, but the
answer is the same one that matters for the tree at large.

## What this leaves, and where it goes

Full `w` is not abandoned; it is **blocked on `/proc`**, which is PLAN.md §8a
step 3.

That turned out to be the same blocker `ps` has, and finding out why changed the
plan. PLAN.md §7 had said `ps` would be ported "on top of `libproc`". Reading
V8's `ps` says otherwise:

```c
prlist = getdir("/proc", 0);                    /* ps.c */
fd = open(strcat(strcpy(sstr, "/proc/"), s), 0);/* doselect.c */
Ioctl(fd, PIOCGETPR, pp);                       /* struct proc */
Sread(fd, UBASE, up);                           /* the u-area, by address */
```

**V8's `ps` is a `/proc` client** — Killian's process filesystem, which is V8's
own invention (`sys/pioctl.h`, `sys/sys/proca.c`). It carries no `sccsid` and no
Berkeley attribution, unlike `w`. So the two process tools in this one tree are
from different eras: `ps` had already moved to `/proc`, and `w` — imported from
Berkeley four years earlier — never did.

The consequence is that a `/proc` server answers both of them, and answers `ps`
*natively* rather than by emulation. Recorded in PLAN.md §7 and §8a.
