# tbl

All 23 files compile with V8's compiler and link freestanding, with no source
changes. It does not yet run.

## Where it is

It dies in `_doprnt` — our libc's, so the fault is a bad argument rather than a
bad formatter — reading address `0x1d7fe`:

```
stop reason = EXC_BAD_ACCESS (code=1, address=0x1d7fe)
frame #0: tbl`_doprnt + 3788
```

The same with input on stdin as from a file, so it is not the input path.

`0x1d7fe` is worth noting: it is *not* the low half of a stack address (those
look like `0x16f...`), so this is unlikely to be the plain pointer-truncation
that `fdprintf` and `sprintf` had in troff. Something is being read as a `%s`
argument that was never a string.

## Where to look

The first `fprintf` tbl reaches is in `t1.c`:

```c
while (gets1(line))
	fprintf(tabout, "%s\n", line);
```

Both arguments were printed before the call and both are sound:

```
tabout=0000000102520820
line=  000000016d8f9970
```

a real `FILE *` and a real stack address. So the fault is **inside** `fprintf`,
not in what it was handed — which moves this out of tbl entirely and into libc.

The distinguishing feature against everything that already works: `tabout` is
`stdout`, and every `fprintf` exercised so far — cat's and cmp's diagnostics —
went to `stderr`. stderr is unbuffered; stdout is not. So the suspect is the
buffered path: `_doprnt` calling `putc`, `putc` calling `_flsbuf`, and `_flsbuf`
allocating the buffer with `malloc` on first use.

`tests/libv8c/run.sh` covers `printf` to stdout and `fputs` to stdout, both of
which take that path and pass, so it is narrower still than "buffered output".
Written as a standalone test — a global `FILE *tabout` assigned `stdout`, a
global `char line[512]`, `fprintf(tabout, "%s\n", line)` — it **works**. So the
crash needs more of tbl's state than its arguments, and the next move is not a
smaller reproduction but a bisection of what runs before it.

Eliminated since:

* **`setinp` does nothing** in the failing case. With input on stdin `argc` is 1,
  so `sargc` is 0 after the decrement and `swapin()` is never called — yet it
  still crashes.
* **`fgets` is correct.** `gets1` fills `line` with `fgets(s, 512, tabin)` and
  then walks it with `while (*s) s++`, so a missing terminator would land
  exactly here. Tested directly: `fgets` reads `hello\n` as six bytes and
  NUL-terminates.

What is left before the first `fprintf` is `signal(SIGPIPE, badsig)` in `main`,
`tabin=stdin; tabout=stdout` in `tbl()`, and `gets1`'s own tail after the fgets
— `while (*s) s++`, the newline strip, and the continuation-line handling.
Instrument `gets1`'s return value and the first sixteen bytes of `line`, which
is the one thing still unprinted at the point of the crash.
