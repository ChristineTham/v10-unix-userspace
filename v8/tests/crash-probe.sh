#!/bin/sh
# tests/crash-probe.sh -- the EMPIRICAL half of the address-0 sweep (PLAN.md
# S4i, S4j).  Not a suite, and deliberately not part of `make test': it takes
# minutes, and what it measures is a number to drive work rather than a
# property to assert.
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
ROOT=$1; export V8ROOT=$ROOT
WORK=$2; mkdir -p "$WORK/run" && cd "$WORK/run" || exit 1
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
