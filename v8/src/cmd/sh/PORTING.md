# The Bourne shell, ported

All 24 source files compile with V8's own compiler and link freestanding. The
shell starts, reads its profile logic, parses, forks, execs and reaps.

## What runs

All of it, as far as the test suite goes (`tests/sh/run.sh`, 21 cases):

```
echo hello world                      hello world
for i in a b c; do echo $i; done      a b c
echo one | tr a-z A-Z                 ONE
if true; then echo yes; fi            yes
echo a > f1; cat f1                   a
echo `echo bq`                        bq
set -- p q; echo $# $1 $2             2 p q
f() { echo infunc; }; f               infunc
case abc in a*) echo matched;; esac   matched
echo ${HOME}                          /Users/christie
test -d /tmp && echo isdir            isdir
```

`$((...))` is correctly a syntax error: arithmetic expansion postdates 1985.

## The last one: a null pointer that used to read as zero

`x=42` hung, and so did every other variable assignment. It looked like a
corrupt name tree — `findnam` walks one — but instrumenting `findnam` to print
its argument showed it never reached the walk:

```
F[]
```

The name printed as nothing at all, because `write(2, NULL, 3)` writes nothing.
`com[0]` is null: a bare assignment leaves `argn` 0 and no command word.
`findnam` passes it to `chkid`, whose loop

```c
while(!ctrlchar(*nam) && (*nam&QUOTE)==0 && *nam!='(' && *nam!='=')
	nam++;
```

is bounded only by the string's terminator. **On a VAX or PDP-11 running Unix,
address 0 read as zero** — so `ctrlchar(0)` was true and the loop stopped on the
first character. The shell depends on that without saying so, and so does a good
deal of V7-era code.

macOS reserves the entire first 4GB as `__PAGEZERO` and it cannot be mapped, so
the read faults. Guarded at the call site in `xec.c`, which is the one place
that can know the word is optional.

## The four porting changes, all LP64

Each is in the file that exists to hold the assumption, and each is marked
`PORT:` in place.

| Where | What | Why |
|---|---|---|
| `mode.h` | `Rcheat` casts to `long`, and the `_cheat` union holds one | The shell's own pointer/integer pun. `addblok` computes `(struct blk *)(Rcheat(bloktop) + reqd)` with `reqd` unsigned, so once the cast says `int` the *addition* is 32-bit and C is right to truncate. |
| `defs.h` | `round()` casts to `long` | Applied to pointers as well as sizes: `rndstak = (char *)round(staktop, BYTESPERWORD)`. |
| `brkincr.h` | `BRKINCR`/`BRKMAX` scaled ×4 | Everything in the arena grew: `BYTESPERWORD` is 8, and `struct namnod` went from about 20 bytes to 64. |
| `stak.c`, `name.c` | `stakroom(n)`, called by `getstak` and by `staknam` before it copies | See below. |

## The hang that was not a crash

Three of these presented identically, and none of them looked like a memory
problem: **the shell hung, in the child of a fork, with the parent asleep in
`wait`.** That reads as a fork or wait bug.

It was not. A store landed on an unmapped page, the handler `stdsigs()` had just
installed returned, and the faulting instruction retried — forever. A fault that
would be a clean `SIGSEGV` in a program without handlers becomes a silent spin
in one with them.

The last of the three is worth stating on its own, because it is not a width
problem at all. `staknam()` copies a whole `"name=value"` to `staktop` with
`movstr()` and only *afterwards* tells `getstak()` how many bytes it used. So
the guaranteed headroom has to cover the **longest single item**, not a fixed
step. `BRKINCR` was 512 bytes, which was ample for a 1985 environment; `PATH` in
the environment this was debugged in is 4315 bytes. `stakroom()` now reserves
for the actual string before the copy, so the size of the environment stops
mattering.

## One that was ours, not the shell's

`fork` was returning the same value in parent and child. XNU's fork syscall
returns two values — x0 holds a pid in *both* processes, and x1 is 0 in the
parent and 1 in the child — and the shim was reading only x0 through `rawsys0`.
Both processes carried on as the shell, neither execd, and the real parent
waited forever. Fixed in `shim/v8sys/syscall.c`, which already had the same
two-register handling for `pipe`.

## Where rung 5 stops for sh, and it is arm64 that stops it

`sh` is not in `tests/jail`'s rung-5 sweep, and the reason recorded here for a
while was wrong. PLAN.md said the blocker was "`msg.o` — a generated source".
`msg.c` is checked in, and always was. Nothing about it is generated.

What upstream's makefile actually does with it is this:

```
msg.o:		msg.c $(FRC)
			CC=$(CC) AS=$(AS) CFLAGS="$(CFLAGS)" sh ./:fix msg
```

and `:fix` — a build helper whose name begins with a colon, so it matches no
glob and is invisible the way `t..c` and `dextern` are — is:

```sh
for i do
	$CC $CFLAGS -S -c $i.c
	ed - <<\! $i.s
	g/^[ 	]*\.data/s/data/text/
	w
	q
!
	$AS -o $i.o $i.s
done
```

Compile to assembly, **rewrite `.data` to `.text`**, reassemble. That is the
VAX shared-text optimisation: the tables land in the read-only text segment, so
every shell process on the machine maps one copy of them instead of getting a
private writable page each. In 1985, with a shell per login, that was real
memory.

`ctype` works here. `msg` cannot, and the reason is structural rather than
fixable:

| file | what it holds | outcome |
|---|---|---|
| `ctype.c` | a character table — no relocations | assembles, links, runs |
| `msg.c` | `struct sysnod commands[]`, a table of **pointers** | `ld: Found illegal text-relocations` |

An initialised pointer in `__TEXT` needs its value written at load time, which
is a text relocation. Mach-O refuses them, and the usual escape is not
available: `-no_pie` is **ignored for arm64**, so position independence is
mandatory and the address can never be resolved statically the way a.out
resolved it at link time. This is not a flag away from working. It is the
target saying no.

So `sh` sits beside `cpp` in PLAN.md §4a's second category — programs whose own
build *description* names the target machine — rather than in the sweep. That
category had one member and looked like a curiosity about `-Dvax=1`; it has two
now, and the shared property is sharper than the flag: **both are 1985 asking
for initialised data in read-only text.** `cpp` asks with `:yyfix` and `cc -R`
(which V8's driver passes through to `as -R`); `sh` asks with `ed`.

`tests/jail` asserts the boundary rather than leaving it unmentioned: that the
rewrite happens, that the result assembles, and that the link then fails on a
text relocation. If a future change makes that link succeed, it is a real change
in what this port can claim and it has to come past a failing test to say so.

### What did come out of it

`:fix` invokes `$AS` **by name**, and V8's make supplies `AS=as` from its
built-in macros. That is the first thing in this port to exec the assembler
without going through `cc`, and it found `hosttools[]` in
`shim/v8sys/syscall.c` listing only `/usr/bin/clang` — while PLAN.md §1 has
sanctioned `as`, `ld`, `ar`, `strip` and `nm` since the beginning. The array
was not wrong so much as unexercised, the same shape as `v8s_mknod` passing its
path unresolved because `mkdir(1)` had never been built. `as` is spelled there
now, and `tests/jail` checks both directions: `as` permitted, `nm` — on the
prose list, but still not execed by anything — refused.
