# newgrp(1) — porting notes

Wave A2 batch 2d.  Changes the caller's group and execs a shell.  One source,
56 lines, installs to `/bin` because `Admin/binfiles` says so.

Upstream: `v8/usr/src/cmd/newgrp`.

## Changes: NONE

`newgrp.c` is **byte-identical to upstream**, and it needed nothing because the
three libc functions it depends on were imported by Wave A2 batch 1:
`getgrnam`, `getpwuid` and `getpass`.  Before that batch, five programs were
resolving those from `-lSystem` and reading the **Mac's** databases from inside
the jail; `tests/kmemu`'s libc-import sweep is what caught it, and `nm -u` on
this binary is empty.

## What it does here

It reads the jail's `/etc/passwd` and `/etc/group`, checks membership, and then

```c
if(setgid(grp->gr_gid) < 0)
	perror("setgid");
done();
```

`setgid(2)` is a raw host syscall in this port and fails `EPERM` — the binary is
not setuid and the caller is not root.  `su(1)` records the same thing from the
other side: there are three different uid-0s here and none of them is a login.
Upstream's install is `chown root; chmod u+s`, which this port does not do and
should not: the host decides what a setuid exec means, and it would be an
escalation on the Mac rather than root over the jail.

## THE THING TO KNOW BEFORE TESTING IT: `done()` EXECS A SHELL

Every exit path goes through

```c
done() { setuid(getuid()); ...close fds...; execl(pw->pw_shell, "sh", 0); }
```

so `newgrp` with no arguments prints a usage message and **replaces itself with
an interactive shell**.  Run without redirecting stdin it takes the terminal,
which reads exactly like a hang — the shape `src/cmd/ex/PORTING.md` spends a
page on, arriving from the program's side rather than the harness's.  Measured:
it cost two minutes of a wedged session before the cause was obvious.

Two consequences.  Any test must supply stdin (`</dev/null` makes the shell read
EOF and exit).  And the usage message is **lost** when stdout is a pipe, because
V8's stdio buffers and `execl` replaces the image without flushing — upstream's
own behaviour, not a port defect.

For the crash probe this is safe rather than `MUTATES`: the probe gives every
program `/dev/null`, so the shell it becomes reads EOF and exits without
executing anything.  Measured across all 53 invocations, zero signal deaths and
no change to the rootfs.

## Still open

Nothing.  `newgrp` behaves exactly as an unprivileged `newgrp` should.
