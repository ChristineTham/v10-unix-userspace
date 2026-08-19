# dc(1) -- porting notes

`dc` is the arbitrary-precision reverse-Polish calculator, and it is the
program `bc(1)` compiles *to*: bc parses, emits dc's language, and execs it
down a pipe. So dc being right is a precondition for bc being right, which is
how the defect below was finally found.

It was imported in an early wave and had **no PORTING.md until now**, which is
itself the finding worth recording: it predates this project's rule that an
import gets audited and written up.

Upstream: `usr/src/cmd/dc/{dc.c,dc.h}`, blobs
`4a65dfd511d4beb01f8e58b6c0cba12ea922dcb1` and
`b0ede7616821a202c4961f79d14129ff441df06b`.

## The one change: an off-by-one that used to be harmless

`init()` ends by threading every entry of the symbol table onto a free list:

```c
sp = sptr = &symlst[0];
while(sptr < &symlst[TBLSZ]){		/* upstream */
	sptr->next = ++sp;
	sptr++;
}
sptr->next=0;
```

The loop runs while `sptr < &symlst[TBLSZ]`, so its last iteration sets
`symlst[TBLSZ-1].next` to `&symlst[TBLSZ]` -- a pointer past the array -- and
leaves `sptr` there. The `sptr->next=0` below then **stores at
`&symlst[TBLSZ]`**, one element beyond the end.

`struct sym` is two pointers. That is 8 bytes on a VAX and **16 here**, so the
array is 2048 bytes there and 4096 here, and what sits immediately after it is
a different object on the two machines. Measured on the linked binary:

```
_symlst  0x10002af88
_tenptr  0x10002bf88      <- symlst + 0x1000, exactly its end
```

So `init()` zeroed `tenptr` -- the block holding the constant 10 -- **four
statements after building it**.

`TBLSZ-1` is what the code already means: link `0..TBLSZ-2` to their successors
and terminate `TBLSZ-1`. It removes the write past the end *and* the second
half of the same defect, which no symptom had ever reached: with upstream's
loop, `symlst[TBLSZ-1].next` points outside the array, so a dc program that
exhausted the symbol table would have been handed `&symlst[TBLSZ]` as a free
slot.

### Why this is fixed rather than recorded

The overrun is upstream's, on any machine -- so at first reading it looks like
the `cb`/`calendar4` shape, where a defect a VAX shared is deliberately frozen
so that repairing it is a decision.

It is not that shape. It is **`sed -n l`'s**: *"LP64 and Mach-O can turn an
upstream out-of-bounds access into a fault -- `trans[]` is an array of
pointers, so the stride doubled, and Mach-O maps nothing where a.out did."*
There it was a read; here it is a **write**, and it lands on a live pointer.
The port cannot host the undefined behaviour even though the VAX got away with
it, and the frozen-bug treatment would enshrine a total failure of the
program's purpose.

## The symptom, which pointed everywhere except at the cause

dc **crashed on any number with an odd count of fractional digits**:

```
$ printf '4 p\n.4 p\n5 p\n' | dc
4                     (and the 5 is gone too; exit 139)
```

Parity, because dc stores numbers **base 100**, two decimal digits to a byte:
`.44`, `.4444`, `1.00` and `4.34` were exact, while `.4`, `.444`, `1.0` and
`0.5` produced nothing. `readin()` ends `scale(p,dpct)`, `scale()` opens
`add0(p,n)`, and `add0`'s only odd arm is `if(ct == 1) t = mult(tenptr,q)`.
The even path never touches `tenptr`, so it never dereferenced the pointer
`init()` had zeroed.

Five things about the diagnosis generalise:

- **"exit 0" was my own pipeline.** The first characterisation of this bug said
  *exit 0, no diagnostic, silently consumes the rest of the input*. It exits
  **139** -- SIGSEGV -- and the rest of the input is simply never read. `$?` had
  been taken from the end of a `| tr` pipeline. This tree's most-cited shell
  hazard, in the instrument used to describe the bug it was pointed at.
- **Adding a print to the trace crashed the even path too**, and that was the
  finding rather than a broken instrument: the added expression was
  `length(tenptr)`, so the *print* dereferenced what the code had not yet
  reached. An instrument that crashes where the program does not has usually
  just told you which object is bad.
- **ASLR said the pointer was real.** `tenptr` read `0x300000068` on one run and
  `0x109680068` on the next -- a value that moves between runs is a genuine
  address, which is what separated "init never set it" from "something cleared
  it afterwards".
- **The bisect was three prints, not a debugger.** Printing `tenptr` at `init`
  exit and `readin` entry located it between them in one build; `lldb` is no use
  here because v8cc emits no unwind info.
- **The layout was the proof.** `nm -n` on the linked binary showed `_symlst`
  and `_tenptr` exactly `0x1000` apart, which is `TBLSZ * sizeof(struct sym)`.
  A hypothesis about an overrun becomes a fact when the neighbour is named.

## What the existing tests could not have found

dc had **three** cases in this tree -- `2 3+`, `3 4*`, `6 7*` -- and all three
are whole numbers. A calculator whose fractional arithmetic is untested is a
calculator whose defining feature is untested, and the defect above made *every
fraction* fail. `tests/wavea` now covers the fractional path, both parities,
and the `4 p / .4 p / 5 p` sequence that shows the crash took the rest of the
input with it.

## Audited and deliberately NOT changed

- **`0.4` where GNU dc prints `.4`.** Upstream's own formatting: `tenot()` does
  `printf("%d.", c/10)` for the odd branch, which prints `0.` when the leading
  base-100 digit is below ten. The even branch reaches `OUTC('.')` instead and
  prints `.44`. The asymmetry is in Bell Labs' source and is what a VAX printed;
  the tests assert V8's answer, not the host's.
- **`int log10;` in `dc.h`**, which collides with the C library's `log10`.
  `dc.o` defines its own and the linked binary carries exactly one, so the
  archive member is never pulled. `tests/kmemu` already tracks `dc/log10` as one
  of the six live common-versus-text pairs.
- **`dc.h`'s `int (*signal())();` and the K&R handler.** These are why the same
  source will not compile under a modern clang, which is worth knowing: the
  experiment "does the bug follow the source or v8cc" costs a set of `-D` and
  `-Wno-` flags, and was not needed once the layout named the cause.
