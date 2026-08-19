#!/bin/sh
# runsuite -- run one test suite, keep its exit status, and record the number of
# cases it reported.
#
# WHY THIS EXISTS.  `make test' has never known how many cases it ran, so
# ARTICLE.md's "N tests across 17 suites" was a number a person updated when
# asked.  Measured, it went stale five times: 1767, then 2222 against 2483,
# then again within the hour, then 2691 against 2701.  CLAUDE.md's own
# asymmetry -- a number with a test beside it stays current and a number in
# prose does not -- with the article's command count as the control, because
# that one HAS a guard and has never drifted.
#
# WHY IT IS NOT A `tee' IN THE RECIPE.  A pipeline's status is its LAST
# element's, so `suite | tee out' reports tee, which always succeeds -- and a
# guard that costs the harness its exit status is worse than the stale number
# it fixes.  This tree has paid for that hazard twice already (the `make |
# grep; echo $?' note, and the icheck capture whose status was head's).  The
# status is carried out of the pipeline on FILE DESCRIPTOR 3 instead, which is
# POSIX and needs neither `pipefail' nor bash.  Output still streams live.
#
# AND THE STATUS IS CHECKED TWICE, because they are different claims.  A suite
# that dies halfway exits non-zero AND prints no summary; a suite that runs to
# the end and fails cases exits non-zero AND says `N failed'.  A suite that
# somehow printed a clean summary and exited non-zero fails here on the first
# test, and one that exited zero having printed nothing fails on the second.
usage() { echo "usage: runsuite.sh <name> <command> [args...]" >&2; exit 2; }
[ $# -ge 2 ] || usage
name=$1; shift

dir=${V8_TCDIR:-}
if [ -n "$dir" ]; then mkdir -p "$dir" || exit 2; fi
st=$(mktemp "${TMPDIR:-/tmp}/runsuite.XXXXXX") || exit 2
out=$(mktemp "${TMPDIR:-/tmp}/runsuite.XXXXXX") || { rm -f "$st"; exit 2; }
trap 'rm -f "$st" "$out"' 0 1 2 13 15

# AND FD 3 IS CLOSED FOR THE SUITE ITSELF, WHICH IS NOT A DETAIL HERE.  V8's
# /dev/tty IS /dev/fd/3 -- a hard link, opened by dup(2) -- so a status
# descriptor left open would hand every suite a terminal it did not have.
# Measured: three wavea cases that assert there is NO terminal went red the
# first time this wrapper ran (`stty says it cannot open the terminal', `p
# without /dev/tty declines', and the crash-probe floor, whose members include
# cpio's unchecked fopen of /dev/tty).  The instrument removing the condition
# it was pointed at, which is what CLAUDE.md records about lldb and cpio.
exec 3>"$st"
{ "$@" 2>&1 3>&-; echo $? >&3; } | tee "$out"
exec 3>&-
status=$(cat "$st")

[ "${status:-1}" -eq 0 ] || exit "${status:-1}"

# The summary line every suite prints as its last act.  Anchored on the name so
# a suite cannot be credited with another's count, and `tail -1' because a suite
# may quote the shape in its own diagnostics.
#
# THE THIRD FIELD IS WHAT MAKES THE RECORDED NUMBER THE PORT'S RATHER THAN THE
# HOST'S.  A suite whose coverage depends on the machine -- kmemu asks whether
# this host has an idle login, a mount point past 32 characters, a pid over
# 32767 -- prints "N not exercised" beside its pass count, and what is recorded
# here is pass + not-exercised: the number of cases the port HAS.  Without it
# the total is a host property, which is what took CI red at 2702 against this
# machine's 2705 with every suite reporting 0 failed.
#
# Both spellings are accepted rather than requiring the third field everywhere,
# because a suite with no conditional coverage has nothing to say and adding a
# ", 0 not exercised" to sixteen summary lines would be noise.  The two-field
# arm supplies the 0 itself, so nothing downstream has to know which is which.
sum=$(sed -n "s/^$name: \([0-9][0-9]*\) passed, \([0-9][0-9]*\) failed, \([0-9][0-9]*\) not exercised\$/\1 \2 \3/p" "$out" | tail -1)
[ -n "$sum" ] ||
sum=$(sed -n "s/^$name: \([0-9][0-9]*\) passed, \([0-9][0-9]*\) failed\$/\1 \2 0/p" "$out" | tail -1)
if [ -z "$sum" ]; then
	echo "runsuite: $name exited 0 but printed no summary line" >&2
	exit 1
fi
set -- $sum
if [ "$2" -ne 0 ]; then
	echo "runsuite: $name exited 0 but reported $2 failed" >&2
	exit 1
fi
[ -n "$dir" ] && printf '%s\n' "$(($1 + $3))" > "$dir/$name"
exit 0
