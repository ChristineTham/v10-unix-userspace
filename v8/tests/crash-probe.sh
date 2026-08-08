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
ROOT=$1; export V8ROOT=$ROOT
WORK=$2; mkdir -p "$WORK/run" && cd "$WORK/run" || exit 1

SKIP='rm rmdir mv cp ln chmod chown chgrp mkdir mkfs clri fsck dd restor dump
      cron su kill sh csh ed qed adb login passwd init mount umount sync
      halt reboot shutdown tee cc as ld ar make nohup at write mail v8'

run1() {	# run1 <prog> <label> [args...]; 5s deadline, no side effects
	p=$1; lbl=$2; shift 2
	perl -e 'alarm 5; exec @ARGV' "$p" "$@" </dev/null >/dev/null 2>&1
	rc=$?
	if [ $rc -ge 129 ] && [ $rc -le 159 ]; then
		sig=$((rc-128))
		[ $sig -eq 14 ] && return		# our own alarm
		[ $sig -eq 13 ] && return		# SIGPIPE
		echo "SIGNAL $sig  $lbl"
		hits=$((hits+1))
	fi
}

hits=0 tried=0
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
