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

# THE SKIP LIST IS A COVERAGE HOLE, AND IT HAS BEEN MEASURED RATHER THAN LEFT
# OPEN.  These are excluded because they create, move, remove or format things
# and a probe that damages the tree is worse than no probe -- which is why
# `fsck -t' could only ever have been found by the static audit (PLAN.md S4i).
# They CAN be probed safely, because the jail is per-binary: a V8 binary
# resolves every path inside $V8ROOT, so a throwaway copy of the rootfs per
# invocation contains anything it does, and `cp -ac' clones on APFS for ~0.3 s.
# Done once against the seventeen that exist here as Mach-O -- rm rmdir mv cp
# ln chmod mkdir mkfs clri fsck restor dump sh ed tee cc make -- for 901
# invocations: ZERO signal deaths.  A negative result, and the reason this
# stays a note rather than becoming a mode of this script.
SKIP='rm rmdir mv cp ln chmod chown chgrp mkdir mkfs clri fsck dd restor dump
      cron su kill sh csh ed qed adb login passwd init mount umount sync
      halt reboot shutdown tee cc as ld ar make nohup at write mail v8'

# run1 <prog> <label> [args...] -- run once and report only a REAL signal death.
#
# THE SHELL CANNOT TELL A SIGNAL FROM AN EXIT STATUS.  $? is 128+N when a child
# is killed by signal N, but a program is free to exit(134) of its own accord --
# and V8 programs whose main() falls off the end return whatever was in the
# register.  Measured: `primes' does exactly that, and 42 of its 53 garbage
# statuses landed in 129..159, which an earlier draft of this script reported
# as SIGABRT.  So the child is run under perl, which keeps the real wait status
# and can ask `$? & 127'.  The alarm is set in the child and survives exec.
run1() {
	p=$1; lbl=$2; shift 2
	rm -rf "$WORK/cell"; mkdir -p "$WORK/cell"
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

hits=0 tried=0 tainted=0
for p in "$ROOT"/bin/* "$ROOT"/usr/bin/* "$ROOT"/etc/* "$ROOT"/usr/lib/refer/* \
         "$ROOT"/usr/lib/spell/* "$ROOT"/lib/*; do
	[ -f "$p" ] && [ -x "$p" ] || continue
	b=$(basename "$p")
	case " $SKIP " in *" $b "*) continue;; esac
	file "$p" 2>/dev/null | grep -q 'Mach-O' || continue
	tried=$((tried+1)); run1 "$p" "$b (no arguments)"
	for o in a b c d e f g h i j k l m n o p q r s t u v w x y z \
	         A B C D E F G H I J K L M N O P Q R S T U V W X Y Z; do
		tried=$((tried+1)); run1 "$p" "$b -$o" "-$o"
	done
done
echo "---"
echo "$tried invocations, $hits died on a signal"
if [ "$tainted" -gt 0 ]; then
	echo "WARNING: $tainted more died on SIGKILL/SIGBUS -- the tree was"
	echo "         almost certainly rebuilt during the run.  Those are not"
	echo "         findings.  Re-run on a quiet tree."
fi
