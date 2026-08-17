# libI77

See **`src/libF77/PORTING.md`**, which covers both halves of the Fortran
runtime as one deliverable. They are not two ports: the f77 driver's library
list is a fixed `{ "-lF77", "-lI77", "-lm", "-lc" }` (`drivedefs`), in that
order, and the order is load-bearing — `cabs`, `sinh`, `cosh`, `tanh` and
`ecvt` are defined by `libv8c` too, and `-lF77 -lI77` preceding `-lc` is what
makes the Fortran ones win.

The two findings that live on this side:

- **V8's shipped `libI77.a` references `setvbuf` and `_bufendtab`, which appear
  in no other archive Bell Labs shipped.** So no Fortran program on a real V8
  could reach `ld`'s exit. `shim/libI77/sysv.c` closes it.
- **libI77 ships its own `stdio.h`, which is System V's**, and it disagrees
  with V8's about the layout of `FILE` — `_flag` is `char` where V8's is
  `short`, putting `_file` at offset 25 rather than 26. It must never be
  compiled against; `tests/deps` asserts that on content rather than on a make
  edge, because these objects have no `.d` files and a `nodep` would pass
  either way.
