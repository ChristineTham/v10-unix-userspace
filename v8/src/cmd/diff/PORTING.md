# Porting notes: diff(1) and diffh(1)

The `diff` directory builds two programs: `diff` itself (installed to `/bin`)
and **`diffh`**, installed to `/usr/lib` and invoked by `diff -h`. The Makefile
passes `-DDIFFH='"/usr/lib/diffh"'` so diff can find it.

`diff`, `diffdir.c` and `diffreg.c` needed no source changes. `diffh.c` needed
**one**, the address-0 class.

## `diffh`: the option loop is main's first statement

```c
main(argc,argv)
char **argv;
{
	char *s0,*s1;
	FILE *dopen();
	while(*argv[1]=='-' && argv[1][1]!=0) {
```

`*argv[1]` runs before anything is opened, so **a bare `diffh` faults
immediately** — and so does the run after the loop has consumed the last option,
since `argv[1]` is then the vector's NULL terminator again. That is why the
crash probe reported **53 of 53**: bare plus every single-letter option.

On the VAX both spellings read address 0, which holds crt0's first byte, `0x00`
(V8's binaries are ZMAGIC, so `N_TXTOFF` is 1024 and the a.out header is never
mapped). `0x00` is not `'-'`, so the loop simply did not run and the test four
lines below reported the real problem:

```c
	if(argc!=3)
		error("must have 2 file arguments","");
```

macOS leaves page 0 unmapped, so the same code SIGSEGVs. Restored as:

```c
	while(argv[1]!=0 && *argv[1]=='-' && argv[1][1]!=0) {
```

**The guard goes on the first test only, and that is deliberate.**
`argv[1][1]` is reached only once `*argv[1]=='-'` has passed, which a null
`argv[1]` cannot do on either machine — so `-` alone still ends the loop, which
is what makes `diffh -` (stdin, by convention) behave as it always did. Guarding
both would have been harmless and would also have said something false about
where the fault was.

## Measured

```
$ diffh                    diffh: must have 2 file arguments    rc 1
$ diffh -b                 diffh: must have 2 file arguments    rc 1
$ diffh f1 f2              2,$c2,$ / < b / --- / > c            rc 0
$ diffh -b f1 f2           2,$c2,$ / < b / --- / > c            rc 0
```

The last is the paired case, and it is the one that matters: a guard written as
an early `return` in front of the loop would pass the first three and silently
stop `-b` from ever being consumed.

## One thing to know before trusting a `$?` here

`diffh`'s `main` ends in a **bare `return;`** — a K&R main with no value — so it
hands back whatever is in the register. Any harness that decides "did it die on
a signal?" from `$?` alone can therefore misread a normal exit as a signal, in
exactly the way the crash probe once reported 254 deaths where there were 96.
`tests/wavea`'s floor sweep reads the real wait status through `fork`/`waitpid`
for this reason.
