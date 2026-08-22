#!/bin/sh
#
# v8 -- enter the Research Unix Eighth Edition world.
#
# You stay in your own home directory and keep your own files.  What changes is
# the SYSTEM around them: /bin, /lib, /etc and /usr belong to V8, and the tools
# on PATH are Bell Labs' rather than the Mac's.  Your Mac is still there --
# every path the V8 world does not claim falls through to the host, so
# /Users/you, /Volumes and /usr/bin/python3 all still resolve.
#
# The jail is per-BINARY, not per-process-tree: host binaries are not linked
# against libv8sys, so they never consult the V8 root and see the real macOS
# with no special case.  Anything the V8 cc compiles does link it, and is
# jailed by construction.  That is why python3 needs no exemption and why you
# cannot compile your way out.
#
# INSTALLED WORLD, NOT A COPY OF ONE.  $V8ROOT below is the tree `make install'
# wrote.  It is ordinary installed software under your own prefix: writable, so
# V8's own Admin/Mk can `cp prog /bin' -- the port's central claim -- and
# replaced wholesale by the next `make install', the same as any other tool.
# Your work does not live there, so an upgrade costs you nothing.

V8ROOT=@ROOT@
export V8ROOT

usage() {
	cat <<EOF
usage: v8 [--pure] [--help] [directory]

  (no options)  enter the V8 world, in your own home directory
  [directory]   start somewhere else instead
  --pure        refuse every host binary except the documented
                assembler/linker exception, and drop the host PATH.  A
                strictly V8 world: python, git and make(1) are not reachable.
                For checking that something really is V8 code

Inside:  macos CMD   run CMD on the Mac, with the Mac's PATH
         exit        leave
EOF
}

pure=0
while [ $# -gt 0 ]; do
	case $1 in
	--pure)  pure=1; shift ;;
	--help|-h) usage; exit 0 ;;
	--) shift; break ;;
	-*) echo "v8: unknown option $1" >&2; usage >&2; exit 2 ;;
	*) break ;;
	esac
done

if [ ! -d "$V8ROOT/bin" ]; then
	echo "v8: no world at $V8ROOT -- run 'make install' first" >&2
	exit 1
fi

# WHO YOU ARE.  V8's own /etc/passwd names Bell Labs people with 1985 uids, so
# getpwuid() would not find whoever is actually here and `ls -l' would print
# numbers instead of names.  The installer writes an entry for the account that
# ran it; this adds one for anybody else, which only matters for a shared
# install under /usr/local.  If the tree is not writable we carry on -- a
# numeric ls -l is a cosmetic loss, not a reason to refuse to start.
_u=$(id -un)
if ! grep -q "^$_u:" "$V8ROOT/etc/passwd" 2>/dev/null; then
	if [ -w "$V8ROOT/etc/passwd" ]; then
		_gecos=$(id -F 2>/dev/null) || _gecos=$_u
		echo "$_u:*:$(id -u):$(id -g):$_gecos:$HOME:/bin/sh" \
			>> "$V8ROOT/etc/passwd"
	fi
fi

# THE HOST PATH IS SAVED BEFORE IT IS SHADOWED, and that is what makes
# `macos CMD' possible: 115 of this world's commands share a name with one of
# the Mac's, so a native build started from in here would otherwise find V8's
# make and V8's cc.  macos(1) restores this and execs.
V8HOSTPATH=$PATH
export V8HOSTPATH

# /etc is on PATH because V8's own sh has it there -- chgrp, chown and the
# section-8 tools live in /etc, which Admin/etcfiles decides and this port
# follows.
#
# The host's PATH is APPENDED rather than dropped, so python3, git and node
# work by name.  --pure is what drops it: there the V8 world is the whole of
# PATH, and an unported tool is conspicuous rather than silently satisfied by a
# modern namesake.
if [ "$pure" = 1 ]; then
	PATH=$V8ROOT/bin:$V8ROOT/usr/bin:$V8ROOT/etc
	V8JAIL=strict; export V8JAIL
	V8HINT="Pure mode: no host binary is reachable."
else
	PATH=$V8ROOT/bin:$V8ROOT/usr/bin:$V8ROOT/etc:$PATH
	V8HINT="Your Mac is still here: $HOME, and 'macos CMD' runs a Mac command."
fi
export PATH V8HINT

# THIS SCRIPT IS THE INIT OF THIS WORLD, and fd 3 is the one thing an init has
# to leave behind.  V8 has no /dev/tty driver: /dev/tty is a hard link to
# /dev/fd/3 and opening it is dup(3) (man4/fd.4, sys/sys2.c:174).  What makes
# fd 3 the terminal is cmd/init.c:368-382, which opens the tty as fd 0 and then
# dups it THREE times -- to 1, 2 and 3.  So pr -p, dump and troff -a can only
# find a terminal if something does this first.
#
# If there is no controlling terminal -- a pipeline, cron, CI -- fd 3 is left
# closed, which is the honest answer rather than a fallback: fd.4 says open
# returns -1 then.
#
# PROBE IN A SUBSHELL, THEN COMMIT.  Two shorter spellings are both wrong,
# measured: `exec 3<>/dev/tty 2>/dev/null' applies its redirections left to
# right, so the diagnostic escapes before 2>/dev/null is in place; and a failed
# redirect on exec terminates a non-interactive POSIX shell outright.  Probing
# first means the exec only ever runs when it will succeed.
( : <>/dev/tty ) 2>/dev/null && exec 3<>/dev/tty

# WHERE YOU LAND, AND WHY IT IS NOT DIRECTLY THERE.
#
# v8(1) chdirs to its argument and then execs /bin/sh with argv[0] `-', a LOGIN
# shell -- and V8's sh reads `.profile' FROM THE DIRECTORY IT STARTS IN, not
# from $HOME: main.c:109 is `pathopen(nullstr, profile)'.  Measured, with $HOME
# and the cwd deliberately different.
#
# So landing straight in your Mac home would make a 1985 Bourne shell read a
# .profile written for zsh or bash.  `export PATH="$PATH:..."' is not 1985
# syntax -- it wants two lines, an assignment then an export -- and the shell
# says so, at length, before every session.
#
# Landing at the world's root instead means the .profile read is OURS, and it
# is the thing that puts you where you asked to be.  That is what a login has
# always done: the shell starts somewhere fixed and the profile moves you.
V8START=${1:-$HOME}
export V8START

exec "$V8ROOT/usr/bin/v8" /
