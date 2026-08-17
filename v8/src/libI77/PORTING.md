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

## `wrt_E` read the wrong arm of a union, and it is correct code on a VAX

`wrtfmt.c:251` (upstream's own line, pristine `:249`) is

```c
	if (len == sizeof(float)) dd = p->pf; else dd = p->pd;      /* by length */
	...
	if(p->pf != 0) dp -= scale;                                 /* NOT by length */
```

The value is chosen by length and the exponent decision then reads the **float**
arm unconditionally. `lwrt_F` calls this with a `double`, so `p->pf` is that
double's first four bytes.

**On a VAX this is exact.** D_floating's leading 32 bits have the identical
layout to F_floating — sign, 8-bit exponent, then fraction — so reading a double's
first word as a float yields the same value, nonzero exactly when the double is
nonzero. On IEEE little-endian those four bytes are the **low mantissa bits**.

So it failed for tidy numbers and worked for untidy ones, which is the worst
shape a numeric defect can have — the values anyone would write a test with are
exactly the ones it breaks:

| written | printed | correct |
|---|---|---|
| `0.375` | `3.750000000e+00` | `e-01` |
| `0.0375` | `3.750000149e-02` | correct |
| `37.5` | `3.750000000e+00` | `e+01` |
| `375.0` | `3.750000000e+00` | `e+02` |
| `1.0e10` | `1.000000000e+10` | correct |
| `1.0e-10` | `1.000000013e-10` | correct |

All seven predicted exactly by asking whether the double's first four bytes are
zero. Fixed to `if(dd != 0)`, which is the value the function had already chosen
— forced by the target in the same sense as `values.h`'s IEEE arm and
`arm64.c`'s `realcon[]`: the floating-point FORMAT changed, not the code's intent.

**It is libI77's and not the compiler's**, established by calling `do_lio` from C
with `/lib/f1` nowhere in the picture and reproducing it byte for byte. It had
been there since stage 1; nothing had ever printed a REAL through this port.

**`tests/wavea` carries TWO cases, and the untidy one is the control.** A "fix"
that simply always applied the scale would pass `0.375` and break `0.0375`, so
the pair is what discriminates rather than a duplicate. Mutation-verified:
reverting the line fires the tidy case and leaves the untidy one green.
