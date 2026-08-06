# rmdir

One of eleven commands imported and never built. Zero warnings under v8cc; one
change, the same class as `mkdir`'s.

## `name[500]` → 1024

`strcpy(name, d)` is the **first statement** in `rmdir()`, before any `stat`, so
the argument alone overruns the buffer and the path need not even exist. Three
`strcat` calls further down add to it. Measured: `SIGBUS` from 550 characters,
well inside macOS's `PATH_MAX` of 1024. (SIGBUS rather than SIGSEGV is what a
misaligned return address gives on ARM64.)

Same fix as `mkdir`'s, one buffer larger, so it takes a long *path* rather than
a single long component.

## Verified clean, and worth recording because it is the obvious suspect

`rmdir` is the only raw directory reader among the newly built commands:

```c
struct direct dir;
while(read(fd, (char *)&dir, sizeof dir) == sizeof dir) {
        if(dir.d_ino == 0) continue;
```

It sizes with `sizeof dir`, never a literal 14 or 16, so the record is
2 + 254 = 256 bytes and matches what the shim writes. The `d_ino == 0` skip is
the classic 16-bit-inode trap and is already closed upstream of the program:
`v8sys_fold_ino()` folds to 16 bits and returns 1, never 0, so a real entry
cannot be skipped.

`stat("")` at line 47 is V7's "empty path means the current directory", which
POSIX makes ENOENT. Already handled in `v8sys_rootpath` — deliberately there
rather than in `vpath`, so every filesystem type gets it.

## Not changed, upstream's own

- Line 48: `fprintf(stderr, "%s: cannot stat \", cmdname\"")` — the escapes make
  the whole thing one format string and `cmdname` is never passed. The port's
  `doprnt` prints garbage and does not crash. Unreachable in practice: it needs
  `stat(".")` to fail.
- Line 40: `rindex` returns a pointer *at* the slash, so the `.`/`..` guard
  compares `"/."` against `"."` and never fires for a qualified path. Fails
  safe — `rmdir ./d/.` removes nothing and exits 0. Should be `np+1`.
