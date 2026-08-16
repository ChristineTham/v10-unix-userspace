# calendar(1) — porting notes

Wave A2 batch 2d.  A reminder service: five artefacts from one directory —
three C programs and two shell scripts.  Upstream: `v8/usr/src/cmd/calendar`.

## Changes: NONE

`calendar1.c`, `calendar2.c` and `calendar4.c` are all **byte-identical to
upstream**, and so are the two scripts.

## The shape: the SPELL split, at its largest here

Upstream's install is

```make
cp calendar /usr/bin
cp calendar3 /usr/lib
mv calendar1 calendar2 calendar4 /usr/lib
```

so the **command** is a shell script in `/usr/bin` and four helpers live in
`/usr/lib`: `calendar1` (expands a user list to calendar paths), `calendar2`
(writes an egrep pattern for the next N days), `calendar3` (a second script,
the per-machine half), `calendar4` (filters to readable-not-writable files).
`spell`/`spellprog` and `diff3`/`diff3.sh` are the same split with fewer pieces.

`$(call v8dest,...)` is **not** used for the helpers.  None of `calendar1`..`4`
appears in any `Admin` table, so `Admin/dest` answers `/usr/bin` by
fall-through — "nobody said", not "V8 said" — while the makefile and the shipped
tree both say `/usr/lib`.  Two sources against a non-answer, exactly `cpp`'s
pattern.

## AND IT WIDENED A SWEEP, WHICH FOUND FIVE DISAGREEMENTS AT ONCE

`tests/wavea` compares each program's own makefile against `Admin/dest` and
asserts the disagreement set exactly.  It enumerated **directory names**, so it
could only ever ask about a program named after its directory — true of
everything the port had imported until this one, which makes four programs
called `calendar1`..`4` out of a directory called `calendar`.

Making the candidate set a **union** of directory names and install-line names
took the set from three to eight: the four helpers plus **`diffh`**, which this
port has installed to `/usr/lib` since Wave A and which the sweep had never
examined.  Same class as the `f[3]` bug batch 2c found in the same parser — a
parser correct for every input it had been given.

The union is load-bearing both ways.  Install-line names have to be filtered to
things the shipped tree has as executables, because an install line also names
headers, tables and scripts — and that filter drops `dump`, which V8 never
shipped, so the directory name is what keeps it in.

## `whereis` is imported and NOT installed

`whereis` is a shell script sitting in the source directory that upstream's
makefile never mentions and the shipped tree does not have.  A file in a source
directory is not an artefact.

## Still open

The `calendar` script itself needs `mail` and `rx` (remote exec), neither
ported, so the *delivery* half cannot run — the pipeline builds `${T}6`, a
script of `mail` commands, and running it fails.  Reading a personal
`$HOME/calendar` and printing today's entries works, which is the documented
primary use.  `calendar2`'s pattern and `calendar4`'s filter are asserted
directly in `tests/wavea`.
