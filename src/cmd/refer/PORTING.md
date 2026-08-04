# refer

All five programs build and link with V8's own compiler: `refer` itself and the
four it execs — `mkey`, `inv`, `hunt`, `deliv`. They are installed into
`rootfs/usr/lib/refer/`, which is where refer looks for them, and the shim
resolves `/usr/lib/...` inside `$V8ROOT`.

40 of the 46 `.c` files compile untouched. **No source changes were needed.**

## Not built: whatabout

`flagger.c`, `kaiser.c`, `thash.c`, `what1.c`, `what2.c` and `what4.c` do not
compile, and are not built. They belong to `whatabout`, which is not in
upstream's own `all` target either. They use the pre-C89 initialiser
`int x 5;` — no `=` — which V8's own grammar already rejects. Skipping them
loses nothing that `all` built.

## Not finished: refer needs a V8 `/bin`

refer runs, gets as far as its helpers, and then does not produce output,
because of one line in `glue2.c`:

```c
savedir()
{
	if (refdir[0]==0)
		corout ("", refdir, "/bin/pwd", "", 50);
	trimnl(refdir);
}
```

`corout` runs the program with `execl(rprog, "deliv", arg, 0)`, so `/bin/pwd`
is invoked with an **empty extra argument**. V7's `pwd` ignored arguments;
macOS's `/bin/pwd` answers `usage: pwd [-L | -P]` and exits, so `refdir` stays
empty and refer cannot find its way back to the working directory.

The right fix is not to patch refer — the code is correct for the system it was
written for. It is that **`/bin/pwd` should be V8's `pwd`**, which this port
already builds (Wave A).

What that needs:

* `rootpath()` in `shim/v8sys/syscall.c` currently redirects `/usr/lib/`,
  `/usr/share/`, `/usr/dict/`, `/lib/` and `/usr/pub/` into `$V8ROOT`.
  `/bin/` is deliberately **not** on that list, because `system(3)` and
  `popen(3)` exec `/bin/sh` and that has to keep working.
* Adding `/bin/` means the V8 world gets its own `/bin`, including its own
  `sh` — which this port has, and which passes 21 tests. That is *more*
  authentic, not less, but it changes what every `system()` call in the tree
  runs, so it wants its own change and its own test pass rather than being
  folded in here.

Until then refer builds, links, and starts its helpers correctly; it is the
`pwd` round trip that is missing.

## What it did cost: execl

refer's first symptom was not a hang but an **interactive shell** — `sh: no job
control in this shell`, then `sh-3.2$`. `system()` was reaching the host's
`execl`, which is variadic, so the `-c` and the command string never arrived and
`/bin/sh` started as a login shell.

`execl`, `execv` and `execle` were missing from `libv8c.a` — V8 kept them in
`libc/sys` as assembly, which the shim replaced. They are now in
`src/libc/gen/exec.c`. See the comment there, and the guard in
`tests/libv8c/run.sh` that now checks no variadic libc function is left for the
host to supply.
