# Porting notes: ps(1)

`ps` is the first program in this port that reads a **filesystem** this port
implements rather than a file it manufactures. It `getdir`s `/proc`, opens
`/proc/<pid>`, asks `PIOCGETPR` for a `struct proc`, and reads a `struct user`
out of the file at virtual address `UBASE`. That is Killian's process
filesystem — V8's own — and the shim side of it is
`shim/libkmemu/procfs.c`, with the account in `shim/libkmemu/NOTES.md` and
PLAN.md §8a step 3.

It runs, listing every process on the machine. Source changes: **two**, both
recorded below. The rest of what it took was elsewhere.

## What it took that was NOT in this directory

**A compiler bug, and it is the interesting one.** `printp.c:43` calls
`sprintf` with nine arguments. AAPCS64 puts the ninth at `[sp, #0]`, and
`arm64_endfunction()` in `compiler/ccom-arm64/emit.c` was pushing the
callee-saved registers at the bottom of the frame — so the last one saved *was*
`[sp, #0]`. `printp` overwrote the register it had saved on behalf of `main`,
and handed the corrupted value back on return. `main`'s `register struct direct
*dp` came back pointing into `cmdline`, and `ps` walked off the end of its
`/proc` array.

Nothing in 156 Wave A programs plus all of Wave B and Wave C provoked it,
because no call in any of them has more than eight arguments. A sweep of every
object in the tree found exactly four functions that do: `printp`, `df`'s
`dfree` and `main`, and `ls`'s `ngs`. The fix puts the call area beneath the
saves; `tests/v8ccom` has two cases for it, and they need **three frames** to
see it, because the damage lands on the caller of the function making the wide
call.

**`/dev/dk`, `/dev/pt` and `/dev/drum`.** `ps.c:21-28` `getdir`s the first two
and opens the third, calling `error()` and exiting on any failure — before it
touches `/proc` at all. They are Makefile targets, empty, and the emptiness is
the true answer: the jail exposes no disk devices and no ptys, and this world
has no swap. Nothing ever reads the drum, because `ps` consults it only for a
process without `SLOAD` and every process here has it.

**The pid width.** `p_pid` was a `short`. See `src/include/PORTING.md`; without
that change `ps` printed negative pids for every process above 32767.

## The two source changes

### `printp.c` — `%4D`, not `%4d`, for the two size columns

`p_dsize`, `p_ssize` and `p_rssize` are `size_t`, which is 8 bytes here and was
4 on the VAX. `%d` truncates to 32 bits. Not theoretical: the largest size this
prints on the development machine is **1,959,395,120 KB against an `INT_MAX` of
2,147,483,647** — 91% of the way there. A process reserving 2 TB of address
space is routine on macOS and was impossible on a VAX, and it would print
negative.

`%D` is V8's own long conversion (`src/libc/stdio/doprnt.c:189` maps it to `d`
with `longflag`), so this is the port spelling the same intent in the tree's own
idiom rather than importing `%ld`.

**No test can fail on this today**, and that is worth stating rather than
hiding: the host's largest process is just under the boundary. `tests/kmemu`
asserts no size is negative, which is the real invariant; it has never fired.

### `getargs.c` — `long nstack, staddr, stblk`

`nstack = ctob(up->u_ssize)` shifts a `size_t` left by 9. The shift happens at
64 bits and the store narrowed to 32, and the `if (nstack > NSTACK)` clamp is
*after* the narrowing — so a large `u_ssize` could produce `nstack == 0`, after
which `sp = stack + 0; while (*--sp == 0)` reads `stack[-1]` before its own
guard can stop it. `shim/libkmemu/procfs.c` chooses `u_ssize` partly to avoid
that; this makes the arithmetic right rather than leaving it resting on the
choice.

`staddr` and `stblk` go with it because they are computed from `nstack`.

## Known wrong, deliberately not changed

**`getargs.c:38,42,44` walk the stack image with a stride of 4 and a load of
8.** `*(long *)(sp -= 4)` steps four bytes and reads eight; on the VAX `long`
was four bytes and this walked the argv/envp pointer arrays one slot at a time.
The scans cannot find their terminator here — a single 4-byte zero word is not
an 8-byte zero — so they run to the end of the buffer and take the `ucommand`
path, which *looks exactly like correct output for a swapped-out process*.

It is not fixed because **the right fix depends on a decision that has not been
made**. If a stack image is ever manufactured it will come from a 64-bit
process, so the correct code would be stride 8, load 8, and `& ~7` at line 37 —
not the stride-4 form the VAX wanted. Changing it now would be choosing the
answer to a question nobody has asked. It is unreachable today: `pr_read`
serves only `[UBASE, UBASE+4016)` and `/dev/drum` is empty, so both reads come
up short and `getargs` always takes the fallback.

`tests/kmemu` asserts that **every** command reads as `(comm)`, so the day a
stack image exists this becomes a failing test rather than a silent wrong
answer.

## `pbi_nice` does not track `renice` on every host

Measured on a GitHub `macos-14` runner, twice, in different runs:

- `ps -o nice=` reported a `nice -n 10` child ten nicer than its parent while
  `proc_pidinfo(PROC_PIDTBSDINFO)` reported **the same `pbi_nice` for both**.
- On another run `pbi_nice` implied −10 for a child `ps` called 0.

`ps(1)` reads `sysctl kern.proc`; the shim reads `proc_pidinfo`. On the
development Mac the two agree exactly; on that runner they do not — so the `N`
column can be absent for a genuinely renice'd process there, and `-l`'s nice
can be wrong.

Not fixed, and the reason is that the fix is a *choice* rather than a
correction. `sysctl kern.proc` is the interface that demonstrably tracks, and
it is also the deprecated `struct kinfo_proc` that `shim/libkmemu/procfs.c`
checked and set aside on the grounds that libproc could answer. That note is
now half wrong: libproc answers identity and state reliably everywhere, and
**nice only on some hosts**. Moving the one field means carrying both
interfaces, which is worth doing only if the `N` column matters more than the
simplicity does.

`tests/kmemu` reports "not exercised" with both numbers when it observes the
disagreement, rather than asserting through it, and still fails on the two
mutations that matter wherever the interfaces agree.

## Lower-priority width issues, measured and left

- **`fdprint.c:34,62`** print an `off_t` and an inode number with `%d`. Wrong
  above 2 GB. Unreachable: `-f`/`-F` go through `Kread`, which needs
  `/dev/kmemr` and the kernel's inode table.
- **`getuname.c:83`** accumulates the uid from `/etc/passwd` into `d_ino`,
  which is `u_short`. So the uid lookup table is 16 bits while `p_uid` was
  widened to 32 — the widening stopped one field short of its consumer. Wrong
  only for a uid above 65535; the highest on this machine is 501. A directory-
  service account would hit it, and the symptom is `?` in the User column,
  which is a visible gap rather than a wrong name.
- **`ps.c:10`'s `tmpstr[4096]`** is upstream's own latent overflow under `-f`,
  and `DIRSIZ` 14 → 254 lowered the threshold from about 95 open files to about
  14. Dormant for the same reason as `fdprint`.

## What the output does not say, and why

- **The TTY column is `?` for everything.** `gettty()` looks `u_ttyino` up in
  the directory records of `/dev`, `/dev/dk` and `/dev/pt`, and inside the jail
  `/dev` holds one entry. This is a `/dev` question, not a `/proc` one:
  `e_tdev`'s minor does map to `/dev/ttys<NNN>` (measured), but the shim's
  directory snapshot reports a symlink's own inode rather than its target's, so
  the number would not match anything until real device entries exist.
- **Every command reads `(name)`** rather than a full command line — V8's own
  presentation for a process whose stack is not readable. Synthesising a stack
  image from `KERN_PROCARGS2` is possible and is a decision to record here
  before it is taken, not a gap to fill quietly.
- **`%cpu` and the sizes are 0 for other users' processes.** The privilege
  boundary is per *field*, not per process: `PROC_PIDT_SHORTBSDINFO` answers for
  all 614 processes, while the task info that carries memory and cpu is denied
  for the 216 this user does not own. Reported as zero rather than as an error,
  because a process that exists must not answer ENOENT.
- **`WCHAN` is 0** under `-l`. macOS exposes no such value, and a fabricated one
  is the single thing that would make the output a lie.
