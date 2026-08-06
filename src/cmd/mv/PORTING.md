# mv

Imported and, for a long time, not built at all — see the note in the Makefile
beside `V8DIRBIN`. Eleven commands were in that state, because the completeness
check globbed `src/cmd/*.c` and `mv` lives at `src/cmd/mv/mv.c`.

Upstream compiles under v8cc with **zero warnings** and needed one change.

## The change: `MAXN` 100 → 1024

`MAXN` is not an independent number. mv's own guard is

```c
if (strlen(target) > MAXN-DIRSIZ-2) {
        fprintf(stderr, "mv :target name too long\n");
```

which states a *relationship* between the buffer and the longest name that can
be appended to it. V7 had `MAXN` 100 and `DIRSIZ` 14, leaving 84 for the
directory part.

This port raises `DIRSIZ` to 254 (`src/include/PORTING.md`), so
`MAXN-DIRSIZ-2` is **−156**. `strlen()` is never less than that, so the guard
fired on every directory-into-directory move:

```
$ mv a b            # b exists and is a directory
mv :target name too long
```

**A bug this port introduced, not one V8 had**, and the message is false — the
name is fine. It was found by the `lp64-auditor` subagent reading the source
before the program was wired into the build, which is what step 2 of the
porting workflow is for; nothing about the program's behaviour on ordinary
files would have suggested it. v8cc emits the constant literally, so it is
checkable rather than deduced:

```
	bl	_strlen
	mov	x10, #-156
	cmp	x9, x10
	b.le	...          ; never taken
```

1024 is macOS's `PATH_MAX` — the longest path mv can be handed, and the longest
the join may produce — so the guard means what it says again and the budget for
the directory part goes from 84 to 768.

It also pulls back an *upstream* hazard that the `DIRSIZ` change had widened.
`move()` does `sprintf(buf, "%s/%s", target, dname(source))` into `char
buf[MAXN]` with no length check at all. That overflow is V8's own and needed
`strlen(target) > 85` in 1985; with `DIRSIZ` 254 and a 100-byte buffer it needed
only a 92-character name, which is an ordinary filename on this host. At 1024 it
needs a 769-character directory path. Not removed — it is upstream's code and
the contract says changes must be forced by the target — but no longer
reachable by accident. `tests/waveb` moves a 200-character name through that
exact path.

## Not changed: directory rename cannot work on this target

`mvdir()` renames a directory the V7 way — `link(source, target)`, rewrite the
`..` entry, `unlink(source)`. macOS refuses `link()` on a directory
unconditionally:

```
host link(dir,dir)   = -1  errno=1 (Operation not permitted)
host rename(dir,dir) =  0
```

So `mv a b` on directories reports `mv: cannot link b/a to a` and stops. The
shim's `dotlink()` escape hatch (`shim/v8sys/syscall.c`) does not help: it
covers a *basename* of `.` or `..`, which these are not.

This is left alone and asserted rather than fixed, on the same terms as `w`'s
`No mem` — a property of the target, stated so that changing it later is a
decision and not a discovery. The fix, when it is wanted, is `rename(2)`, which
does the link, the `..` rewrite and the unlink atomically and is what makes the
whole of `mvdir` unnecessary. That is a rewrite of authentic source, so it wants
its own argument; it is not a one-token change like `MAXN`.

Consequence worth knowing: the `..`-rewriting code at `mvdir`'s heart, and
`check()`'s walk up the tree, are unreachable on this host.

## `/bin/cp` is a runtime dependency the build graph cannot see

When `link()` fails across devices, mv forks and `execl("/bin/cp", ...)`. That
is a dependency on another program, and nothing in any makefile expresses it.

While `cp` was among the eleven unbuilt commands this mattered more than it
reads: `rootpath()` resolves into the rootfs only if the copy is there, so with
`rootfs/bin/cp` missing mv would have exec'd **the Mac's** `/bin/cp`, which
never calls `rootpath()` — a cross-device move doing its work on host paths
while mv's own `stat`/`link`/`unlink` used jail paths. Both are built now, and
`tests/jail` asserts `/bin/cp` and `/bin/mv` exist rather than excusing them,
which is what it used to do.
