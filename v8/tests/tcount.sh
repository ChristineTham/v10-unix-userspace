#!/bin/sh
# tcount -- the guard ARTICLE.md's test count never had.
#
# Run from `make test's own recipe, which make reaches only when every suite
# has succeeded, so there is no circularity: the objection recorded against
# doing this from INSIDE a suite -- "the test total cannot be had without
# running every suite" -- is true of a suite and false of the target that
# depends on all of them.
#
# EVERY INPUT IS GUARDED, because this file exists to stop a number going stale
# and a check that quietly measures nothing is how the last one did.  A missing
# directory, a count that is not a number, a suite that did not report, and an
# ARTICLE.md that states no total are all FAILURES rather than skips -- the
# rule tests/cpp's `if [ -d "$V8INC" ]' skip taught, which reported 12 passed
# over a case that had not run.
usage() { echo "usage: tcount.sh <countdir> <article> <nsuites>" >&2; exit 2; }
[ $# -eq 3 ] || usage
dir=$1 article=$2 want=$3

[ -d "$dir" ]      || { echo "tcount: no count directory $dir"    >&2; exit 1; }
[ -f "$article" ]  || { echo "tcount: no article at $article"     >&2; exit 1; }
case $want in ''|*[!0-9]*) echo "tcount: suite count [$want] is not a number" >&2; exit 1;; esac

n=0 total=0
for f in "$dir"/*; do
	[ -f "$f" ] || continue
	c=$(cat "$f")
	case $c in ''|*[!0-9]*)
		echo "tcount: $f holds [$c], which is not a count" >&2; exit 1;;
	esac
	total=$((total + c)) n=$((n + 1))
done
if [ "$n" -ne "$want" ]; then
	echo "tcount: $n suites reported a count, want $want" >&2
	exit 1
fi

# The sentence carries TWO numbers and both are claims, which is the lesson the
# command-count guard learned by capturing one of a pair four words apart.
stated=$(sed -n 's/.*\*\*\([0-9][0-9]*\) tests across \([0-9][0-9]*\) suites\*\*.*/\1 \2/p' \
         "$article" | head -1)
[ -n "$stated" ] || { echo "tcount: $article states no test count" >&2; exit 1; }
set -- $stated
if [ "$1" -ne "$total" ] || [ "$2" -ne "$want" ]; then
	echo "tcount: $article says $1 tests across $2 suites;" \
	     "the run was $total across $want" >&2
	exit 1
fi
echo "tests: $total cases across $n suites, and ARTICLE.md agrees"
