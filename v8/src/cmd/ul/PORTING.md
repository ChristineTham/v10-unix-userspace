# ul(1)

Underlining filter. The first consumer of `libtermcap` here, and the program
that was explicitly deferred out of Wave A2 batch 2 waiting for it.

**One source change**, forced by the target. Two upstream defects deliberately
left alone.

## 1. `ul -t` with the option last — the tenth address-0 crash

`ul.c:38` was `termtype = argv[1]`. With `-t` as the final argument `argv[1]` is
the null the kernel puts after the last argument, and `tnamatch()` dereferences
it on the first comparison. Measured: **exit 139, SIGSEGV**.

That is the same class as the nine already fixed (`refer`, `quot`, `ncheck`,
`unexpand`, `icheck`, `dcheck`, `fsck`, `join`, `yacc`, `hunt`, `nroff`/`troff`)
and the same trigger every time: **the option is the last thing on the command
line**.

The fix is `argv[1] ? argv[1] : ""`, and `""` is not a guard — **it is the
VAX's own answer**, measured rather than recalled:

- V8's shipped binaries are **ZMAGIC** (`od -An -tx1 -N4 usr/bin/ul` gives
  `0b 01 00 00` = 0413), so `N_TXTOFF` is 1024 and virtual address 0 is the
  first byte of **crt0**, not of the header.
- That byte is **`0x00`**, identical in `usr/bin/ul` and `bin/ls`. The full
  sixteen are `00 00 c2 08 5e d0 ae 08 6e 9e ae 0c 50 d0 50 ae`, which read
  through the VAX `struct _iobuf` give `_cnt 0x08c20000` and `_flag 0xd050` —
  the values PLAN.md already records, which is what says the layout is being
  read correctly.

So `*(char *)0` **is** the empty string, and a VAX reached `tnamatch("")`.

What that does is worth writing down, because it is not "nothing":
`tnamatch` returns a match when `*Np == 0` and the entry's first character is
`|`, `:` or NUL — and `/etc/termcap` has **exactly one blank line**, at line
372, which `tgetent` reads as an entry whose first character is NUL. So `""`
*matches* it, `tnchktc()` then looks for the last colon in a zero-length entry,
reads three bytes before its own buffer, and writes `Bad termcap entry` before
returning 0. `ul` takes its own `case 0`, assumes a dumb terminal, and passes
the text through unadorned.

All of that happened on a VAX too. The port reproduces it byte for byte rather
than approximating it with a null check that would have returned early.

## 2. Two upstream defects, NOT fixed, because a VAX had them identically

The option loop has no advance at the end of its body:

```c
	while (argc > 0 && argv[0][0] == '-') {
		switch(argv[0][1]) {
		case 't':
			if (argv[0][2]) termtype = &argv[0][2];
			else { termtype = argv[1]; argc--; argv++; }
			break;
		...
		}
	}
```

- **`ul -tvt100` — the form ul's own usage message documents — HANGS.**
  `argv[0]` is never advanced, so the condition is true forever. Measured:
  still running after 3s, killed.
- **`ul -t vt100` leaves `vt100` as a FILE argument.** The `-t` value is
  consumed but the option itself is not, so the loop exits with `argv[0]`
  pointing at the terminal name. `ul -t vt100 f` prints
  `vt100: No such file or directory` and exits 1.

Neither depends on LP64, Mach-O or the ABI, so neither is forced by the target
and S1 says record rather than patch — the same verdict as `make`'s `meter()`,
`ls.c:257`'s unchecked `malloc` and `bcd`. **The working spelling is the
environment**: `TERM=vt100 ul file`, which is what `man` and `nroff` pipelines
actually use, and what `tests/wavea` exercises.

Worth knowing when reading the hang: measuring it needed care. A first attempt
used a deadline wrapper written for the occasion, and it reported `rc=0` for
the hanging case — an instrument written five minutes earlier being wrong, the
shape CLAUDE.md records about `tests/crash-probe.sh`. Timing the process
directly is what settled it.

## 3. What is tested, and what the negative control is for

`tests/wavea` derives the expected output **from `/etc/termcap`** rather than
transcribing it: it pulls `us=` and `ue=` out of the vt100 entry, strips the
leading padding digits, decodes `\E`, and requires `ul` to emit exactly that.
So the case tests that ul found the entry and decoded it, not that a string was
typed correctly twice.

The `TERM=dumb` case is the control that makes it mean something: `dumb` has no
`us`/`ue`, so the same input must come out plain. Without it, a `ul` that
always emitted `\E[4m` would pass.

## Still open

- **`ul -i`** is a whole second mode (`iul()`, `doulg()`, `dographic()`),
  reached only by that flag, and nothing exercises it. It has its own
  1985 buffers — `linebuf[BUFSIZ]`, `genbuf[BUFSIZ]` — with a bound check that
  `goto ovflo`s, so it is guarded upstream; but it is unread here.
- **`tgetflag("os")` sends ul to `execv("/bin/cat", argv[-1])`** for
  overstriking terminals, and `argv[-1]` is one before the current argument —
  correct only because `main` advanced it. No terminal in the cases has `os`,
  so that arm is unexercised.
