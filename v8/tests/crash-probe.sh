#!/bin/sh
# tests/crash-probe.sh -- the EMPIRICAL half of the address-0 sweep (PLAN.md
# S4i, S4j).  Deliberately not part of `make test': it takes ~13 minutes, and
# the everyday command has to stay fast enough to run without thinking.
#
# THIS HEADER USED TO SAY "what it measures is a number to drive work rather
# than a property to assert", and that sentence is why the floor rotted.  A
# probe whose expected output lives in someone else's prose is checked when
# somebody remembers to check it: this one read 54 while the truth was 160,
# because four Wave A2 imports added 106 signal deaths and nothing went red.
# It asserts now -- against tests/crash-probe.floor, in both directions -- and
# it runs in CI as its own job.  `make test' still gets the fast half, a
# 2-second sweep of four programs in tests/wavea.
#
#   tests/crash-probe.sh $PWD/rootfs /tmp/probework
#
# Run every installed Mach-O binary bare and then with each of the 52
# single-letter options as its LAST argument -- which is the trigger for the
# whole class -- and report anything that dies on a signal.  4134 invocations,
# 254 signals when it was written; PLAN.md S4j has the triage, and says plainly
# that they are not all one class.
# Empirical counterpart to the address-0 audit.  Three things the first draft
# got wrong, each of which hid a real finding:
#   - it scanned only /bin and /usr/bin, so /etc (icheck, dcheck, fsck) and
#     /usr/lib (hunt) were never tried;
#   - it always passed an option, so the BARE invocation -- which is how
#     unexpand crashes -- was never tried;
#   - it had no per-invocation timeout, so one program that waits blocks the
#     whole run and the sweep silently never finishes.
#
# A FOURTH THING, AND IT IS A LIMITATION RATHER THAN A BUG.  Every invocation
# gets /dev/null on stdin, so for a program that REQUIRES input all 53 options
# also reach the empty-input path -- and the probe cannot tell an option bug
# from that.  lex is the worked example: its 53 are three separate faults, and
# fixing the first (warning()'s fflush of a null fout, which killed
# `lex -a spec.l' on a spec lex compiles fine) changed this script's count by
# ZERO, because the 40 then died a little further along at the second.  When a
# program crashes on EVERY option including bare, suspect one shared downstream
# path and re-run it by hand with real input before believing a single cause.
# RESOLVED BEFORE THE cd BELOW, and that ordering is the whole of a bug this
# script shipped for exactly one run.  $0 is usually relative
# (`tests/crash-probe.sh'), so computing this after cd'ing to the workdir makes
# `dirname $0' resolve against the wrong directory: the subshell failed, $HERE
# came out empty, and the floor path became `/crash-probe.floor'.  It was caught
# only because a missing floor file is a FAILURE rather than a skip -- the guard
# firing on the person who wrote it.
HERE=$(cd "$(dirname "$0")" && pwd) || exit 1
ROOT=$1; export V8ROOT=$ROOT
WORK=$2; mkdir -p "$WORK/run" || exit 1
# ABSOLUTE, because the next line cd's and run1 appends to $WORK/observed from
# inside $WORK/cell.  A relative workdir would have written the observations
# somewhere else and left the floor comparison reading an empty file, which is
# the direction that reports a pass.
WORK=$(cd "$WORK" && pwd) || exit 1
: > "$WORK/observed"
cd "$WORK/run" || exit 1
# Optional third argument: the per-invocation deadline in seconds (default 5).
# A program that has not crashed in a second almost certainly is not going to,
# and shortening it only produces more SIGALRMs, which are filtered -- so it
# trades run time for nothing.  The whole sweep is minutes either way.
DEADLINE=${3:-5}

# A FIFTH THING, AND IT IS THE FIRST ONE'S OTHER HALF.  The scan list used to
# be six literal globs, and two of them were wrong in the same direction as the
# original /etc omission: `$ROOT/usr/lib/spell/*' treats spell as a DIRECTORY by
# analogy with refer, and /usr/lib/spell is a Mach-O FILE -- V8's spellprog, the
# binary the /usr/bin/spell shell script calls.  So it matched nothing and spell
# was never probed.  /usr/lib/man was not named at all.  Both are clean (106
# invocations, zero signals), so this cost nothing THIS time; it is fixed
# because a hole that happens to be empty still hides the next thing to fall in
# it.  Note the shape -- the fix that added /etc and /usr/lib/refer stopped one
# directory short, which is CLAUDE.md's rule that the fix lands on one line and
# the line beside it keeps the assumption.
#
# So the population is now DERIVED: find over the installed directories,
# filtered to Mach-O.  A newly installed program is covered the day it lands,
# and the script PRINTS what it found -- because the count below drifted from
# seventeen to eighteen (the `v8' launcher) with nothing to say so.

# THE TWO LISTS ARE A CLASSIFICATION, NOT A SKIP LIST, and the split is the
# whole point.  What was one blob is now two, because the reasons are opposite
# and only one of them is a permanent exclusion.
#
# UNSAFE escapes the jail, so no amount of throwaway rootfs contains it: halt,
# reboot, shutdown, init, sync, mount and umount act on the HOST; kill signals
# host pids; adb wants ptrace on them; su, login, passwd, cron, at, mail and
# write are interactive or touch system state; as, ld and ar are the host's by
# PLAN.md S1, so probing them probes Apple's tools rather than this port's.
#
# MUTATES only changes things INSIDE the jail, which the jail can therefore
# contain: a V8 binary resolves every path inside $V8ROOT, so a throwaway copy
# of the rootfs per invocation bounds anything it does.  Measured here: `cp -ac'
# clones a 15 MB rootfs in 0.146 s.  These are probed under PROBE=mutating,
# each invocation in its own clone -- because a prober must be a pure function
# of the program and its arguments, which is the hermeticity lesson above.
UNSAFE='halt reboot shutdown init sync mount umount kill adb
        cron su login passwd at write mail as ld ar'
MUTATES='rm rmdir mv cp ln chmod chown chgrp mkdir mkfs clri fsck dd restor
         dump sh csh ed qed tee cc make nohup v8'

# safe (default) -- today's population, and the numbers stay comparable.
# mutating      -- only the MUTATES set, each invocation in a fresh rootfs.
# all           -- both.
PROBE=${PROBE:-safe}

# run1 <prog> <label> [args...] -- run once and report only a REAL signal death.
#
# THE SHELL CANNOT TELL A SIGNAL FROM AN EXIT STATUS.  $? is 128+N when a child
# is killed by signal N, but a program is free to exit(134) of its own accord --
# and V8 programs whose main() falls off the end return whatever was in the
# register.  Measured: `primes' does exactly that, and 42 of its 53 garbage
# statuses landed in 129..159, which an earlier draft of this script reported
# as SIGABRT.  So the child is run under perl, which keeps the real wait status
# and can ask `$? & 127'.  The alarm is set in the child and survives exec.
#
# When $clone is set the invocation also gets its own copy of the whole rootfs
# and runs the binary OUT OF that copy, with V8ROOT pointing at it -- so a
# program that removes /bin removes the clone's.  Both halves are needed: a
# fresh cwd alone would not stop `rm -r /' from emptying the real tree.
run1() {
	p=$1; lbl=$2; shift 2
	rm -rf "$WORK/cell"; mkdir -p "$WORK/cell"
	if [ -n "$clone" ]; then
		rm -rf "$WORK/root"
		cp -ac "$ROOT" "$WORK/root" 2>/dev/null ||
			cp -a "$ROOT" "$WORK/root" || return
		V8ROOT=$WORK/root; export V8ROOT
		p=$WORK/root${p#$ROOT}
	else
		V8ROOT=$ROOT; export V8ROOT
	fi
	res=$(cd "$WORK/cell" && perl -e '
		my $deadline = shift;
		my $pid = fork();
		if (!defined $pid) { print "EXIT 0\n"; exit }
		if ($pid == 0) {
			alarm $deadline;
			open(STDIN,  "<", "/dev/null");
			open(STDOUT, ">", "/dev/null");
			open(STDERR, ">", "/dev/null");
			exec @ARGV;
			exit(127);
		}
		waitpid($pid, 0);
		my $st = $?;
		if ($st & 127) { printf "SIG %d\n", $st & 127 }
		else           { printf "EXIT %d\n", $st >> 8 }
	' "$DEADLINE" "$p" "$@")
	case "$res" in
	"SIG "*)
		sig=${res#SIG }
		[ "$sig" = 14 ] && return		# our own alarm
		[ "$sig" = 13 ] && return		# SIGPIPE
		# SIGKILL is never a program bug: something outside killed it,
		# and a rebuild during the run will do that.
		if [ "$sig" = 9 ]; then
			echo "TAINTED sig 9  $lbl  (killed from outside -- rebuild?)"
			tainted=$((tainted+1))
			return
		fi
		echo "SIGNAL $sig  $lbl"
		echo "$sig $lbl" >> "$WORK/observed"
		hits=$((hits+1))
		;;
	esac
}

# A name in both lists would be silently probed or silently not, depending on
# which case ran first.  Say so instead.
for n in $UNSAFE; do
	case " $(echo $MUTATES) " in
	*" $n "*) echo "BUG: $n is in both UNSAFE and MUTATES"; exit 1;;
	esac
done

hits=0 tried=0 tainted=0 nsafe=0 nmut=0
for p in $(find "$ROOT/bin" "$ROOT/usr/bin" "$ROOT/etc" "$ROOT/lib" \
                "$ROOT/usr/lib" -type f -perm -100 2>/dev/null | sort); do
	file "$p" 2>/dev/null | grep -q 'Mach-O' || continue
	b=$(basename "$p")
	case " $(echo $UNSAFE) " in *" $b "*) continue;; esac
	clone=
	case " $(echo $MUTATES) " in
	*" $b "*)
		case $PROBE in mutating|all) clone=1;; *) continue;; esac
		nmut=$((nmut+1));;
	*)
		case $PROBE in mutating) continue;; esac
		nsafe=$((nsafe+1));;
	esac
	tried=$((tried+1)); run1 "$p" "$b (no arguments)"
	for o in a b c d e f g h i j k l m n o p q r s t u v w x y z \
	         A B C D E F G H I J K L M N O P Q R S T U V W X Y Z; do
		tried=$((tried+1)); run1 "$p" "$b -$o" "-$o"
	done
done
rm -rf "$WORK/root"

# NAMED BUT ABSENT is the drift the transcribed count could not show.  A name
# here is defensive -- the program is not installed -- and printing it is what
# turns "seventeen" from something someone wrote down into something measured.
absent=
for n in $UNSAFE $MUTATES; do
	find "$ROOT/bin" "$ROOT/usr/bin" "$ROOT/etc" "$ROOT/lib" "$ROOT/usr/lib" \
	     -type f -name "$n" 2>/dev/null | grep -q . || absent="$absent $n"
done

echo "---"
echo "population: $nsafe safe, $nmut mutating (PROBE=$PROBE)"
[ -n "$absent" ] && echo "named but not installed:$absent"
echo "$tried invocations, $hits died on a signal"
if [ "$tainted" -gt 0 ]; then
	echo "WARNING: $tainted more died on SIGKILL/SIGBUS -- the tree was"
	echo "         almost certainly rebuilt during the run.  Those are not"
	echo "         findings.  Re-run on a quiet tree."
fi

# ---------------------------------------------------------------------------
# THE FLOOR, AND WHY THIS SCRIPT NOW HAS AN EXIT STATUS.
#
# For most of its life this printed a number and exited 0.  That is what let
# the floor rot: the expected output lived in a sentence in CLAUDE.md, a human
# had to run the script and compare by eye, and when four Wave A2 programs
# added 106 signal deaths NOTHING WENT RED for weeks.  CLAUDE.md's own entry on
# the incident ends "a floor belongs in a suite or in CI, not in prose".  This
# is that, and the script is in CI now.
#
# THE EXPECTATION IS A LIST, NOT A COUNT, and that is the whole design.  A
# count lets two changes cancel: fix one crash, introduce another, and the
# total is unchanged.  crash-probe.floor names every invocation that is
# expected to die, so a swap fails on both halves.
#
# IT IS CHECKED IN BOTH DIRECTIONS.  A new crash is a regression, obviously.
# A crash that has GONE is also a failure, because a floor that over-states is
# a place for the next regression to hide -- exactly how tests/kmemu's allowed
# list is kept honest.  Fixing something therefore requires updating the floor
# in the same commit, which is the point: it makes the removal deliberate and
# reviewable rather than silent.
#
# A MISSING FLOOR FILE IS A FAILURE, NOT A SKIP.  tests/cpp once wrapped its
# most valuable case in `if [ -d "$V8INC" ]` and reported "12 passed" from
# outside the repo root.  Anchor to $0 and refuse to run without it.
# ---------------------------------------------------------------------------
FLOOR=$HERE/crash-probe.floor

# LC_ALL=C ON EVERY sort AND THE comm, because comm requires both inputs in the
# SAME collating order and a locale's order is a property of the host.  Getting
# this wrong would not fail cleanly -- comm would pair the wrong lines and
# report entries as simultaneously new and gone, on some machines and not
# others.  That is the host-property trap in a guard, which this suite has been
# bitten by twice; the fix is to make the order ours.  The committed order of
# the floor file is irrelevant for the same reason: both sides are re-sorted
# here, by this sort.
LC_ALL=C; export LC_ALL
sort -o "$WORK/observed" "$WORK/observed" 2>/dev/null || : > "$WORK/observed"

case $PROBE in
mutating)
	# The MUTATES set is expected to be entirely clean: re-measured at 21
	# programs, 1113 invocations, zero signal deaths, with the real rootfs
	# byte-identical afterwards.  No floor file, and an empty expectation is
	# the strongest one there is -- which is also why this arm needs no
	# maintenance as the population grows.  (The recorded figure was 18 and
	# 954; three more MUTATES programs have been installed since, so the
	# COUNT was stale while the expectation was not.  That is the argument
	# for expressing an expectation as a property rather than a number
	# wherever you can.)
	: > "$WORK/expected";;
*)
	if [ ! -f "$FLOOR" ]; then
		echo "::error::no floor file at $FLOOR -- refusing to report a"
		echo "          pass, because a probe with no expectation is"
		echo "          the thing this file exists to stop."
		exit 1
	fi
	grep -v '^[[:blank:]]*#' "$FLOOR" | grep -v '^[[:blank:]]*$' |
		sort > "$WORK/expected";;
esac

new=$(comm -13 "$WORK/expected" "$WORK/observed")
gone=$(comm -23 "$WORK/expected" "$WORK/observed")
rc=0
if [ -n "$new" ]; then
	echo "::error::NEW signal deaths not in the floor:"
	printf '%s\n' "$new" | sed 's/^/    + /' | head -60
	rc=1
fi
if [ -n "$gone" ]; then
	echo "::error::floor entries that NO LONGER crash -- if you fixed these,"
	echo "          remove them from tests/crash-probe.floor in the same commit:"
	printf '%s\n' "$gone" | sed 's/^/    - /' | head -60
	rc=1
fi
# A tainted run cannot testify either way, so it must not read as a pass.
[ "$tainted" -gt 0 ] && rc=1
[ $rc -eq 0 ] && echo "floor: exactly as declared ($(wc -l < "$WORK/expected" | tr -d ' ') entries)"
exit $rc
