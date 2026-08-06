# cp

One of eleven commands imported into a directory of their own and never built.

**No changes.** Compiles under v8cc with zero warnings and is clean on every
hazard this port audits: no undeclared pointer-returning function, no K&R
parameter holding a pointer, no `sizeof(int) == sizeof(char *)` assumption, no
pointer cast to `int`, no variadic call reaching libSystem, no raw directory
read, no `exec`. `nm -u rootfs/bin/cp` is empty — nothing resolves out of the
host's libc.

Recorded because "nothing to change" is a measurement, not an absence of one.

The one fixed buffer, `iobuf[4096]`, doubles as the destination-path buffer and
is unbounded — but 4096 exceeds macOS's `PATH_MAX` of 1024, and the `stat()`
that precedes the join cannot succeed on a longer string anyway. Verified with a
1089-character target: clean failure, no crash.

`cp` matters beyond itself: `mv` execs `/bin/cp` for a cross-device move, and
while `cp` was unbuilt that `execl` reached **the Mac's** `/bin/cp`, which never
calls `rootpath()`. See `src/cmd/mv/PORTING.md`.
