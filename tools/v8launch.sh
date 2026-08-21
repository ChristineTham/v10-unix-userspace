#!/bin/sh
# v8 -- enter the Research Unix V8 world.  Installed by `make install'.
#
# THE GOLDEN IMAGE AND THE WORKING COPY, which is the whole shape of this
# script and the reason it stopped being four lines.
#
# `make install' writes a PRISTINE tree and never touches it again.  The first
# time a person runs this, that tree is copied to a working copy under their
# own home, the user is created in it, and every run after that uses the copy.
# So the world is a place to keep things: a file written in /usr/<name>, a
# program built and installed into /bin, an edited /etc/motd all survive the
# next launch, the next `make install', and a `make clean' in the build tree.
#
# WHY IT CANNOT BE THE INSTALLED TREE ITSELF.  Three reasons, and the first is
# fatal on its own.  $PREFIX defaults under /usr/local, which is root-owned on
# macOS, so the world would be read-only to the person using it -- and this
# port's central claim is that V8 rebuilds V8, which means Bell Labs' own
# Admin/Mk has to be able to `cp prog /bin'.  A read-only world cannot do the
# one thing the port exists to demonstrate.  Second, `make install' rebuilds
# from clean and copies over the top, so anything a user had put there would be
# destroyed by an upgrade.  Third, one root-owned tree is shared by every
# account on the Mac, and a world you cannot write is a demo rather than a
# system.
#
# RESET IS USER-INITIATED AND CONFIRMED.  `v8 --reset' is the only thing that
# destroys a working copy, it says exactly what will be lost, and it requires
# the word RESET rather than a bare y -- because the working copy is the only
# place a person's work lives and nothing else backs it up.

V8GOLDEN=@GOLDEN@
V8WORK=${V8WORK:-$HOME/.v8}

usage() {
	cat <<EOF
usage: v8 [--pure] [--golden] [--reset] [--help] [directory]

  (no options)  enter your world at $V8WORK, as yourself, in \$HOME
  [directory]   land there instead of \$HOME -- v8(1) takes the landing
                directory as its argument and always execs /bin/sh, so
                this is a place to start, not a command to run
  --pure        refuse every host binary but the documented toolchain
                exception (V8JAIL=strict).  A pure V8 world: nothing that
                is not V8 can be run, including python and git
  --golden      enter the pristine installed image read-only, changing
                nothing.  For seeing what a fresh world looks like
  --reset       DELETE the working copy and recreate it from the golden
                image on the next launch.  Confirms first
EOF
}

# ---------------------------------------------------------------- options
pure=0 golden=0 reset=0
while [ $# -gt 0 ]; do
	case "$1" in
	--pure)    pure=1;   shift ;;
	--golden)  golden=1; shift ;;
	--reset)   reset=1;  shift ;;
	-h|--help) usage; exit 0 ;;
	--)        shift; break ;;
	-*)        echo "v8: unknown option $1" >&2; usage >&2; exit 2 ;;
	*)         break ;;
	esac
done

if [ ! -d "$V8GOLDEN" ]; then
	echo "v8: no golden image at $V8GOLDEN -- run 'make install' first" >&2
	exit 1
fi

# ------------------------------------------------------------------ reset
if [ "$reset" = 1 ]; then
	if [ ! -d "$V8WORK" ]; then
		echo "v8: no working copy at $V8WORK -- nothing to reset."
	else
		echo "v8: this DELETES your world at $V8WORK."
		echo "    Every file you created inside it is lost, including"
		echo "    anything under /usr/$(id -un) and anything you installed."
		echo "    Nothing else has a copy."
		printf "    Type RESET to confirm: "
		read ans
		if [ "$ans" != RESET ]; then
			echo "v8: not reset."
			exit 1
		fi
		rm -rf "$V8WORK" || exit 1
		echo "v8: world removed.  It is rebuilt from the golden image now."
	fi
fi

# ----------------------------------------------------- create the world
#
# ONE user, and it is whoever is running this.  /etc/passwd is synthesized
# rather than copied for the reason the Makefile's own rule gives: V8's file
# names Bell Labs people with 1985 uids, so getpwuid() would fail to find the
# person actually here and `ls -l' would print numbers.  It is done HERE and
# not at build time because the golden image may be installed by one account
# and run by another -- `sudo make install' is exactly that case.
makeuser() {
	_w=$1 _u=$(id -un) _uid=$(id -u) _gid=$(id -g)
	_gecos=$(id -F 2>/dev/null) || _gecos=$_u
	[ -n "$_gecos" ] || _gecos=$_u

	mkdir -p "$_w/etc" "$_w/usr/$_u"
	{ echo "root:*:0:0:Superuser:/:/bin/sh"
	  echo "$_u:*:$_uid:$_gid:$_gecos:/usr/$_u:/bin/sh"
	} > "$_w/etc/passwd"

	# A .profile, because the Bourne shell reads one and a world that greets
	# you by name is the difference between a system and a demo.  Written
	# only if absent, so a reset keeps nothing and an upgrade destroys
	# nothing.
	if [ ! -f "$_w/usr/$_u/.profile" ]; then
		cat > "$_w/usr/$_u/.profile" <<EOF
PATH=/bin:/usr/bin:/etc
export PATH
echo "Research Unix, Eighth Edition."
echo "You are \$LOGNAME.  Your files are in \$HOME and they persist."
EOF
	fi
}

if [ "$golden" = 1 ]; then
	V8ROOT=$V8GOLDEN
elif [ ! -d "$V8WORK" ]; then
	echo "v8: first run -- building your world at $V8WORK"
	mkdir -p "$V8WORK" || exit 1
	cp -R "$V8GOLDEN"/. "$V8WORK"/ || exit 1
	# The golden may be root-owned and mode-restricted; the copy is ours and
	# has to be writable, or the world is read-only for the second time.
	chmod -R u+w "$V8WORK" 2>/dev/null
	makeuser "$V8WORK"
	echo "v8: done.  'v8 --reset' returns it to the golden image."
	V8ROOT=$V8WORK
else
	# An upgrade can add a user to an existing world without touching
	# anything else: makeuser only ever rewrites /etc/passwd, and leaves a
	# .profile that is already there alone.
	[ -d "$V8WORK/usr/$(id -un)" ] || makeuser "$V8WORK"
	V8ROOT=$V8WORK
fi
export V8ROOT

# ------------------------------------------------------------ the login
#
# V8ROOT overrides the root compiled into every binary, so a moved or copied
# world still works.  V8's own make passes it down to everything it runs.
V8USER=$(id -un)
HOME=/usr/$V8USER
USER=$V8USER
LOGNAME=$V8USER
export HOME USER LOGNAME

# THE HOST PATH IS SAVED BEFORE IT IS SHADOWED, and that is what makes
# `macos CMD' possible: 62 of this world's commands share a name with one of
# the Mac's, so a native build started from in here would otherwise find V8's
# make and V8's cc.  macos(1) restores this and execs.
V8HOSTPATH=$PATH
export V8HOSTPATH

# /etc is on PATH because V8's own sh has it there -- chgrp, chown and the
# section-8 tools live in /etc, which Admin/etcfiles decides and this port
# follows.
PATH=$V8ROOT/bin:$V8ROOT/usr/bin:$V8ROOT/etc:$PATH
export PATH

[ "$pure" = 1 ] && { V8JAIL=strict; export V8JAIL; }

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

# v8(1) TAKES THE LANDING DIRECTORY AS ITS ARGUMENT, which is upstream's own
# interface -- `chdir(argc>1? argv[1]: "/")' -- so the login lands in the home
# directory without one line of change to V8's source.
#
# AND THAT IS WHY IT IS DONE HERE.  A first draft taught v8.c to read $HOME
# itself, and tests/jail caught it within the run: a bare v8 invoked WITHOUT
# this launcher inherits the Mac's HOME, so `chdir("/Users/christie")' walked
# straight out of the world -- /Users is not a mount-table prefix, so the shim
# leaves it alone.  The environment is the launcher's to set and the argument
# is the program's to take; crossing them made a host variable into a jail
# escape.
#
# AND $1 IS A DIRECTORY, NOT A COMMAND.  The usage line said `[command ...]'
# for as long as this script has existed, and nothing ever implemented it:
# v8.c ends `execl("/bin/sh", "-", 0)' unconditionally, so there is nowhere for
# a command to go.  What that cost is a bad diagnostic rather than a bad
# feature -- `v8 /bin/echo hi' chdir'd to /bin/echo and printed
# `v8: can't chdir', which reads as a broken install rather than as a wrong
# invocation.  Found by installing to a scratch prefix and running the result,
# which is the only thing that exercises this line at all.
exec "$V8ROOT/usr/bin/v8" "${1:-$HOME}"
