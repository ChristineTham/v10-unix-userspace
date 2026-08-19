#!/bin/sh
# Wave A: real V8 commands, compiled by v8cc, linked freestanding, run.
#
# These are authentic V8 sources from src/cmd, not test fixtures. Every compiler
# bug found so far has lived in combinations of features that real code uses and
# synthetic tests do not, so this suite leads with real programs by design.

ROOT=$(cd "$(dirname "$0")/../.." && pwd)	# the release tree, v8/
# AND THE SCRIPT ITSELF, ABSOLUTE, because two cases READ THIS FILE and the
# suite cd's to $TMP below -- so `"$0"' is a relative path to nowhere the
# moment the suite is invoked as `v8/tests/wavea/run.sh' from the repo root,
# which CLAUDE.md documents as a supported way to run one.  Measured: the
# f77refuse coverage case then reports `no alternation extracted'.  It fails
# LOUDLY, which is luck -- the other reader has a vacuity guard and would have
# derived a count of zero.  tests/cpp's anchor-to-dirname lesson, in a case.
SELF=$ROOT/tests/wavea/run.sh
REPO=$(cd "$ROOT/.." && pwd)			# the repository above it
CC=$ROOT/rootfs/bin/cc
V8ROOT=$ROOT/rootfs
export V8ROOT
TMP=${TMPDIR:-/tmp}/wavea.$$
mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT

LIBC=$ROOT/build/stage0/libc/libv8c.a
CRT=$ROOT/build/stage0/crt0.o
STUBS=$ROOT/build/stage0/v8sys/libv8stubs.a
# Link the ARCHIVES, never a glob of loose .o files: a stale object left
# behind by a rename once shadowed libv8stubs.a entirely, and the suites
# passed while testing the old code.
SHIM=$ROOT/build/stage0/v8sys/libv8sys.a

pass=0 fail=0

# build <name> <source>
#
# THE INSTALLED BINARY IS WHAT GETS TESTED, when there is one.
#
# This suite used to compile every case into $TMP and delete it on exit, so
# "Wave A works" was backed by artifacts nobody could run and by a code path
# nobody shipped.  CLAUDE.md's rule is the other way round -- a program is not
# testable until it is installed -- and the rest of the tree follows it.  Every
# single-file command in src/cmd is now a real $(ROOTFS)/bin binary, so the
# cases below run those.
#
# The fallback compile stays for exactly one caller: rt.c, a synthetic stdio
# round-trip written by this script, which has no installed counterpart.  If a
# real command ever falls through to it that is a missing install rule, and the
# `every imported command is installed' case at the foot of this file says so.
# v8which NAME -- where the rootfs installed a command.  /bin and /usr/bin are
# both real answers and which one is upstream's decision, not ours: V8's /bin is
# a 56-entry root-filesystem set and wc, tr, sort, sed and most of Wave A live in
# /usr/bin.  See the Makefile's v8dest, and the case at the foot of this file
# that asserts each one landed where the shipped tree has it.
v8which() {
	for d in bin usr/bin etc; do
		[ -x "$V8ROOT/$d/$1" ] && { echo "$V8ROOT/$d/$1"; return 0; }
	done
	return 1
}

build() {
	if src=$(v8which "$1"); then
		cp "$src" "$TMP/$1" || return 1
		return 0
	fi
	if [ $# -lt 2 ]; then
		echo "FAIL $1 (not installed, and no source to fall back to)"
		return 1
	fi
	if ! "$CC" -c -o "$TMP/$1.o" "$2" > "$TMP/$1.log" 2>&1; then
		echo "FAIL $1 (compile)"; head -3 "$TMP/$1.log"; return 1
	fi
	if ! clang -nostdlib -e _v8start -o "$TMP/$1" "$CRT" "$TMP/$1.o" \
	    "$LIBC" "$STUBS" "$SHIM" -lSystem >> "$TMP/$1.log" 2>&1; then
		echo "FAIL $1 (link)"; head -3 "$TMP/$1.log"; return 1
	fi
	return 0
}

check() { # check <name> <expected> <actual>
	if [ "$2" = "$3" ]; then pass=$((pass+1))
	else fail=$((fail+1)); echo "FAIL $1"; echo "  want [$2]"; echo "  got  [$3]"; fi
}

cd "$TMP" || exit 1
printf 'alpha\nbeta\ngamma\n' > in.txt

# ---- cat ----------------------------------------------------------------
if build cat "$ROOT/src/cmd/cat.c"; then
	check 'cat one file'  'alpha beta gamma' "$(./cat in.txt | tr '\n' ' ' | sed 's/ $//')"
	check 'cat two files' '6' "$(./cat in.txt in.txt | wc -l | tr -d ' ')"
	check 'cat stdin pipe' 'piped' "$(printf 'piped\n' | ./cat)"
	check 'cat stdin redirect' 'alpha beta gamma' \
	    "$(./cat < in.txt | tr '\n' ' ' | sed 's/ $//')"
else
	fail=$((fail+4))
fi

# ---- echo ---------------------------------------------------------------
if build echo "$ROOT/src/cmd/echo.c"; then
	check 'echo words' 'hello world' "$(./echo hello world)"
	check 'echo -n'    'nonl'        "$(./echo -n nonl)"
else
	fail=$((fail+2))
fi

# ---- wc -----------------------------------------------------------------
if build wc "$ROOT/src/cmd/wc.c"; then
	check 'wc counts' '3 3 17' "$(./wc < in.txt | tr -s ' ' | sed 's/^ //')"
else
	fail=$((fail+1))
fi

# ---- basename -----------------------------------------------------------
if build basename "$ROOT/src/cmd/basename.c"; then
	check 'basename strips dir'    'cat.c' "$(./basename /usr/src/cmd/cat.c)"
	check 'basename strips suffix' 'cat'   "$(./basename /usr/src/cmd/cat.c .c)"
else
	fail=$((fail+2))
fi

# ---- tee ----------------------------------------------------------------
if build tee "$ROOT/src/cmd/tee.c"; then
	check 'tee to file and stdout' 'hi hi' \
	    "$(printf 'hi\n' | ./tee t.out | cat - t.out | tr '\n' ' ' | sed 's/ $//')"
else
	fail=$((fail+1))
fi

# ---- yes ----------------------------------------------------------------
if build yes "$ROOT/src/cmd/yes.c"; then
	check 'yes repeats' 'y y y' "$(./yes 2>/dev/null | head -3 | tr '\n' ' ' | sed 's/ $//')"
else
	fail=$((fail+1))
fi

# ---- rev ----------------------------------------------------------------
if build rev "$ROOT/src/cmd/rev.c"; then
	check 'rev reverses' 'cba' "$(printf 'abc\n' | ./rev)"
else
	fail=$((fail+1))
fi

# ---- tr -----------------------------------------------------------------
if build tr "$ROOT/src/cmd/tr.c"; then
	check 'tr range'    'ABC' "$(printf 'abc\n' | ./tr a-z A-Z)"
	check 'tr explicit' 'xyz' "$(printf 'abc\n' | ./tr abc xyz)"
	# -d took a SIGBUS until string literals moved to writable data: with one
	# argument the other string is the literal "", and nextc() ends with
	# `if(c==0) *--s->p = 0;` -- a write straight into it.
	check 'tr -d'       'acac' "$(printf 'abcabc\n' | ./tr -d b)"
	check 'tr -cd'      'abcabc' "$(printf 'abc-abc\n' | ./tr -cd abc)"
	# -s squeezes characters that are in STRING2, per V8's tr(1) -- so with
	# no string2 nothing is squeezed, which is not the BSD reading.
	check 'tr -s two strings' 'abc' "$(printf 'aabbcc\n' | ./tr -s a-c a-c)"
	check 'tr -s one string'  'aabbcc' "$(printf 'aabbcc\n' | ./tr -s abc)"
else
	fail=$((fail+6))
fi

# ---- cmp ----------------------------------------------------------------
# cmp needs the ctype table, and it was the first program to want one.
printf 'alpha\nbeta\n' > c1.txt; cp c1.txt c2.txt; printf 'alpha\nbetX\n' > c3.txt
if build cmp "$ROOT/src/cmd/cmp.c"; then
	./cmp c1.txt c2.txt > cmp.out 2>&1
	check 'cmp same (silent)'  ''   "$(cat cmp.out)"
	check 'cmp same (status)'  '0'  "$?"
	check 'cmp differ' 'c1.txt c3.txt differ: char 10, line 2' \
	    "$(./cmp c1.txt c3.txt 2>&1)"
	./cmp -s c1.txt c3.txt 2>/dev/null
	check 'cmp -s status' '1' "$?"
	check 'cmp -l' '10 141 130' "$(./cmp -l c1.txt c3.txt | tr -s ' ' | sed 's/^ //')"
	./cmp c1.txt nosuchfile > /dev/null 2>&1
	check 'cmp missing file status' '2' "$?"
else
	fail=$((fail+6))
fi

# ---- sum ----------------------------------------------------------------
if build sum "$ROOT/src/cmd/sum.c"; then
	check 'sum is stable' "$(./sum < in.txt)" "$(./sum < in.txt)"
else
	fail=$((fail+1))
fi

# ---- getchar/putchar round trip through stdio ---------------------------
cat > rt.c <<'EOF'
#include <stdio.h>
main() { int c; while ((c = getchar()) != EOF) putchar(c); return 0; }
EOF
if build rt rt.c; then
	check 'stdio round trip' 'alpha beta gamma' \
	    "$(./rt < in.txt | tr '\n' ' ' | sed 's/ $//')"
else
	fail=$((fail+1))
fi

# ---- head ---------------------------------------------------------------
if build head "$ROOT/src/cmd/head.c"; then
	check 'head -2'      'alpha beta' "$(./head -2 in.txt | tr '\n' ' ' | sed 's/ $//')"
	check 'head default' 'alpha beta gamma' \
	    "$(./head in.txt | tr '\n' ' ' | sed 's/ $//')"
	check 'head stdin'   'alpha' "$(./head -1 < in.txt)"
else
	fail=$((fail+3))
fi

# ---------------------------------------------------------------------------
# The rest of Wave A.
#
# `try` builds a command and runs one case; a build failure counts as a failure
# for that case rather than silently skipping, so a regression in the compiler
# shows up here as a number rather than as a shorter run.
# ---------------------------------------------------------------------------
try() {	# try <cmd> <label> <expected> <shell pipeline>
	cmd=$1; label=$2; want=$3; shift 3
	if [ ! -x "./$cmd" ] && ! build "$cmd" "$ROOT/src/cmd/$cmd.c"; then
		fail=$((fail+1)); return
	fi
	check "$label" "$want" "$(eval "$@" 2>&1)"
}

printf 'banana\napple\ncherry\napple\n' > s.txt
printf 'one two three\nfour five six\n' > w.txt

try sort   'sort'            'apple apple banana cherry' "./sort s.txt | tr '\n' ' ' | sed 's/ \$//'"
try sort   'sort -r'         'cherry banana apple apple' "./sort -r s.txt | tr '\n' ' ' | sed 's/ \$//'"
try sort   'sort -u'         'apple banana cherry'       "./sort -u s.txt | tr '\n' ' ' | sed 's/ \$//'"
try uniq   'uniq -c'         '2' "./sort s.txt | ./uniq -c | awk '\$2==\"apple\"{print \$1}'"
try tail   'tail -2'         'cherry apple' "./tail -2 s.txt | tr '\n' ' ' | sed 's/ \$//'"
try cut    'cut -f2'         'two five' "./cut -f2 -d' ' w.txt | tr '\n' ' ' | sed 's/ \$//'"
try paste  'paste two files' 'one two three	banana' "./paste w.txt s.txt | head -1"
try fold   'fold -5'         'one t wo th ree' "./fold -5 w.txt | head -3 | tr '\n' ' ' | sed 's/ \$//'"
try expand 'expand tabs'     'a       b' "printf 'a\tb\n' | ./expand"
# unexpand without -a converts only LEADING blanks, per V8's unexpand(1);
# it also needs a file argument, since with none argv[0] is NULL and the
# very first thing it does is read argv[0][0].
try unexpand 'unexpand leading' 'X	y' "printf '        y\n' > ue.txt; ./unexpand ue.txt | sed 's/^/X/'"
try od     'od -c'           '0000000   a   b   c  \n' "printf 'abc\n' | ./od -c | head -1 | sed 's/  *\$//'"
try comm   'comm -12'        'apple' "printf 'apple\nbeta\n' > c_a; printf 'apple\ngamma\n' > c_b; ./comm -12 c_a c_b"
try join   'join'            'k v1 v2' "printf 'k v1\n' > j_a; printf 'k v2\n' > j_b; ./join j_a j_b"
try look   'look'            'apple' "./sort s.txt > s_sorted; ./look app s_sorted | head -1"
try grep   'grep'            'banana' "./grep ana s.txt"
try grep   'grep -c'         '2' "./grep -c apple s.txt"
try grep   'grep -v'         '2' "./grep -v apple s.txt | wc -l | tr -d ' '"
try fgrep  'fgrep'           'cherry' "./fgrep cherry s.txt"
# number(1) reads numbers until EOF and prints "..." as its continuation
# prompt, so only the first line is the answer.
try number 'number'          'forty two.' "echo 42 | ./number | head -1 | sed 's/^ *//'"
try printenv 'printenv'      'v8test' "V8TESTVAR=v8test ./printenv V8TESTVAR"
try seq    'seq'             '1 2 3' "./seq 3 | tr '\n' ' ' | sed 's/ \$//'"
try cal    'cal 2 1985'      'February 1985' "./cal 2 1985 | head -1 | sed 's/^ *//;s/ *\$//'"
# V8's split names its pieces with -f, not a positional prefix, and it opens
# the next piece before discovering EOF -- hence a third, empty file.
try split  'split -f'        'banana apple|cherry apple|' \
    "./split -2 -f sp_ s.txt && for f in sp_aa sp_ab; do printf '%s|' \"\$(tr '\n' ' ' < \$f | sed 's/ \$//')\"; done"
try col    'col passes text' 'plain' "printf 'plain\n' | ./col"
try pr     'pr has a header' '1' "./pr s.txt | head -3 | grep -c 's.txt'"
# V8's vis renders a control character as a three-digit octal escape; the
# \^A form is a later BSD addition.
# Compared through sed so the backslash survives the harness's own echo.
try vis    'vis'             'a-BS-001b' "printf 'a\001b\n' | ./vis | head -1 | sed 's/\\\\/-BS-/'"
try ascii  'ascii'           '1' "./ascii | grep -c 'nul'"
try deroff 'deroff'          'text' "printf '.PP\ntext\n' | ./deroff | tr -d ' \n'"

# ls exercises more of the shim than anything else here: the directory layer,
# stat translation, the uid/gid lookups and ctime(3) all at once.
mkdir -p lsdir && : > lsdir/alpha && : > lsdir/beta && mkdir -p lsdir/sub
try ls     'ls'               'alpha beta sub' "./ls lsdir | tr '\n' ' ' | sed 's/ \$//'"
try ls     'ls -a includes .' '2' "./ls -a lsdir | grep -c '^\\.'"
try ls     'ls -l is one line per file, plus total' '4' "./ls -l lsdir | wc -l | tr -d ' '"
try ls     'ls -l marks the directory' 'd' "./ls -l lsdir | awk '\$NF==\"sub\"{print substr(\$1,1,1)}'"

# pwd is here rather than with the filters because it exercises the shim's
# directory layer end to end: getwd(3) walks to the root matching each entry
# against stat("."), which is what caught the APFS firmlink problem.
#
# THIS CASE FAILED ONCE, THE CAUSE WAS FOUND, AND IT IS NOW FIXED -- so the
# case runs unconditionally again, and the case below it guards the fix.
#
# getwd()'s same-device arm finds the entry in `..' whose d_ino equals
# stat(".")'s st_ino, and stops there.  Both sides pass through
# v8sys_fold_ino(), which used to be a pure 64-bit-to-16-bit XOR fold, so two
# entries in one directory could share a d_ino and the walk named whichever
# readdir yielded first.  Measured over EVERY directory in $TMPDIR, sampling 60
# from each class: 1545 directories, 121 in a collision group, right 32/60
# inside against 60/60 outside -- 47% wrong, and 6 of those printed another
# directory's path and exited 0.
#
# The fold is now an append-only process-local table (shim/v8sys/dir.c), and
# re-measured on the same host afterwards: 1752 of 1752 directories right, 0
# wrong, 0 `getwd: can't change back'.  src/libc/gen/PORTING.md has the whole
# account, including why a confirming stat() does NOT fix it and why the
# cheaper snapshot-local fix cannot work.
#
# Two instrument errors on the way, both in the flattering direction and both
# the same shape as the ones CLAUDE.md already records.  Comparing V8's pwd
# against the UNRESOLVED path made all 12 samples read as failures, because
# $TMPDIR is behind the /private firmlink; and a collision list truncated to
# the first 12 of 109 groups made a colliding directory look like a
# non-colliding control that failed, which would have falsified the diagnosis.
# Classify the whole population, then sample it.
#
# THE HYPOTHESIS THIS REPLACES WAS WRONG, and the experiment that cleared it
# was wrong in an instructive way.  It blamed v8sys_dirsize's two passes over a
# changing directory, and tested that with 400 walks against a parent being
# churned -- which could not have found the real cause, because the churn
# created its own entries and APFS hands out CONSECUTIVE inodes, which the old
# fold separated perfectly.  A collision needs inodes spread over time, which
# is why no test can manufacture one and why this case fired roughly never.
try pwd 'pwd' "$(/bin/pwd -P)" "./pwd"

# ...AND THE PROPERTY BEHIND IT, WHICH IS WHAT A FRESH DIRECTORY CANNOT SHOW.
# pwd passing says the walk found the right entry once.  What the fix actually
# promises is that no two entries of one directory share a v7 inode in one
# process, and the only place that can be exercised is a directory whose inodes
# were handed out over months -- $TMPDIR.  Measured here: 6729 entries, 6729
# distinct values, where the old fold gave 519 entries sharing 257 values.
#
# It asks through V8's own `ls -i', so it measures the implementation under
# test rather than a second spelling of the rule -- the mistake this repo
# refuses everywhere else, and one that would go stale the moment the map
# changes again.
#
# A DUPLICATE IS NOT AUTOMATICALLY A FAULT: two names for one file share a host
# inode and MUST share a v7 one.  So a duplicate is reconciled against the host
# before it is called a failure, and that reconciliation costs nothing on the
# normal path because the duplicate list is empty.
pwdpar=$(dirname "$TMP")
"$ROOT/rootfs/bin/ls" -ia "$pwdpar" > "$TMP/inos" 2>/dev/null
pwdn=$(wc -l < "$TMP/inos" | tr -d ' ')
if [ "${pwdn:-0}" -lt 2 ] || [ "${pwdn:-0}" -gt 65535 ]; then
	# Above 65535 the property cannot hold -- there are not that many
	# numbers -- so this is a genuine skip rather than a weak pass.
	echo "pwd: inode distinctness NOT EXERCISED -- $pwdpar holds ${pwdn:-0}" \
	     "entries, and the property needs a directory with a history."
else
	# A pass on a nearly empty directory is nearly vacuous, and check()
	# says nothing when it passes.  Two entries collide with probability
	# about n^2/2*65536, so a run under the birthday bound reports how
	# little it proved instead of passing quietly.  A fresh CI runner is
	# always in this arm; this host is not.
	[ "$pwdn" -ge 256 ] || echo "pwd: inode distinctness WEAKLY exercised" \
	     "-- only $pwdn entries in $pwdpar, against a birthday bound of 256."
	pwdbad=0
	for v in $(awk '{print $1}' "$TMP/inos" | sort | uniq -d); do
		# every name sharing v7 inode v, stat'd on the host: one
		# distinct host inode means they are links to one file
		nh=$(awk -v v="$v" '$1==v{sub(/^[0-9]+[ \t]+/,""); print}' \
		         "$TMP/inos" |
		     while IFS= read -r nm; do
			/usr/bin/stat -f '%i' "$pwdpar/$nm" 2>/dev/null
		     done | sort -u | wc -l | tr -d ' ')
		[ "${nh:-0}" -le 1 ] || pwdbad=$((pwdbad+1))
	done
	check "no two files of $pwdpar share a v7 inode ($pwdn entries)" \
	      '0' "$pwdbad"
fi

# ---------------------------------------------------------------------------
# EVERY IMPORTED SINGLE-FILE COMMAND MUST BE INSTALLED.
#
# The cases above run $(ROOTFS)/bin binaries, so a command that builds but is
# never installed is invisible to them -- it simply is not exercised, and the
# suite gets shorter rather than redder.  That is how seven commands (ascii,
# bcd, cal, morse, ptx, units, vis) sat compiled-but-unshipped: the old harness
# compiled them into a temp directory, so they looked covered.
#
# `cc' is the one deliberate exception: the driver has its own rule because it
# links against libv8c, which a driver has to compile first (the cc-seed cycle).
#
# This asked only about /bin, because /bin was the only directory this port
# installed into.  V8 has four -- /bin is a 56-entry root-filesystem set from
# when / had to fit on one pack, /usr/bin holds most of the world, /etc the
# section-8 tools and /lib cpp -- so the question is WHETHER, and the case
# below is WHERE.  Splitting them that way is what stops this one from being
# weakened by the layout change that made it necessary.
missing=
for src in "$ROOT"/src/cmd/*.c; do
	name=$(basename "$src" .c)
	[ "$name" = cc ] && continue
	v8which "$name" >/dev/null || missing="$missing $name"
done
if [ -z "$missing" ]; then
	pass=$((pass+1))
else
	fail=$((fail+1))
	echo "FAIL these src/cmd commands are built but not installed:$missing"
fi

# ...AND IN THE DIRECTORY V8 PUT IT IN, which is the half the check above cannot
# make on its own and which this port got wrong for forty-one commands.
#
# The Makefile derives the destination from third_party/.../cmd/Admin's tables,
# which is Bell Labs' BUILD description.  This case deliberately asks a
# DIFFERENT source -- the shipped /bin, /usr/bin, /lib and /etc directories of
# the distribution itself -- so the two check each other rather than one being
# read back twice.  They agree on every command this port installs; the five
# that upstream shipped source for and never installed (bcd, head, morse,
# unexpand, yes) have no shipped answer, so only Admin/dest has an opinion and
# they are excused by name.
SHIPPED=$REPO/third_party/Research-Unix-v8/v8
shipdir() {
	for d in bin usr/bin lib etc usr/lib; do
		[ -e "$SHIPPED/$d/$1" ] && { echo "$d"; return 0; }
	done
	return 1
}
misplaced=
for src in "$ROOT"/src/cmd/*.c "$ROOT"/src/cmd/*/; do
	name=$(basename "${src%/}" .c)
	case $name in
	cc|ccom) continue ;;			# built into the toolchain
	bcd|head|morse|unexpand|yes) continue ;;	# never shipped; see above
	esac
	want=$(shipdir "$name") || continue	# not a program V8 shipped
	got=
	for d in bin usr/bin lib etc usr/lib; do
		[ -x "$V8ROOT/$d/$name" ] && { got=$d; break; }
	done
	[ -z "$got" ] && continue		# the completeness cases above own this
	[ "$want" = "$got" ] || misplaced="$misplaced $name(/$got,want /$want)"
done
if [ -z "$misplaced" ]; then
	pass=$((pass+1))
else
	fail=$((fail+1))
	echo "FAIL installed where V8 did not put them:$misplaced"
fi

# WHERE THE TWO SOURCES DISAGREE, pinned so the Makefile's reasoning stays
# falsifiable.  $(call v8dest,...) reads binfiles/etcfiles/libfiles and stops;
# Admin/dest reads a fourth table, ulibfiles, that the Makefile deliberately
# does NOT -- and the comment there justifies the omission by naming exactly
# which six of the 128 listed commands the tables and the shipped tree disagree
# about.  A comment naming six things is a claim, and the tables are vendored
# files that a future import could change.  So recompute the set.
#
# `man' is the one that matters: it is in ulibfiles, so dest says /usr/lib,
# while the shipped tree has /usr/bin -- and this port installs it.  Omitting
# the arm is what makes us agree with the tree.  Adding it "for completeness"
# would break man and this case would not notice, because it asks about
# upstream rather than about us; the case above is the one that would.
A=$ROOT/src/cmd/Admin
adest() {
	grep -qx "$1" "$A/binfiles"  && { echo bin;     return; }
	grep -qx "$1" "$A/etcfiles"  && { echo etc;     return; }
	grep -qx "$1" "$A/libfiles"  && { echo lib;     return; }
	grep -qx "$1" "$A/ulibfiles" && { echo usr/lib; return; }
	echo usr/bin
}
differ=
# -f ON BOTH SIDES, and getting this wrong gave two different wrong answers
# before it gave the right one.  A name in these tables can be a COMMAND or a
# DIRECTORY: font, macros, term and tmac are directories under /usr/lib, and
# lint and uucp are a command in /usr/bin AND a directory of the same name in
# /usr/lib.  Ask with -e and the six become ten; ask with -e on one side and -f
# on the other and lint and uucp look like disagreements about a command when
# the tables are describing the directory.  The question this port has is only
# ever about where a command goes, so both sides ask for a regular file.
shipfile() {
	for d in bin usr/bin lib etc usr/lib; do
		[ -f "$SHIPPED/$d/$1" ] && { echo "$d"; return 0; }
	done
	return 1
}
for name in $(cat "$A"/binfiles "$A"/etcfiles "$A"/libfiles "$A"/ulibfiles | sort -u); do
	d=$(adest "$name")
	[ -f "$SHIPPED/$d/$name" ] && continue		# they agree
	shipfile "$name" >/dev/null || continue		# no command of that name
	differ="$differ $name"
done
check 'the tables and the shipped tree disagree about exactly these six' \
   ' crontab lint login man pstat uucp' "$differ"

# A THIRD SOURCE, AND THE CASE ABOVE STRUCTURALLY CANNOT SEE IT.  Eleven of the
# imported makefiles say where they install -- `mv lex $(DESTDIR)/usr/bin',
# `cp ps /bin', `D=/etc/quot' -- which is an upstream statement about a
# destination that neither Admin/dest nor the shipped tree is.  It matters
# because the pair above can only compare programs the distribution SHIPPED,
# and `dump' is not one: it is in none of binfiles, etcfiles or libfiles
# either, so Admin/dest answers /usr/bin BY FALL-THROUGH, which is "nobody
# said" rather than "V8 said".  Its own Makefile does say: `mv dump
# $(DESTDIR)/etc', which is also where its two siblings restor and dumpdir go.
# The Makefile's $(MKFILEETC) is that one exception, and this recomputes the
# whole set so the list cannot quietly grow.
#
# Only lines that move or copy THE PROGRAM ITSELF count.  lex's makefile also
# does `cp ncform $(DESTDIR)/usr/lib/lex' and yacc's `cp yaccpar
# $(DESTDIR)/usr/lib' -- data files, and both would read as a destination for
# the program if the second field were not checked.  sh's `mv /bin/sh /bin/osh'
# is skipped the same way; the line that counts is `cp sh /bin/sh' below it.
# Make variables have to be expanded or the answer is the literal `$B'.
# tsort's makefile is `B = /usr/bin' then `cp tsort $B', and read unexpanded it
# looks like a disagreement -- which is how this parser first reported one.
# A single non-recursive pass is enough for 1985 makefiles; note `$B' with no
# parentheses is make's single-character form and has to be handled too.
mkdest() {	# mkdest <directory> <program> -- where its OWN makefile puts it
	_m=$(ls "$ROOT/src/cmd/$1"/[Mm]akefile 2>/dev/null | head -1)
	[ -n "$_m" ] || return 1
	shift
	awk -v p="$1" '
	    { line[NR] = $0 }
	    /^[A-Za-z_][A-Za-z0-9_]*[ \t]*=/ {
		n = $0; sub(/[ \t]*=.*/, "", n); gsub(/[ \t]/, "", n)
		v = $0; sub(/^[^=]*=[ \t]*/, "", v); sub(/[ \t]*$/, "", v)
		var[n] = v }
	    END {
		# TWO CANDIDATES, BECAUSE A MAKEFILE MAY STATE TWO
		# DESTINATIONS AND ONLY ONE OF THEM IS THE INSTALL.  ex has
		# "ninstall: ... cp a.out ${NBINDIR}/ex" with NBINDIR=/usr/new
		# at line 111 and "install: ... cp a.out ${BINDIR}/ex" with
		# BINDIR=/usr/bin at line 125 -- BSD staging directory first,
		# real install second -- and a loop that exits on the first
		# match reads the staging one.  /usr/new is in no shipped V8
		# tree.  So a recipe under a target named install wins, and
		# anything else is only a fallback for the makefiles that name
		# their install target something else or use the D= form.
		best = ""; bestinst = ""; tgt = ""
		for (i = 1; i <= NR; i++) {
			s = line[i]
			# Recipe lines begin with whitespace; a target line does
			# not.  Test BEFORE the leading tab is stripped below.
			if (s !~ /^[ \t]/ && s ~ /^[A-Za-z_][A-Za-z0-9_.\-]*[ \t]*:/) {
				tgt = s; sub(/[ \t]*:.*/, "", tgt)
			}
			# BOTH SPELLINGS.  DESTDIR is an install prefix passed
			# on the command line, so it is never one of the
			# variables defined in the file and the generic
			# expansion below cannot reach it -- it needs this
			# explicit strip, and until csh arrived the strip knew
			# only the paren form.  csh installs with a brace
			# DESTDIR, which read literally looks like a program
			# going to a directory named after the variable, and
			# it was reported as a ninth disagreement.  Same shape
			# as the tsort case (B = /usr/bin taken literally): a
			# false positive in the one sweep whose whole job is
			# finding real ones.
			#
			# NOTE THE QUOTING.  The note further down says no
			# apostrophes in here; what it does not say is that
			# the backquote-plus-apostrophe idiom used everywhere
			# ELSE in this file supplies one.  Those comments are
			# fine because they sit OUTSIDE this awk program.
			# Writing the idiom here, in a comment about this very
			# sweep, is how that was rediscovered -- twice.
			gsub(/\$\(DESTDIR\)/, "", s)
			gsub(/\$\{DESTDIR\}/, "", s)
			for (n in var) {
				gsub("\\$\\(" n "\\)", var[n], s)
				gsub("\\$\\{" n "\\}", var[n], s)
				if (length(n) == 1) gsub("\\$" n, var[n], s)
			}
			# strip the leading tab of a recipe line first --
			# split on /[ \t]+/ makes f[1] the empty string
			# otherwise, and every mv/cp line stops matching.
			# (No apostrophes in here: the whole program is
			# inside a single-quoted shell string.)
			sub(/^[ \t]+/, "", s)
			nf = split(s, f, /[ \t]+/)
			if (s ~ /^D[ \t]*=/) {
				d = s; sub(/^D[ \t]*=[ \t]*/, "", d)
			} else if (nf >= 3 && (f[1] == "mv" || f[1] == "cp")) {
				# THE DESTINATION IS THE LAST FIELD, NOT f[3], and
				# the program may be any of the sources.  This was
				# f[2] == p with d = f[3], which is right for
				# "cp sh /bin/sh" and WRONG for the pack makefile
				# line "cp pack unpack /usr/bin" -- there f[3] is
				# the second PROGRAM, so the sweep reported packs
				# destination as unpack and called it a
				# disagreement.  A false positive in the one case
				# whose whole job is finding real ones.
				#
				# (Still no apostrophes in here, for the reason
				# the note above gives: the whole program is one
				# single-quoted shell string, and an apostrophe in
				# a COMMENT closes it just as well as one in code.
				# Measured -- that is how this edit first failed.)
				# ...OR THE DESTINATION NAMES IT, which is the
				# THIRD bug found in this parser and the largest
				# by population.  Requiring p among the SOURCES
				# assumes the installed file is spelled with the
				# name of the program, and NINE imported
				# makefiles do not spell it that way: six use
				# the a.out idiom (awk eqn ex m4 qed ratfor,
				# whose target is a.out and whose install arm is
				# "cp a.out /usr/bin/NAME"), two use the shell
				# script split (spell struct, "cp NAME.sh
				# /usr/bin/NAME"), and plot installs $(PROGS).
				# Every one of them was silently reported as
				# stating no destination at all.
				#
				# grap escaped only by an accident of ORDERING:
				# its "cp grap /usr/bin" is at line 12 and its
				# "cp a.out /usr/bin/grap" at line 31, and this
				# loop exits on the first match.
				#
				# The vacuity guard below could not see any of
				# it -- it requires ten makefiles to state a
				# destination, and the ones that do kept the
				# count up while a fifth of the population went
				# unread.  A threshold guard bounds how wrong a
				# sweep can be, never how incomplete.
				# THE BASENAME TEST APPLIES TO A SINGLE-SOURCE cp
				# ONLY, and that is cp semantics rather than a
				# heuristic: with more than one source the
				# destination MUST be a directory.  Without the
				# nf==3 guard, struct line 27 --
				# "cp structure beautify /usr/lib/struct" --
				# matches, because /usr/lib/struct is a DIRECTORY
				# named struct holding structure and beautify,
				# and the sweep reported struct as installing to
				# /usr/lib.  Measured: a false positive this very
				# fix introduced, the same shape as the f[3] bug
				# it sits beside.
				found = 0
				for (k = 2; k < nf; k++) if (f[k] == p) found = 1
				if (nf == 3) { b = f[nf]; sub(/^.*\//, "", b)
					       if (b == p) found = 1 }
				if (!found) continue
				d = f[nf]
			} else continue
			sub("/" p "$", "", d); sub(/^\//, "", d)
			if (d == "") continue
			if (best == "") best = d
			if (tgt == "install" && bestinst == "") bestinst = d
		}
		if (bestinst != "") print bestinst
		else if (best != "") print best }
	' "$_m"
}
# THE CANDIDATE SET IS A UNION, AND FOR MOST OF THIS SWEEP'S LIFE IT WAS HALF
# OF ONE.  The loop asked `mkdest <directory>' and so could only ever see a
# program whose name IS its directory's -- which is every program the port had
# imported until calendar, whose makefile installs calendar1, calendar2,
# calendar3 and calendar4 out of a directory called calendar.  Four genuine
# disagreements, invisible by construction: the makefile says /usr/lib for all
# four and Admin/dest answers /usr/bin by fall-through, which is cpp's exact
# pattern and the thing this sweep exists to find.  Structurally the same
# oversight as the f[3] bug two batches ago -- a parser correct for every input
# it had been given.
#
# THE SECOND HALF CANNOT REPLACE THE FIRST, which is why this is a union.
# Install-line names are filtered to those the SHIPPED TREE has as executables,
# because an install line also names headers, tables and scripts; and `dump' is
# a program V8 did not ship at all, so that filter drops it while the directory
# name keeps it.  Take either source alone and coverage goes down.
mkprogs() {	# every program name a directory's makefile installs
	_m=$(ls "$ROOT/src/cmd/$1"/[Mm]akefile 2>/dev/null | head -1)
	[ -n "$_m" ] || return 0
	awk '{ sub(/^[ \t]+/, ""); n = split($0, f, /[ \t]+/)
	       if (n >= 3 && (f[1] == "cp" || f[1] == "mv"))
		       for (k = 2; k < n; k++) print f[k] }' "$_m" |
	sort -u | while read -r _n; do
		case "$_n" in */*|'$'*|-*|'') continue;; esac
		for _p in bin usr/bin lib etc usr/lib; do
			if [ -f "$SHIPPED/$_p/$_n" ] && [ -x "$SHIPPED/$_p/$_n" ]; then
				echo "$_n"; break
			fi
		done
	done
}
mkdiffer= mkseen=
for d in "$ROOT"/src/cmd/*/; do
	dir=$(basename "$d")
	for name in $dir $(mkprogs "$dir"); do
		case " $mkseen " in *" $name "*) continue;; esac
		want=$(mkdest "$dir" "$name"); [ -n "$want" ] || continue
		mkseen="$mkseen $name"
		got=$(adest "$name")
		[ "$want" = "$got" ] || mkdiffer="$mkdiffer $name($want,dest=$got)"
	done
done
# The sweep has to have found them, or the disagreement set is empty for the
# wrong reason -- tests/cpp's failure exactly.
if [ "$(echo $mkseen | wc -w | tr -d ' ')" -ge 10 ]; then pass=$((pass+1))
else fail=$((fail+1)); echo "FAIL only $(echo $mkseen | wc -w) makefiles state a destination:$mkseen"; fi

# AND A THRESHOLD BOUNDS HOW WRONG A SWEEP CAN BE, NEVER HOW INCOMPLETE IT IS.
# The guard above passed throughout the whole life of a parser that could not
# read NINE of the imported makefiles -- a fifth of the population -- because
# the ones it could read kept the count over ten.  What it could not see was an
# install line whose SOURCE is not spelled with the name of the program:
#
#   cp a.out /usr/bin/ratfor      awk eqn ex m4 qed ratfor -- target is a.out
#   cp struct.sh /usr/bin/struct  spell struct -- the shell-script split
#
# grap has the a.out form too and escaped by an accident of ORDERING, its
# "cp grap /usr/bin" sitting nineteen lines above its "cp a.out /usr/bin/grap".
#
# So the guard is a RELATION now rather than a floor: every directory whose
# makefile installs the program under the directory's own name must resolve.
# That is derived from the tree each run, so a tenth such makefile is covered
# the day it is imported and nobody edits this file.  The a.out set is named
# explicitly beside it because those are the six the threshold could not see,
# and a case aimed at the defect is what says the sweep discriminates.
mkblind=
for name in awk eqn ex m4 qed ratfor spell struct; do
	[ -d "$ROOT/src/cmd/$name" ] || continue
	[ -n "$(mkdest "$name" "$name")" ] || mkblind="$mkblind $name"
done
check 'the sweep reads a makefile whose install source is not the program name' \
      '' "$mkblind"

# The two that are not merely "seen" but were being read WRONG, one each for
# the two halves of the fix.  ex states two destinations -- ninstall to
# /usr/new, install to /usr/bin -- and a first-match loop takes BSD staging.
# struct line 27 is "cp structure beautify /usr/lib/struct", where the trailing
# struct is a DIRECTORY and not the program, which is why the basename test is
# restricted to a single-source cp.
check 'ex resolves to its install arm, not ninstall staging' \
      'usr/bin' "$(mkdest ex ex)"
check 'a multi-source cp does not read its directory as the program' \
      'usr/bin' "$(mkdest struct struct)"
# THREE, and ALL THREE are programs that appear in no table at all, so
# Admin/dest is answering by fall-through in each -- which is what makes the
# third source worth consulting rather than a curiosity.  `cpp' is the one that
# shows the fall-through is simply wrong: its makefile says /lib, the SHIPPED
# TREE says /lib, and dest says /usr/bin.  This port already puts cpp in /lib,
# but by accident rather than by derivation -- it is a toolchain target with its
# own rule and never goes through $(call v8dest,...).  dump had no such accident
# available, so the Makefile's $(MKFILEETC) follows the makefile deliberately.
#
# diff3 IS THE THIRD AND IT ARRIVED WITH BATCH 2C, which is this case doing
# exactly what it was written for -- it went red on the import rather than the
# import going in unnoticed.  Its makefile says `mv diff3 /usr/lib' and the
# shipped tree HAS usr/lib/diff3, so two sources again outvote the fall-through.
# The Makefile does not route diff3 through v8dest at all: the binary is
# /usr/lib/diff3 and the COMMAND is a shell script in /usr/bin, which is the
# spell/spellprog split and not a destination question.
# EIGHT NOW, AND FIVE OF THEM ARRIVED BY WIDENING THE SWEEP RATHER THAN BY
# IMPORTING ANYTHING.  Batch 2d made the candidate set a union (see mkprogs
# above), and the moment the sweep could see programs whose names differ from
# their directory's it found five it had never asked about: calendar1..4, and
# `diffh', which has been installed to /usr/lib by this port since Wave A and
# was simply never examined.  Every one is the same shape as cpp -- the program
# is in no Admin table, so dest answers usr/bin by FALL-THROUGH, and both the
# makefile and the shipped tree say otherwise.  Two sources against a
# non-answer.
#
# So the set is not "three things upstream got wrong"; it is "every program
# upstream installs somewhere other than /usr/bin without writing it in a
# table", and the count moves when the SWEEP improves as well as when the tree
# does.  Ordered by directory, then by makefile appearance, which is why diffh
# (src/cmd/diff) precedes diff3 (src/cmd/diff3).
check 'an imported makefile and Admin/dest disagree about exactly these eight' \
   ' calendar1(usr/lib,dest=usr/bin) calendar2(usr/lib,dest=usr/bin) calendar3(usr/lib,dest=usr/bin) calendar4(usr/lib,dest=usr/bin) cpp(lib,dest=usr/bin) diffh(usr/lib,dest=usr/bin) diff3(usr/lib,dest=usr/bin) dump(etc,dest=usr/bin)' \
   "$mkdiffer"
# ...and the port sides with the makefile in every one of the five new cases,
# which is what says the widening found a blind spot rather than a bug.  diffh
# and the four calendar helpers are all installed where their makefiles say.
check 'and the port already installs all five where the makefile says' 'usr/lib x5' \
   "$(n=0; for f in diffh calendar1 calendar2 calendar3 calendar4; do
        [ -f "$V8ROOT/usr/lib/$f" ] && n=$((n+1)); done; echo "usr/lib x$n")"
check 'and for cpp the shipped tree sides with the makefile' 'lib' "$(shipfile cpp)"
# diff3 needs a DIFFERENT assertion from cpp, and the first draft used cpp's
# and went red: shipfile() answers with the first directory holding that NAME,
# and the shipped tree has BOTH -- usr/bin/diff3 the script and usr/lib/diff3
# the binary.  So `usr/bin' is a correct answer to a question diff3 does not
# raise.  What settles the disagreement is that the makefile-named location
# exists AT ALL, and that the /usr/bin entry is not the program.
check 'and for diff3 the shipped tree has the binary where its makefile said' \
   'binary-in-usr-lib script-in-usr-bin' \
   "$([ -f "$SHIPPED/usr/lib/diff3" ] && printf 'binary-in-usr-lib '
      head -1 "$SHIPPED/usr/bin/diff3" 2>/dev/null | grep -q '^e=' &&
        printf 'script-in-usr-bin')"
# ...and the Makefile followed the makefile rather than the fall-through.
instdir() { for d in bin usr/bin lib etc; do
		[ -x "$V8ROOT/$d/$1" ] && { echo "$d"; return 0; }; done; return 1; }
check 'so dump is installed in /etc, beside restor and dumpdir' \
   'etc etc etc' \
   "$(for n in dump restor dumpdir; do instdir "$n"; done | tr '\n' ' ' | sed 's/ $//')"
check 'and cpp in /lib, where its own makefile and the tree both put it' \
   'lib' "$(instdir cpp)"

# ...AND THE SAME QUESTION OF THE DIRECTORIES, which is where the glob above
# was blind. `src/cmd/*.c' matches src/cmd/cat.c and never src/cmd/mv/mv.c, so
# a command upstream happens to keep in a directory of its own was invisible to
# the check whose whole job is finding commands that were imported and never
# shipped. ELEVEN were hiding there -- cp, dc, ed, factor, mkdir, mv, primes,
# rmdir, sed, fmt, tsort -- so the V8 world had no cp, no mv and no sed.
#
# It surfaced from the far end instead: four upstream makefiles in the rung-5
# sweep died on `Cannot load mv'.
#
# Not every directory is a /bin command -- the compiler passes, the Wave C
# document tools install to /usr/bin, and refer and spell build several
# programs under other names -- so this asks only that SOMETHING executable by
# that name was installed somewhere in the rootfs.
dmissing=
for d in "$ROOT"/src/cmd/*/; do
	name=$(basename "$d")
	ls "$d"*.c >/dev/null 2>&1 || continue
	case "$name" in
	# built into the toolchain rather than installed under their own name
	ccom|cc) continue ;;
	# struct's and csh's exemptions are BOTH GONE: each is built and
	# installed now, so the guard verifies them like anything else.  A skip
	# left behind after its reason expires is a hole, and this list is a set
	# of claims.  csh's stood for exactly one commit and its stated reason
	# -- a SIGCHLD that never wakes pjwait -- was wrong about the layer: the
	# signal machinery was correct throughout and sh.proc.h's `short p_pid'
	# was truncating the host pid so pchild could never match it.
	# src/cmd/plot builds `tek' and `hpplot', never a program called `plot'.
	# The COMMAND of that name is a 16-line shell dispatcher V8 ships with no
	# source at all -- usr/src/cmd/plot's makefile does not install it -- so
	# this directory can never satisfy the rule by its own name.  Asserted the
	# other way round below: tek must be installed, and hpplot must not,
	# because libcurses is unported.  src/libplot/PORTING.md.
	plot) continue ;;
	esac
	find "$V8ROOT" -name "$name" -type f -perm -u+x 2>/dev/null | grep -q . ||
		dmissing="$dmissing $name"
done
if [ -z "$dmissing" ]; then
	pass=$((pass+1))
else
	fail=$((fail+1))
	echo "FAIL these src/cmd DIRECTORIES are imported but nothing is installed:$dmissing"
fi

# ...and the ones just added must actually run, not merely exist.  A binary
# that dies on startup still satisfies the check above -- which is exactly what
# units and ptx did until their data files were installed too.
check 'installed cal runs'   'February 1985' \
    "$("$V8ROOT/usr/bin/cal" 2 1985 | head -1 | sed 's/^ *//;s/ *$//')"
check 'installed vis runs'   'a\001' "$(printf 'a\001\n' | "$V8ROOT/usr/bin/vis")"
check 'installed ascii runs' '1'     "$("$V8ROOT/usr/bin/ascii" | grep -c 'nul')"
# bcd and morse read STDIN, not argv.
check 'installed bcd punches a card' '1' \
    "$(echo AB | "$V8ROOT/usr/bin/bcd" | grep -c '^/AB')"
check 'installed morse runs' '1' "$(echo e | "$V8ROOT/usr/bin/morse" | grep -c 'dit')"
# units reads /usr/lib/units, and answers "no table" without it.  1 inch is
# 2.54 cm, so this checks the table was actually consulted rather than opened.
check 'installed units converts' '1' \
    "$(printf 'inch\ncm\n' | "$V8ROOT/usr/bin/units" 2>&1 | grep -c '2\.54')"
# ptx reads /usr/lib/eign (the "ignore" word list) and emits one .xx line per
# rotation.  Without the file it exits with "Cannot open  file /usr/lib/eign".
check 'installed ptx permutes' '3' \
    "$(printf 'alpha beta gamma\n' | "$V8ROOT/usr/bin/ptx" | grep -c '^\.xx')"

# --- the eleven that were imported into their own directories and never built
# Same rule as above: existing is not running. These are the ones the glob
# missed, and cp/mv/mkdir/rmdir are the ones upstream's makefiles reach for.
W=${TMPDIR:-/tmp}/wavea_dir.$$
mkdir -p "$W"; trap 'rm -rf "$W"' EXIT
echo hello > "$W/a"

"$V8ROOT/bin/cp" "$W/a" "$W/b" 2>/dev/null
check 'installed cp copies'  'hello' "$(cat "$W/b" 2>/dev/null)"
"$V8ROOT/bin/mv" "$W/b" "$W/c" 2>/dev/null
check 'installed mv moves'   'hello gone' \
    "$(cat "$W/c" 2>/dev/null) $([ -f "$W/b" ] && echo still || echo gone)"

# A FILE into an existing directory -- move()'s path, which has no length guard
# of its own and joins with sprintf. Distinct from mv-a-DIRECTORY below, which
# is the guarded path; getting those two the wrong way round makes a test that
# cannot see the bug it is named for.
mkdir -p "$W/into"; echo m > "$W/m"
mverr=$("$V8ROOT/bin/mv" "$W/m" "$W/into" 2>&1)
check 'mv file into a directory is silent' '' "$mverr"
check '...and the file arrives'  'm' "$(cat "$W/into/m" 2>/dev/null)"

# ...and the unguarded sprintf that the same change had widened: move() joins
# target and basename into char buf[MAXN] with no length check of its own. That
# is upstream's code, so it is not fixed -- but at MAXN 1024 an ordinary long
# filename no longer overruns it, where at 100 a 92-character name did.
long=$(printf 'n%.0s' 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 \
                     1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 \
                     1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 \
                     1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0)
echo L > "$W/$long"
"$V8ROOT/bin/mv" "$W/$long" "$W/into" 2>/dev/null
check 'a 110-character name survives the join' 'L' "$(cat "$W/into/$long" 2>/dev/null)"

# MV OF A DIRECTORY is the guarded path -- mvdir()'s
# `strlen(target) > MAXN-DIRSIZ-2', a RELATIONSHIP between the buffer and the
# longest name that can be appended to it. Raising DIRSIZ 14 -> 254 made that
# -156, so the guard fired on every such move with a message that is false.
#
# It cannot SUCCEED here either: mvdir renames the V7 way, link() then unlink(),
# and macOS refuses link() on a directory with EPERM. So the assertion is that
# it fails for the HONEST reason -- the same treatment as w's `No mem', and it
# doubles as the check that the -156 guard is gone. rename(2) is what a fix
# would use; src/cmd/mv/PORTING.md.
mkdir -p "$W/dsrc" "$W/ddst"
case "$("$V8ROOT/bin/mv" "$W/dsrc" "$W/ddst" 2>&1)" in
*"cannot link"*) pass=$((pass+1)) ;;
*) fail=$((fail+1))
   echo "FAIL mv of a directory: expected the link() refusal, got [$("$V8ROOT/bin/mv" "$W/dsrc" "$W/ddst" 2>&1)]" ;;
esac
"$V8ROOT/bin/mkdir" "$W/d" 2>/dev/null
check 'installed mkdir makes a directory' 'yes' "$([ -d "$W/d" ] && echo yes)"
"$V8ROOT/bin/rmdir" "$W/d" 2>/dev/null
check 'installed rmdir removes it'        'yes' "$([ ! -d "$W/d" ] && echo yes)"

# A NAME AT macOS's NAME_MAX. mkdir's pname[]/dname[] were 128 bytes, sized when
# a component was DIRSIZ = 14; at DIRSIZ 254 one legal name overruns both, and
# a 255-character one SIGSEGV'd *after* creating the directory and unlinking it
# again -- destroying what it made and reporting nothing.
# src/cmd/mkdir/PORTING.md.
# THE EXIT STATUS IS THE ASSERTION, not whether the directory appeared. With
# the 128-byte buffers the crash lands on the RETURN from mkdir(), after the
# work is done -- so `[ -d ... ]' is satisfied by a run that died of SIGSEGV,
# and a test that checks only for the directory passes on the broken build.
# Measured while mutating: exit 139, directory present.
n255=$(awk 'BEGIN{s="";while(length(s)<255)s=s "x";print substr(s,1,255)}')
"$V8ROOT/bin/mkdir" "$W/$n255" 2>/dev/null
mkrc=$?
check 'mkdir exits cleanly on a 255-character name' '0' "$mkrc"
check '...and the directory is there'               'yes' \
    "$([ -d "$W/$n255" ] && echo yes)"
"$V8ROOT/bin/rmdir" "$W/$n255" 2>/dev/null

# rmdir's name[] was 500 and strcpy(name, d) is its FIRST statement, so the
# argument alone overran it -- SIGBUS at 550, no filesystem access needed. The
# path need not exist; only the exit status matters, and 138 is the crash.
long550=$(awk 'BEGIN{s="";while(length(s)<550)s=s "y";print substr(s,1,550)}')
"$V8ROOT/bin/rmdir" "$long550" >/dev/null 2>&1
rc=$?
[ "$rc" -lt 128 ] && pass=$((pass+1)) ||
    bad_rmdir=1
[ "${bad_rmdir:-0}" = 1 ] &&
    { fail=$((fail+1)); echo "FAIL rmdir crashed on a 550-character argument (exit $rc)"; }

# sed is the one whose absence would have been felt hardest, and -n with a line
# address exercises the address parser rather than just the copy loop.
check 'installed sed selects a line' 'b' \
    "$(printf 'a\nb\nc\n' | "$V8ROOT/usr/bin/sed" -n '2p')"
check 'installed sed substitutes'    'xbc' \
    "$(printf 'abc\n' | "$V8ROOT/usr/bin/sed" 's/a/x/')"

# `sed -n l' ON A HIGH BYTE, which crashed. char is signed, so a byte >= 0200 is
# negative, `if(*p1 >= 040)' sent it down the control-character arm, and
# `trans[*p1]' indexed a 32-entry array of POINTERS with a negative subscript.
# The out-of-bounds read is upstream's; LP64 doubled the stride and Mach-O maps
# nothing there, so the VAX printed nonsense and this faulted. Newly reachable
# too -- the byte that does it is any UTF-8 continuation. src/cmd/sed/PORTING.md.
check 'sed -n l survives a UTF-8 byte' 'éx' \
    "$(printf '\303\251x\n' | "$V8ROOT/usr/bin/sed" -n l)"
utf8rc=$(printf '\303\251x\n' | "$V8ROOT/usr/bin/sed" -n l >/dev/null 2>&1; echo $?)
check '...and exits cleanly rather than crashing' '0' "$utf8rc"
# ...while the control characters it DOES have escapes for still get them: the
# fix must not have sent 0..037 down the printable arm as well. Asserted as a
# property rather than a literal, because trans[011] is ">\\t" -- a prefix
# character and then the escape -- and hard-coding that spelling tests the table
# rather than the branch. What matters is that no raw tab reaches the output.
tabout=$(printf 'a\tb\n' | "$V8ROOT/usr/bin/sed" -n l)
case "$tabout" in
*'\t'*) pass=$((pass+1)) ;;
*) fail=$((fail+1)); echo "FAIL sed -n l did not escape a tab: [$tabout]" ;;
esac
# ...and no RAW tab survives. od -c renders a real tab as the two adjacent
# characters \t, while the escape sed emits is a backslash and a t with od's
# column padding between them -- so a match here means the byte came through
# unescaped, which is the opposite of what the first case asks.
if printf 'a\tb\n' | "$V8ROOT/usr/bin/sed" -n l | od -An -c | grep -q '\\t'; then
	fail=$((fail+1)); echo "FAIL sed -n l let a raw tab through"
else
	pass=$((pass+1))
fi
# tsort is a real algorithm, so a cycle-free order is a real answer.
check 'installed tsort orders'  'a b c' \
    "$(printf 'a b\nb c\n' | "$V8ROOT/usr/bin/tsort" | tr '\n' ' ' | sed 's/ $//')"
# factor echoes the number, then each factor on its own indented line, then a
# blank one -- so the factors are lines 2..n-1 with the indentation stripped.
check 'installed factor factors' '7 13' \
    "$("$V8ROOT/usr/bin/factor" 91 | tail -n +2 | tr -d ' \t' | grep -v '^$' |
       tr '\n' ' ' | sed 's/ $//')"
check 'installed dc computes'   '42'   "$(echo '6 7 * p' | "$V8ROOT/usr/bin/dc")"
check 'installed fmt reflows'   'one two three' \
    "$(printf 'one\ntwo\nthree\n' | "$V8ROOT/usr/bin/fmt" | head -1)"
# ed is a line editor driven entirely by stdin; append, write, quit.
printf 'a\nhello ed\n.\nw %s/e.txt\nq\n' "$W" | "$V8ROOT/bin/ed" >/dev/null 2>&1
check 'installed ed writes a file' 'hello ed' "$(cat "$W/e.txt" 2>/dev/null)"
# primes takes a starting value and counts up.
check 'installed primes generates' '11 13 17' \
    "$(echo 11 | "$V8ROOT/usr/bin/primes" 2>/dev/null | head -3 | tr '\n' ' ' | sed 's/ $//')"
rm -rf "$W"

# date(1) -- the first piece of Phase 4, and the only groveler that needs no
# kernel state at all.  Compared against the host by FIELD rather than by whole
# string, because V8 predates the tz database and names the zone itself:
# "GMT+10:00" where macOS says "AEST".  That difference is authentic, so the
# test asserts the fields V8 and the host must agree on -- weekday, month, day
# and the time to the minute -- and deliberately not the zone name.
# LC_ALL=C ON THE HOST SIDE, because the locale is not what is under test.
# %a and %b render through LC_TIME, and V8's date has no locale at all -- it
# indexes a compiled-in English table. Neither the Makefile nor the CI workflow
# pins LANG, so the developer's environment decides: under fr_FR.UTF-8 the host
# says `jeu. aout' and all three of these fail, reading as a timezone bug in the
# shim rather than as a locale difference in the comparison.
check 'date agrees with the host on the day' \
    "$(LC_ALL=C /bin/date '+%a %b %e' | tr -s ' ')" \
    "$("$V8ROOT/bin/date" | awk '{print $1, $2, $3}' | tr -s ' ')"
check 'date agrees with the host to the minute' \
    "$(/bin/date '+%H:%M')" \
    "$("$V8ROOT/bin/date" | awk '{print substr($4,1,5)}')"
check 'date prints a four-digit year' "$(/bin/date '+%Y')" \
    "$("$V8ROOT/bin/date" | awk '{print $NF}')"

# ---------------------------------------------------------------------------
# THE ARGUMENT VECTOR RUNS OUT, AND V8 READS ONE MORE.
#
# argv[argc] is a NULL the kernel plants.  On the VAX reading through it landed
# on address 0, which is the first byte of crt0 -- V8 binaries are ZMAGIC, so
# N_TXTOFF is 1024 and the a.out header is never mapped -- and that byte is
# 0x00.  So a program that consumed one argument too many got a byte that was
# not '-' and not a digit, and carried on correctly.  macOS leaves page 0
# unmapped, so the same code SIGSEGVs.
#
# This block used to say the byte was 0207, the low byte of the magic.  Wrong,
# and it reached the same answer for every case here, which is exactly why it
# went unnoticed for months -- see PLAN.md S4i.
#
# ncheck was the first instance found and quot the second; sweeping the whole
# tree for the shape turned up nine more, of which these are the ones whose
# programs live in Wave A.  Every fix reproduces the VAX's ANSWER rather than
# merely dodging the fault, so these cases assert the behaviour and not just
# the exit status -- a `return' bolted in front of the deref would pass an
# exit-status-only test while changing what the program does.
#
# Each is the LAST argument on the line, which is the whole trigger.
# ---------------------------------------------------------------------------
echo
echo "  -- reading past the end of argv (address 0 was readable on a VAX)"

# THE WORST ONE, because it is not a dangling option at all: unexpand with no
# arguments is the primary documented use of the program.  Berkeley left the
# argc test out of one of a matched pair -- expand.c:20 has it.
printf 'a\tb\n' > ux.txt
check 'unexpand with no arguments reads stdin' "$(printf 'a\tb')" \
    "$("$(v8which unexpand)" < ux.txt 2>&1)"
check 'and exits 0 rather than faulting' '0' \
    "$("$(v8which unexpand)" < ux.txt >/dev/null 2>&1; echo $?)"
# The control: the sibling that always had the guard must not have changed.
check 'expand with no arguments still reads stdin' 'a       b' \
    "$(printf 'a\tb\n' | "$(v8which expand)" -8 2>&1)"

# join: the -o field list and -j both walk to the end of the line.  Upstream's
# answer for -o is the `else break' arm, after which argc is wrong and join
# prints its usage; for -j it is atoi(0) == 0.
check 'join -o running off the end prints usage' '1' \
    "$("$(v8which join)" -o 1.1 >/dev/null 2>&1; echo $?)"
check 'join -j1 with no number prints usage' '1' \
    "$("$(v8which join)" -j1 >/dev/null 2>&1; echo $?)"
# ...and join still joins, which is what says the guards did not break it.
printf '1 a\n2 b\n' > j1.txt
printf '1 x\n2 y\n' > j2.txt
check 'join still joins two files' '1 a x 2 b y' \
    "$("$(v8which join)" j1.txt j2.txt | tr '\n' ' ' | sed 's/ *$//')"

# yacc -o took the NULL as its output file name.  Upstream's answer is the
# fopen failure and the message that follows it; "" reaches the same one.
check 'yacc -o with no file name reports it cannot open' \
    'cannot open table file' \
    "$("$(v8which yacc)" -o 2>&1 | sed -n 's/.*\(cannot open table file\).*/\1/p')"

# AND THE CRASH THAT WAS NOT IN yacc.  With the output file unopenable, yacc's
# error() runs cleantmp(), which unlinks two temp names setup() had not yet
# assigned -- and unlink(0) faulted inside OUR shim, in dotlink(), which
# inspects the path before the syscall can answer EFAULT.  tests/v8sys has the
# syscall-level case; this one is the program that found it.
check 'and exits rather than faulting in the shim' '1' \
    "$("$(v8which yacc)" -o >/dev/null 2>&1; echo $?)"

# ptx came from the crash probe (tests/crash-probe.sh, PLAN.md S4j) rather than
# from the static audit, and its guard is the interesting part: `argc >= 2' is
# off by one because the loop above it tests `argc>1', so argc still counts the
# program name.  Two arguments means ptx and -w with nothing after.  The guard
# is left as upstream wrote it and the READ is what changed, so `ptx -w' still
# reaches the "Wrong width:" complaint a VAX printed rather than being quietly
# accepted -- which is what raising the guard to 3 would have done.
check 'ptx -w with no width gives the VAX diagnostic' 'ptx: Wrong width:' \
    "$("$(v8which ptx)" -w </dev/null 2>&1 | head -1 | sed 's/ *$//')"
check 'ptx -g with no gap exits rather than faulting' '1' \
    "$("$(v8which ptx)" -g </dev/null >/dev/null 2>&1; echo $?)"
# ...and both options still do their job, so the guards are not bare returns.
# two rotations of a two-word line, which is ptx doing its job.
check 'ptx -w 60 still sets the width and permutes' '2' \
    "$(printf 'alpha beta\n' | "$(v8which ptx)" -w 60 2>&1 | grep -c '^\.xx')"

# pr -m is from the same probe and is deliberately in a DIFFERENT class, which
# is why it sits apart from the block above: nothing here reads address 0.
# `pr -m' with no file operands never opens anything -- main() opens the -m
# files itself, and print()'s `Multi != 'm'' test is a proxy for "already
# open" that keeps saying yes when the operand list was empty.  get() then
# reads main()'s auto `fstr', which has never been written; measured, f_f was
# 0x8, so getc((FILE *)8) faulted.  An uninitialised FILE * is arbitrary on any
# machine, so there is no VAX answer -- pr.1 is the authority instead, and it
# states the rule with no exception for -m: "For no file arguments ... pr
# prints its standard input."
printf 'from-stdin\n' > prm.txt
check 'pr -m with no files reads stdin, as pr.1 says' 'from-stdin' \
    "$("$(v8which pr)" -m -t < prm.txt 2>&1)"
# -M is the second probe hit and not a second bug: TOLOWER folds the two.
check 'pr -M is the same option and no longer faults' 'from-stdin' \
    "$("$(v8which pr)" -M -t < prm.txt 2>&1)"
check 'and exits 0 rather than faulting' '0' \
    "$("$(v8which pr)" -m -t < prm.txt >/dev/null 2>&1; echo $?)"
# THE CONTROL, and it is the whole reason the test reads Nfiles rather than
# just calling mustopen unconditionally: doing that would reopen Files[0] over
# the top of the first named file with stdin, and -m would silently print the
# wrong column.  The crash goes away either way; only this case can tell them
# apart.
printf 'alpha\nbeta\n' > prm1.txt
printf 'ONE\nTWO\n'    > prm2.txt
check 'pr -m still merges two named files into columns' 'alpha ONE' \
    "$("$(v8which pr)" -m -t prm1.txt prm2.txt < prm.txt | sed -n 1p | tr -s ' ' | sed 's/ *$//')"

# lex's warning() is address-0's class and IS restorable, and it took a real
# specification to see it -- which is a limitation of the crash probe rather
# than of the audit.  The probe feeds every program /dev/null, so for a program
# that needs input all 53 invocations collapse onto the empty-spec path and it
# reported one bug 53 times with this one hidden behind it.
#
# warning() ends `fflush(errorf); fflush(fout); fflush(stdout);' and fout is
# NULL until lgate() opens it, which only happens once section one sees %%.
# The unknown-option arm runs long before that, so `lex -a spec.l' SIGSEGV'd on
# a spec lex compiles perfectly without the flag.
#
# The VAX answer is measured rather than assumed, and it is "do nothing":
# V8 binaries are ZMAGIC, so virtual 0 is crt0 rather than the a.out header,
# and those bytes read through the VAX struct _iobuf give _flag 0xd050.
# fflush opens `(iop->_flag&(_IONBF|_IOWRT))==_IOWRT' with _IOWRT 02 and
# _IONBF 04; 0xd050 & 06 is 0, so the && short-circuits before _base is read.
printf '%%%%\n[a-z]+\tprintf("word\\n");\n' > lx.l
check 'lex with an unknown option still warns' '0: (Warning) Unknown option a' \
    "$("$(v8which lex)" -a lx.l 2>&1 | head -1)"
check 'and still compiles the spec, which is the VAX answer' '0' \
    "$(rm -f lex.yy.c; "$(v8which lex)" -a lx.l >/dev/null 2>&1; echo $?)"
# The control that says the warning path is not just being skipped: the flag
# must change NOTHING about the output, so the two runs agree byte for byte.
"$(v8which lex)" lx.l >/dev/null 2>&1; mv lex.yy.c lx.plain
"$(v8which lex)" -a lx.l >/dev/null 2>&1
check 'and produces the same output as without the option' 'same' \
    "$(cmp -s lx.plain lex.yy.c && echo same || echo differs)"

# ...AND THE EMPTY SPECIFICATION IS DELIBERATELY STILL BROKEN, because a VAX
# broke too.  With no %% anywhere, ptail() reaches ctail()'s fprintf(fout,...)
# with fout still NULL; on a VAX fprintf got past the _IONBF test and _doprnt
# wrote through _ptr, which those same crt0 bytes make 0x08aed05e -- ~145 MB,
# far past a 56 KB lex's break, so SEGFLT.  Upstream's defect on upstream's
# hardware, so S1 says record it rather than patch it, as bcd and ls.c:259 are.
# Asserted on the OUTPUT rather than on the signal, because that is the part
# that is true on both machines and does not depend on how it dies.
check 'lex on a spec with no %% writes no output' 'none' \
    "$(rm -f lex.yy.c; "$(v8which lex)" < /dev/null >/dev/null 2>&1; \
       [ -f lex.yy.c ] && echo wrote || echo none)"

# ---------------------------------------------------------------------------
# THE SAME CLASS, FOUR MORE PROGRAMS -- and they arrived unmeasured, which is
# the part worth remembering.  cb, diffh, tar and cpio came in with the Wave A2
# batch and added 106 signal deaths to tests/crash-probe.sh, taking it from the
# 54 recorded in CLAUDE.md to 160.  Nothing went red, because the probe is a
# manual instrument whose expected output was a NUMBER IN PROSE, and a number
# only a human runs is a number that rots.  So the floor for these four is a
# case now; the sweep at the end of this block is the guard that the prose was.
#
# All four are the last-argument trigger, and all four are fixed to the VAX's
# ANSWER rather than to the absence of the fault -- 0x00 at address 0 is not
# '-' and not a digit, so each program had a defined behaviour to restore.
# ---------------------------------------------------------------------------
echo
echo "  -- cb, diffh, tar, cpio (the Wave A2 arrivals, 106 crashes)"

DIFFH=$V8ROOT/usr/lib/diffh	# not in v8which's three directories

# diffh: `while(*argv[1]=='-' ...)' is main's FIRST statement, so a bare diffh
# faults before anything is opened, and so does the run after the loop eats the
# last option.  The VAX read 0x00, which is not '-', so the loop did not run and
# the argc!=3 test below it reported the usage error.
printf 'a\nb\n' > dh1.txt; printf 'a\nc\n' > dh2.txt
"$DIFFH" </dev/null >/dev/null 2>dh.err; dhrc=$?
check 'diffh with no arguments does not die on a signal' 'lived' \
    "$([ $dhrc -lt 128 ] && echo lived || echo "signal $((dhrc-128))")"
check 'and gives the VAX diagnostic' 'diffh: must have 2 file arguments' \
    "$(cat dh.err)"
check 'diffh -b with the option last is the same' \
    'diffh: must have 2 file arguments' \
    "$("$DIFFH" -b </dev/null 2>&1 >/dev/null)"
# The paired case: -b must still be CONSUMED, or the guard could have been a
# `return' in front of the loop and both cases above would still pass.
check 'diffh -b still diffs, so the option is still consumed' '2,$c2,$' \
    "$("$DIFFH" -b dh1.txt dh2.txt </dev/null 2>/dev/null | head -1)"

# cb has TWO sites, which the 50 rather than 53 is the tell for: `-s' and `-j'
# are real options that continue, and bare cb reads stdin, so 52 - 2 = 50.
# 49 die in the default arm and the fiftieth in `-l'.
"$(v8which cb)" -a </dev/null >/dev/null 2>cb.err; cbrc=$?
check 'cb -a with the option last does not die on a signal' 'lived' \
    "$([ $cbrc -lt 128 ] && echo lived || echo "signal $((cbrc-128))")"
check 'and exits 1 as the VAX did' '1' "$cbrc"
# THE VAX PRINTED THE BYTE AT ADDRESS 0, so the message ends `option \0\n'.
# Asserted on the byte count as well as the text, because tr -d would hide a
# missing NUL and the NUL is the whole claim.
check 'and %c of address 0 puts a NUL in the message' '21' \
    "$(wc -c < cb.err | tr -d ' ')"
check 'the rest of which is upstream unchanged' 'cb: illegal option' \
    "$(tr -d '\000' < cb.err | sed 's/ *$//')"
# A FIDELITY CASE: this asserts a bug is STILL THERE.  *argv[1] is the first
# character of the NEXT argument where (*argv)[1] was meant -- a precedence
# bug, and upstream's own on upstream's hardware, so S1 forbids correcting it.
# Measured: `cb -a x.c' names x and `cb -Q zzz' names z, on any machine.
check 'cb still names the WRONG letter, which is upstream not us' \
    'cb: illegal option x' "$("$(v8which cb)" -a x.c </dev/null 2>&1 >/dev/null)"
# The second site, and its paired working case.
printf 'int main(){return 0;}\n' > cb.c
check 'cb -l with no number does not die either' '0' \
    "$(: | "$(v8which cb)" -l >/dev/null 2>&1; echo $?)"
check 'cb -l 60 still sets the width and reformats' 'int main(){' \
    "$("$(v8which cb)" -l 60 cb.c </dev/null 2>/dev/null | head -1)"

# tar's options are ONE bundled argument -- `tar cbf 2 f' -- so the trigger is
# a `b' in the last bundle with no number after it.  atoi read 0x00 as an empty
# string and returned 0, and the very next line rejects 0.
check 'tar with b last does not die on a signal' 'lived' \
    "$("$(v8which tar)" b </dev/null >/dev/null 2>/dev/null; \
       s=$?; [ "$s" -lt 128 ] && echo lived || echo "signal $((s-128))")"
check 'and gives the VAX diagnostic' 'Invalid blocksize. (Max 40)' \
    "$("$(v8which tar)" b </dev/null 2>&1 >/dev/null)"
# PAIRED, AND DELIBERATELY NARROW: what the fix owes is that `-b' still
# CONSUMES its argument, and that is provable without building an archive.
#
# The first version asserted a whole create/list/extract cycle and went red on
# a GitHub runner twice -- `tar cbf 2' dying of a SIGNAL in the create, with -f
# so no tape involved, on a freshly built rootfs.  That is a real tar defect on
# that machine and it does not reproduce here in 150 runs; task #77 has it.  A
# case that fails for a reason unrelated to what it asserts is how a suite
# stops being read, so these two assert the argument handling and nothing else.
#
# `tar tbf 2 nosuch.a' IS THE DISCRIMINATOR.  If `b' failed to consume "2",
# `f' would take it as the archive name and the message would say `cannot open
# 2'.  Naming the FILE is what proves the number was eaten.  (`tar b 99' would
# NOT discriminate: an unconsumed argument leaves nblock 0, and 0 and 99 are
# both rejected by the same line with the same words.)
check 'tar tbf 2 <name> consumes the 2, so the error names the file' \
    'tar: cannot open nosuch.a' \
    "$("$(v8which tar)" tbf 2 nosuch.a </dev/null 2>&1 >/dev/null)"
# And nothing here may leave a tape behind -- these use -f, but a leftover from
# anything else would be invisible and this suite has been bitten by that.
check 'and no tape was left in the rootfs' 'absent' \
    "$([ -e "$V8ROOT/dev/rmt1" ] && echo present || echo absent)"

# cpio dereferences argv[1] before anything else; the VAX got 0x00, which is
# not '-', and printed its usage.
"$(v8which cpio)" </dev/null >/dev/null 2>cp.err; cprc=$?
check 'cpio with no arguments does not die on a signal' 'lived' \
    "$([ $cprc -lt 128 ] && echo lived || echo "signal $((cprc-128))")"
check 'and prints usage and exits 2, as the VAX did' '2' "$cprc"
check 'the usage being upstream unchanged' \
    'Usage: cpio -o[acvB] <name-list >collection' "$(head -1 cp.err)"
# Paired: cpio must still round-trip, which is the half a `return' would break.
mkdir -p cpx && printf 'contents\n' > cf1
echo cf1 | "$(v8which cpio)" -o > cp.a 2>/dev/null
check 'cpio still round-trips a file through an archive' 'contents' \
    "$(cd cpx && "$(v8which cpio)" -i < ../cp.a >/dev/null 2>&1; cat cf1 2>/dev/null)"

# THE FLOOR, DERIVED RATHER THAN TRANSCRIBED -- one perl over 3 x 53
# invocations, the same shape tests/crash-probe.sh uses, and for its reasons:
# the real wait status rather than $?, because diffh's main ends in a BARE
# `return' and hands back whatever is in the register, which is how the crash
# probe once reported 254.
#
# fd 3 is closed EXPLICITLY.  V8's /dev/tty is /dev/fd/3, so whether cpio -i
# faults depends on whether a descriptor happened to be inherited -- a host
# property, and this suite has been bitten by those.  Closing it makes the
# condition ours.
#
# THE EXPECTATION IS DERIVED FROM tests/crash-probe.floor RATHER THAN SPELLED
# HERE, which matters more than it looks: two hand-written copies of one list
# agree with each other about a set that is wrong, and this project has been
# bitten by exactly that (v8fsd's and p9cl's errno tables agreed perfectly
# about a set missing seven names).  The floor file is the single source, and
# `?'-prefixed entries -- host-dependent, tolerated either way -- are excluded
# from both sides here for the same reason the probe excludes them.
#
# The expected value is a LIST, not a count, so a fix that broke one program
# and repaired another cannot cancel out.  cpio -i is deliberately NOT fixed:
# chgreel()'s fopen("/dev/tty") is unchecked and the fgets that follows is a
# getc, which is `--(p)->_cnt' -- a WRITE to virtual 0, and V8 binaries are
# ZMAGIC whose text is read-only shared, so a VAX faulted there too.  Third
# member of that family after pr.c's Ttyin and troff/hc.c's rcf, both of which
# S1 also leaves alone.  src/cmd/cpio.PORTING.md has the measurement.
# AND `tar' IS NOT IN THIS SWEEP, WHICH IS A CONTAINMENT DECISION RATHER THAN
# AN OMISSION.  `tar -c' with no -f creats its default tape, /dev/rmt1, and
# creat keys on the parent -- $V8ROOT/dev exists -- so it writes a 10240-byte
# file INTO THE ROOTFS.  Every later tar then finds a tape that opens and takes
# a different path (`tar -u' measured at exit 1 without it and exit 0 with it),
# which makes tar's results a function of iteration order.  A fresh cwd cannot
# contain a program that writes to an absolute path in the jail.
#
# Containing it properly costs a rootfs clone PER INVOCATION -- 53 x 0.146 s --
# which is not a price `make test' should pay, so tar moved to the probe's
# MUTATES set and is swept in CI under PROBE=mutating, where each invocation
# gets its own copy.  What guards tar's actual fix here is its own pair of
# targeted cases above, which is the part this suite owes.
#
# THIS COST TWO RED CI RUNS and both were my sweep, not the port: `tar -c' and
# `tar -r' crashed once each on a runner, and the create case saw an empty
# archive, all downstream of one leftover tape.
mkdir -p a0sweep
FLOORF=${FLOORF:-$ROOT/tests/crash-probe.floor}
# Strict entries for these three programs, label only, in the probe's own
# wording -- "<prog> (no arguments)" for the bare invocation.
a0want=$(grep -v '^[[:blank:]]*#' "$FLOORF" | grep -v '^[[:blank:]]*$' |
         grep -v '^?' | sed 's/^[0-9][0-9]* //' |
         grep -E '^(cb|diffh|cpio) ' | LC_ALL=C sort | paste -sd, - | sed 's/,/, /g')
# Tolerated ones are dropped from the observed side too.
a0tol=$(grep '^?' "$FLOORF" | sed 's/^?[[:blank:]]*[0-9][0-9]* //' |
        grep -E '^(cb|diffh|cpio) ' | LC_ALL=C sort)
check 'the only surviving crashes in the three are the ones the floor declares' \
    "$a0want" \
    "$(cd a0sweep && A0TOL=$a0tol perl -e '
	my @bad;
	for my $p (@ARGV) {
		my ($n) = $p =~ m{([^/]+)$};
		for my $o ("", map { "-$_" } ("a".."z", "A".."Z")) {
			my @cmd = ($p);
			push @cmd, $o if $o ne "";
			my $pid = fork();
			next if !defined $pid;
			if ($pid == 0) {
				alarm 5;
				open(STDIN,  "<", "/dev/null");
				open(STDOUT, ">", "/dev/null");
				open(STDERR, ">", "/dev/null");
				close(3);
				{ exec @cmd; }
				exit(127);
			}
			waitpid($pid, 0);
			my $s = $? & 127;
			next if $s == 0 || $s == 13 || $s == 14;
			# Same wording the probe uses, so this and the floor
			# file speak one language and the compare is literal.
			# (No apostrophes in here: the perl body is inside a
			# shell single-quoted string, and one apostrophe ends
			# it -- which cost a run to find.)
			push @bad, $n . ($o eq "" ? " (no arguments)" : " $o");
		}
	}
	my %tol = map { $_ => 1 } grep { length } split /\n/, $ENV{A0TOL} // "";
	print join(", ", sort grep { !$tol{$_} } @bad);
    ' "$(v8which cb)" "$DIFFH" "$(v8which cpio)")"

# AND THE WHOLE-TREE FLOOR IS IN CI NOW, which is the other half of the same
# lesson -- but a 13-minute job is a slow way to learn that its expectation
# file is malformed.  These three cases cost milliseconds and catch the ways
# tests/crash-probe.floor can be wrong without any program misbehaving.
FLOORF=$ROOT/tests/crash-probe.floor
check 'the crash-probe floor file exists' 'yes' \
    "$([ -f "$FLOORF" ] && echo yes || echo no)"
# Every entry is "<signal> <program> ...", optionally prefixed "? " for the
# tolerated/host-dependent kind.  A malformed line would be compared literally
# against the probe's output and could never match, so the failure would be a
# 13-minute CI run reporting a phantom regression.
check 'every floor entry is [?] <signal> <label>' '0' \
    "$(grep -v '^[[:blank:]]*#' "$FLOORF" 2>/dev/null | grep -v '^[[:blank:]]*$' |
       grep -cvE '^\??[[:blank:]]*[0-9]+ [a-zA-Z0-9_.-]+( .*)?$')"
# THE TOLERATED SET MUST STAY SMALL AND VISIBLE, because it is the one arm that
# can pass with a crash present.  Asserting the exact count -- rather than a
# ceiling -- means adding one is a deliberate edit here as well as there, which
# is the whole difference between an exemption and a hole.
#
# It is EMPTY, and it got there the right way: its only member, `tar -u', was
# put in when the crash looked host-dependent and taken out when the cause
# turned out to be containment in the test (tar writes its default tape into
# the rootfs).  An escape hatch with nothing in it is the correct steady state.
check 'the tolerated set is empty' '0' \
    "$(grep -c '^?' "$FLOORF" 2>/dev/null)"
# AND EVERY PROGRAM IT NAMES MUST STILL BE INSTALLED.  A floor naming a
# program that has been removed can never be satisfied -- the probe would
# report it "gone" forever -- and that is a stale-allow-list failure, the
# shape tests/kmemu's import list is kept honest against.
check 'every program named in the floor is installed' '' \
    "$(grep -v '^[[:blank:]]*#' "$FLOORF" 2>/dev/null | grep -v '^[[:blank:]]*$' |
       sed 's/^?[[:blank:]]*//' |
       awk '{print $2}' | sort -u | while read -r p; do
           find "$V8ROOT/bin" "$V8ROOT/usr/bin" "$V8ROOT/etc" "$V8ROOT/lib" \
                "$V8ROOT/usr/lib" -type f -name "$p" -perm -100 2>/dev/null |
               grep -q . || printf '%s ' "$p"
       done | sed 's/ *$//')"

# ---------------------------------------------------------------------------
# /dev/null, THROUGH THE INSTALLED BINARIES.  tests/v8sys asserts the mechanism
# against the shim directly; this asserts what a person at the prompt sees, and
# the distinction is one this tree has already paid for once -- tests/freestanding
# proved the shim imported nothing from libc and never the world built on it.
# "A guard on a seam is not a guard on what crosses it."
#
# The node is emptied first, deliberately.  Every suite here has to be a pure
# function of the tree, and this one would otherwise inherit whatever an earlier
# suite's failure left in the file -- which is precisely how the mutation run
# for these cases produced a false pass.  Whether anything ANYWHERE writes to
# /dev/null is a different question and has a better instrument: the crash
# probe, which runs every installed program against every option.
DEVNULL=$V8ROOT/dev/null
: > "$DEVNULL" 2>/dev/null
check 'the /dev/null node exists, so ls /dev is authentic' 'yes' \
    "$([ -f "$DEVNULL" ] && echo yes || echo no)"
check '...and ls lists it' 'null' \
    "$("$V8ROOT/bin/ls" /dev 2>/dev/null | grep '^null$')"
# `> /dev/null' is the commonest redirection in Unix, and it is what created
# the file in the first place: creat keys on the PARENT and $V8ROOT/dev exists.
"$V8ROOT/bin/sh" -c 'echo discarded-by-mem.c-line-156 > /dev/null' 2>/dev/null
"$V8ROOT/bin/sh" -c 'echo and-this-one-too >> /dev/null' 2>/dev/null
check 'a shell redirect to /dev/null writes nothing to the node' '0' \
    "$(wc -c < "$DEVNULL" | tr -d ' ')"
# The read is the sharper half: `prog < /dev/null' is how a program is given
# EMPTY input, and before the type it was given whatever last wrote.
check 'cat /dev/null is empty' '0' \
    "$("$V8ROOT/bin/cat" /dev/null 2>/dev/null | wc -c | tr -d ' ')"
# ...and the REDIRECT is a second path to the same place, worth its own case:
# `cat /dev/null' is the program opening its argument, `sh -c "cat < /dev/null"'
# is the SHELL opening the name and dup'ing it onto fd 0.  Note the redirect has
# to be inside the V8 shell -- written bare here it would be the host's shell
# opening the host's /dev/null, and the case would pass without the jail being
# involved at all.  (The first draft did exactly that, and named a `wc' in /bin
# that is in /usr/bin, which is the only reason it was noticed.)
check 'sh redirecting < /dev/null gives the program EOF' '0' \
    "$("$V8ROOT/bin/sh" -c 'cat < /dev/null' 2>/dev/null | wc -c | tr -d ' ')"
# proto-dev:25 is `crw-rw-rw- 1 root man 3, 2 ... null'.  The mode is what
# test(1) branches on and the numbers are what a masked passthrough loses --
# Darwin packs the major at bit 24 and V8 at bit 8, so an inherited stat
# reports major 0, which is `console'.
"$V8ROOT/bin/test" -c /dev/null; check 'test -c /dev/null is true'  '0' "$?"
"$V8ROOT/bin/test" -f /dev/null; check 'test -f /dev/null is false' '1' "$?"
check 'ls -l /dev/null reports V8 numbers' 'crw-rw-rw- 3, 2' \
    "$("$V8ROOT/bin/ls" -l /dev/null 2>/dev/null | awk '{print $1, $5, $6}')"

# ---------------------------------------------------------------------------
# libtermlib and ul -- the first library imported out of usr/src/lib, and the
# first program here that reads a DATABASE rather than only its arguments.
#
# WHICH UPSTREAM COPY THIS IS, asserted rather than described.  There are two
# termcap.c in the archive, differing by eleven lines, and the shipped
# usr/lib/libtermcap.a is the one built from lib/libtermlib -- provable because
# `ioctl' occurs in the whole library only inside those eleven lines, so an
# undefined _ioctl in the archive is a fingerprint of that source.  Our archive
# must carry it too, or we built the ex copy by mistake.
check 'libtermcap carries the jerq fingerprint (_ioctl)' 'yes' \
    "$(nm -u "$ROOT/build/stage0/termlib/libtermcap.a" 2>/dev/null |
       grep -qx '_ioctl' && echo yes || echo no)"

# The exported API, DERIVED from the archive rather than transcribed, so a
# member that stops being compiled shows up here instead of at the first
# program that wants it.
check 'libtermcap exports the termcap API' \
    '_tgetent _tgetflag _tgetnum _tgetstr _tgoto _tputs' \
    "$(nm -g "$ROOT/build/stage0/termlib/libtermcap.a" 2>/dev/null |
       awk '$2=="T"{print $3}' | grep -E '^_(tget|tgoto|tputs)' |
       sort -u | tr '\n' ' ' | sed 's/ $//')"

# A HARD LINK, because upstream's install is `ln libtermcap.a libtermlib.a'
# and the shipped tree has both names.  Compared by INODE: two copies of the
# same bytes would pass a cmp and would not be what V8 shipped.
check 'libtermlib.a is a hard link to libtermcap.a' 'same' \
    "$(a=$(ls -i "$V8ROOT/usr/lib/libtermcap.a" 2>/dev/null | awk '{print $1}'); \
       b=$(ls -i "$V8ROOT/usr/lib/libtermlib.a" 2>/dev/null | awk '{print $1}'); \
       [ -n "$a" ] && [ "$a" = "$b" ] && echo same || echo differs)"

# The database is DATA and must arrive unaltered -- it is the one file here
# that no compiler touches, so nothing else would notice if it did not.
check '/etc/termcap is installed byte-identical to the import' 'same' \
    "$(cmp -s "$ROOT/src/etc/termcap" "$V8ROOT/etc/termcap" && echo same || echo differs)"

# THE END-TO-END CASE, and its expected value is DERIVED FROM THE DATABASE.
# Transcribing \E[4m would make this a test of my typing; reading us= and ue=
# out of /etc/termcap makes it a test that ul found the vt100 entry, decoded
# the escapes, and handed them to tputs.
#
# The leading digits are a PADDING DELAY (`us=2\E[4m' is 2ms), which tputs
# strips and converts to pad characters -- and emits none here, because ul
# never sets ospeed and tputs returns early when it is 0.  So the expected
# output is the escape alone, which is also what makes the strip observable.
vt=$(awk '/^d1\|vt100\|/{p=1} p{print; if(!/\\$/) exit}' "$V8ROOT/etc/termcap")
tcap() { printf '%s' "$vt" | grep -oE ":$1=[^:]*" | head -1 |
         sed "s/^:$1=//; s/^[0-9]*//; s/\\\\E/$(printf '\033')/g"; }
printf 'a\b_b\b_ c\n' > ul.in
check 'ul emits the vt100 underline sequences /etc/termcap declares' \
    "$(printf '%sab%s c' "$(tcap us)" "$(tcap ue)")" \
    "$(TERM=vt100 "$(v8which ul)" ul.in)"

# The negative control, and it is what says the sequences came from the entry
# rather than from ul: `dumb' has no us/ue at all, so the same input must come
# out plain.  Without this, a ul that always emitted \E[4m would pass above.
check 'ul on a dumb terminal emits no escape' 'ab c' \
    "$(TERM=dumb "$(v8which ul)" ul.in)"

# ul -t WITH THE OPTION LAST -- the tenth member of this port's address-0
# class, and the same trigger as the other nine.  argv[1] is null, and
# tnamatch() dereferences it.
#
# ASSERTED ON THE OUTPUT AS WELL AS ON SURVIVAL, because a fix must not merely
# stop the crash: "" is the VAX's own answer (virtual 0 is crt0's first byte
# and it is 0x00, measured on the shipped binaries), so tgetent finds no
# terminal and ul takes its own `case 0' -- underlining stripped, text intact.
check 'ul -t with no argument does not die on a signal' 'lived' \
    "$(TERM=vt100 "$(v8which ul)" -t < ul.in >ul.o1 2>/dev/null; \
       s=$?; [ "$s" -lt 128 ] && echo lived || echo "signal $((s-128))")"
check 'and still passes the text through, unadorned' 'ab c' "$(cat ul.o1)"

# tgoto(3) -- REACHED BY NOTHING INSTALLED, so a probe is the only instrument.
#
# ul is libtermcap's only consumer and it never does cursor addressing, so the
# largest member of the library had no case looking at it.  Measured rather
# than suspected: dropping upstream's four -DCM_ flags took tgoto.o from 2376
# to 2096 bytes and every case above stayed green.  That is a STRONG mutation
# with no test aimed at it -- neither dead code nor a vacuous case, just no
# consumer -- which is the third reason this file records for a mutation not
# firing, and the one a probe fixes.
#
# It matters now rather than later because ex/vi is the next consumer and
# cursor addressing is the whole of a screen editor.
if "$CC" -c -o tgotoprobe.o "$ROOT/tests/wavea/tgotoprobe.c" >tgp.log 2>&1 &&
   clang -nostdlib -e _v8start -o tgotoprobe "$CRT" tgotoprobe.o \
       "$ROOT/build/stage0/termlib/libtermcap.a" \
       "$LIBC" "$STUBS" "$SHIM" -lSystem >>tgp.log 2>&1; then
	tgout=$(./tgotoprobe 2>&1)
	check 'tgoto interprets the unguarded %i%d;%d' 'ok' \
	    "$(printf '%s\n' "$tgout" | grep -c 'ok plain' | sed 's/^1$/ok/')"
	check 'tgoto: all five cursor-addressing forms' '0' \
	    "$(printf '%s\n' "$tgout" | sed -n 's/^tgotoprobe: \([0-9]*\) failed$/\1/p')"
	# Name the failures rather than only counting them: a bare count says
	# which flag is missing only by arithmetic.
	if printf '%s\n' "$tgout" | grep -q '^FAIL'; then
		printf '%s\n' "$tgout" | grep '^FAIL' | sed 's/^/  tgoto /'
	fi
else
	fail=$((fail+2)); echo "FAIL tgotoprobe (build)"; head -3 tgp.log
fi

# ---------------------------------------------------------------------------
# ex(1) -- which IS vi(1).  The largest program in this port, and the first
# here that edits rather than filters.
#
# NEVER RUN THESE THROUGH A BACKGROUNDING WRAPPER.  A deadline helper that
# does `"$@" &' gives the child /dev/null for stdin in a non-interactive
# shell, so ex reads EOF, executes nothing and exits 0 -- which is
# indistinguishable from a broken editor and was diagnosed as one for a whole
# session.  ex terminates on its own here: every case ends in `q'.
printf 'alpha\nbeta\ngamma\n' > ex.in
exrun() { # exrun <file> <commands...>
	f=$1; shift
	printf '%s\n' "$@" | TERM=vt100 "$(v8which ex)" "$f" 2>&1
}

# The status line, with the counts DERIVED from the file rather than typed.
# ex reports lines and characters, and wc knows both independently.
check 'ex reports the file it opened' \
    "\"ex.in\" $(wc -l < ex.in | tr -d ' ') lines, $(wc -c < ex.in | tr -d ' ') characters" \
    "$(exrun ex.in q | head -1)"

# THE BUFFER, and the carriage returns are ex's and not a defect.
#
# By default `p' output comes out `aa\n\r bb\n\r'.  That is ex's OPTIMIZE
# option: pstart() clears CRMOD on fd 1 -- `tty.sg_flags = normf &
# ~(ECHO|XTABS|CRMOD)' -- so the kernel stops mapping \n to \r\n and ex
# supplies the CR itself.  Nothing about it is machine-dependent, and it is
# reachable from the user side, which is what makes it testable rather than a
# sentence: `set nooptimize' takes the same early return pstart() takes when
# termcap says NONL, and the CRs vanish.  Both are asserted, so a change to
# either is a failure rather than a surprise.
check 'ex prints the buffer' 'alpha beta gamma' \
    "$(exrun ex.in '1,$p' q | tail -n +2 | tr -d '\r' | tr '\n' ' ' | sed 's/ $//')"
check 'and supplies its own CR, because optimize clears CRMOD' 'aa|bb|' \
    "$(printf 'aa\nbb\n' > ex.crm; exrun ex.crm '1,$p' q | tail -n +2 |
       tr '\n\r' '|@' | sed 's/@//g')"
check 'set nooptimize gives it back to the kernel' 'aa|bb|' \
    "$(exrun ex.crm 'set nooptimize' '1,$p' q | tail -n +2 | tr '\n\r' '|@')"

# THE EDIT ITSELF, asserted on the FILE and not on ex's exit status -- rm(1)
# taught this port that a 1985 tool's status is not an instrument.
cp ex.in ex.work
exrun ex.work '2s/beta/BETA/' w q > /dev/null
check 'ex substitutes and writes' 'alpha BETA gamma' \
    "$(tr '\n' ' ' < ex.work | sed 's/ $//')"
exrun ex.work '$a' 'delta' . w q > /dev/null
check 'ex appends' 'alpha BETA gamma delta' \
    "$(tr '\n' ' ' < ex.work | sed 's/ $//')"
exrun ex.work 1d w q > /dev/null
check 'ex deletes' 'BETA gamma delta' \
    "$(tr '\n' ' ' < ex.work | sed 's/ $//')"

# THE FOUR NAMES ARE ONE BINARY, which is what V8 shipped: usr/bin/{ex,vi,
# view,edit} are 116736 bytes each and there is NO usr/bin/e, so the makefile
# arm that mentions `e' was never the one that ran.  Compared by INODE,
# because four copies would pass a cmp and would not be a hard link.
#
# THAT SENTENCE IS TRUE AND WAS READ AS MORE THAN IT SAYS.  `e' IS shipped --
# in /bin, at 13312 bytes against ex's 116736 -- because it is ED's link, made
# by cmd/ed/Makefile, not ex's.  So the ex arm never ran AND `e' was a genuine
# missing name, and for several batches only the first half was written down.
# It is installed now and asserted below beside ed.
# TWO NUMBERS, NOT ONE, and the second is what makes the case non-vacuous.
# `ls -i a b c 2>/dev/null | sort -u | wc -l' answers 1 when b and c DO NOT
# EXIST, because a missing name is a missing ROW rather than an error -- so the
# obvious form passes against a tree where the links were never installed at
# all.  Measured: mutation B (dropping $(LINKED_INSTALL)) fired the behavioural
# cases and left every inode case green.  So assert the names SEEN beside the
# distinct inodes: `4 1' is four names sharing one file, `3 1' is a missing
# link, `4 4' is four copies.
check 'ex vi view edit are one inode' '4 1' \
    "$(cd "$V8ROOT/usr/bin" && ls -i ex vi view edit 2>/dev/null |
       awk '{n++; ino[$1]=1} END {d=0; for (k in ino) d++; print n+0, d}')"

# AND THE LINK IS LOAD-BEARING, not decoration: the binary reads argv[0] and
# becomes a screen editor when called vi.  That is also why this is the case
# to assert -- it proves the link reached the program, which an inode
# comparison alone does not.
check 'invoked as vi it demands a terminal' 'interactively' \
    "$(printf 'q\n' | TERM=vt100 "$V8ROOT/usr/bin/vi" ex.in 2>&1 |
       grep -o 'interactively' | head -1)"
check 'invoked as ex it does not' '' \
    "$(printf 'q\n' | TERM=vt100 "$(v8which ex)" ex.in 2>&1 |
       grep -o 'interactively' | head -1)"

# ---------------------------------------------------------------------------
# awk(1) -- and every case below is aimed at ONE of the four places a pointer
# was being carried in an `int'.  awk is the first program here whose value
# stack, whose regex table and whose scanner all hold addresses in objects
# declared as integers, because on a VAX that was the same thing.
#
#   awk.lx.l:10   `extern int yylval' against yacc's `#define YYSTYPE long'
#   awk.lx.l x7   `(int) lookup/fieldadr/setsymtab/tostring' into it
#   b.c:38        `int rlxval', which holds `(long) tostring(cbuf)' for a CCL
#   awk.h:165     `int lval' in struct rrow, read back as `(char *) lval'
#
# See src/cmd/awk/PORTING.md.  A truncated pointer in any of them is a wild
# address, so the failure mode is a crash or a nonsense answer rather than a
# subtly wrong one -- which is why these read like ordinary awk usage.
awkbin=$(v8which awk)
printf 'alpha 3 x\nbeta 5 y\ngamma 7 z\n' > awk.in

# THE VALUE STACK: every one of these goes through yylval as a pointer.
# `$1' is fieldadr(), a bare name is setsymtab(), a string constant is
# setsymtab() over tostring(), and `$0' is lookup().
check 'awk prints a field'        'alpha beta gamma' \
    "$("$awkbin" '{print $1}' awk.in | tr '\n' ' ' | sed 's/ $//')"
check 'awk prints the whole record' 'alpha 3 x' \
    "$("$awkbin" 'NR==1 {print $0}' awk.in)"
check 'awk prints a string constant' 'row: alpha' \
    "$("$awkbin" 'NR==1 {print "row:", $1}' awk.in)"

# THE REGEX TABLE.  A character class is the ONLY path that puts a pointer in
# struct rrow's lval: b.c stores `(long) right(v)' there and reads it back as
# `(char *)' for member().  A plain /beta/ is CHAR nodes and would pass with
# the field four bytes wide, so the class is the case that discriminates.
check 'awk matches a character class'  'beta gamma' \
    "$("$awkbin" '/[bg]/ {print $1}' awk.in | tr '\n' ' ' | sed 's/ $//')"
check 'awk matches a NEGATED class'    'alpha' \
    "$("$awkbin" '$1 ~ /^[^bg]/ {print $1}' awk.in | tr '\n' ' ' | sed 's/ $//')"
check 'and a plain string, which is the control' 'beta' \
    "$("$awkbin" '/beta/ {print $1}' awk.in)"

# NUMBERS.  awk prints an integral value with "%.20g" (tran.c:271), so this is
# also the end-to-end case for the %g trailing-zero fix in doprnt.c -- before
# it, `3 15' came out `3.0000000000000000000 15.000000000000000000'.
check 'awk sums a column'         '3 15' \
    "$("$awkbin" '{s += $2} END {print NR, s}' awk.in)"
check 'awk formats a non-integer with OFMT' '2.5' \
    "$(echo | "$awkbin" '{print 5/2}')"
check 'awk reaches the math library' '1.41421' \
    "$(echo | "$awkbin" '{print sqrt(2)}')"

# ARRAYS, the built-ins, and -F.  The array subscript is a string built from
# the symbol table, so it is the value stack again one level down.
check 'awk associates an array' 'keys 3' \
    "$("$awkbin" '{a[$1] = $2} END {n = 0; for (k in a) n++; print "keys", n}' awk.in)"
check 'awk substr and index'    'al 1 5' \
    "$("$awkbin" 'NR==1 {print substr($1,1,2), index($1,"a"), length($1)}' awk.in)"
check 'awk -F takes a separator' 'b' \
    "$(printf 'a:b:c\n' | "$awkbin" -F: '{print $2}')"
check 'awk printf'              'alpha=3' \
    "$(printf 'alpha 3\n' | "$awkbin" '{printf "%s=%d\n", $1, $2}')"

# THE EMPTY MAIN RULE, which is the case that matters most and reads like the
# least.  `BEGIN{...}' has NO main pattern-action statement, so awk.g.y:177's
# empty `pa_stats' puts a null in narg[1] of the PROGRAM node and program()
# executes it once per input record.  real_execute() guards a null; the
# `execute' MACRO reads (p)->ntype first, which a VAX could do and this machine
# cannot -- so `BEGIN{print 1}' was a SIGSEGV.
#
# EVERY ONE OF THESE MUST HAVE INPUT.  With empty stdin the record loop never
# runs, the null is never executed, and all four pass against the broken macro.
# That is what hid it for the first hour: `awk 'BEGIN{print 1}' < /dev/null'
# exits 0 either way.
#
# THE TWO EMPTY-OUTPUT CASES CARRY THEIR EXIT STATUS, and mutation is what
# said they had to.  Restoring the broken macro fired the BEGIN and END cases
# and left `{}' and `'' green -- because a program that SIGSEGVs produces no
# output either, so `check <name> '' "$(...)"' cannot tell a crash from a
# correct silent run.  A case whose expected output is empty is vacuous
# against a crash unless it also asserts the status.
awkq() { # awkq <program>: "<status>|<output>"
	printf 'a\nb\n' | "$awkbin" "$1" > awkq.out 2>&1
	printf '%s|%s' "$?" "$(cat awkq.out)"
}
check 'awk runs a BEGIN-only program'   '0|start' "$(awkq 'BEGIN{print "start"}')"
check 'awk runs an END-only program'    '0|2'     "$(awkq 'END{print NR}')"
check 'awk runs an empty action'        '0|'      "$(awkq '{}')"
check 'awk runs an empty program'       '0|'      "$(awkq '')"
check 'and all three parts together'    'start 3 5 7 n=3' \
    "$("$awkbin" 'BEGIN{print "start"} {print $2} END{print "n="NR}' awk.in |
       tr '\n' ' ' | sed 's/ $//')"

# AN OPTION IN THE LAST POSITION, where argv[1] is the argv terminator.  63 of
# the 64 single-letter options reached it, because the loop consumes an unknown
# letter and falls through to `lexprog = argv[1]'.  A VAX read the empty string
# at address 0, so the answer to restore is the EMPTY PROGRAM -- exit 0 and no
# output -- rather than a diagnostic this port invented.
check 'an unknown trailing option is the empty program' '0|' \
    "$(printf 'a\nb\n' | "$awkbin" -a > awkopt.out 2>&1; printf '%s|%s' "$?" "$(cat awkopt.out)")"
check 'and so is a trailing --' '0|' \
    "$(printf 'a\nb\n' | "$awkbin" -- > awkopt.out 2>&1; printf '%s|%s' "$?" "$(cat awkopt.out)")"
# -f with no name is fopen("") -- which V7's namei and this shim both make the
# CURRENT DIRECTORY, so awk parses the raw bytes of a directory as its program.
#
# ONLY THE ABSENCE OF A SIGNAL IS ASSERTABLE, and the first draft of this case
# asserted the exit STATUS and went red on its second run.  What awk makes of
# those bytes is a function of what happens to be in the suite's temp
# directory, which the cases above change as they go -- so it exited 2 with a
# syntax error one run and 0 the next, from the same binary.  That is a
# property of the directory rather than of the port, which is this suite's
# most-swept-for defect arriving in a case written the same hour; and it is
# also V8's own behaviour, since a VAX read the same directory the same way.
check 'a trailing -f does not die on a signal' 'ok' \
    "$(printf 'a\n' | "$awkbin" -f >/dev/null 2>&1; s=$?
       [ "$s" -lt 128 ] && echo ok || echo "signal $((s - 128))")"
check 'and -f with a real program file works' 'x y z' \
    "$(printf '{print $3}\n' > awk.prog; "$awkbin" -f awk.prog awk.in |
       tr '\n' ' ' | sed 's/ $//')"

# WHERE IT LANDED, and it is upstream's decision: the shipped tree has
# usr/bin/awk and no bin/awk, and awk's own makefile says `cp a.out
# /usr/bin/awk'.  Nothing in the Makefile chose it -- $(call v8dest,awk) falls
# through to usr/bin because awk is in none of Admin's three tables.
check 'awk installs in /usr/bin' "$V8ROOT/usr/bin/awk" "$awkbin"

# MAKETAB IS A BUILD TOOL AND MUST NOT BE IN THE WORLD.  It compiles and links
# exactly like a command here -- it is a V8 binary built by v8cc and run by the
# build -- so the only thing keeping it out of $(ROOTFS) is that no rule puts
# it there.  tests/deps cannot assert this: touching maketab.c legitimately
# makes awk stale, because maketab writes proctab.c.  "Is it a prerequisite"
# and "is it a component" are different questions and this is the second.
check 'maketab is not installed' '' \
    "$(v8which maketab 2>/dev/null)"
check 'but the build did make and run it' '1' \
    "$([ -x "$ROOT/build/stage0/awk/maketab" ] && [ -s "$ROOT/build/stage0/awk/proctab.c" ] &&
       echo 1 || echo 0)"

# ---------------------------------------------------------------------------
# Wave A2 batch 2b -- the five single-file commands still in scope.  Batch 1
# was recorded as having exhausted that set and had not: re-measured, 36 of the
# 156 missing programs still have a bare .c in cmd/.  Most are the toolchain
# exception (ar ld nm ranlib size) or act on the host (halt reboot init mount
# ...); these five are the remainder.
#
# uuencode / uudecode: a ROUND TRIP, because either half alone can be
# self-consistently wrong -- the encoding is six bits per character and a table
# error that both share is invisible until something else decodes it.  The
# decoded name comes from uuencode's `begin' line, not from the shell.
uuin=uu.in; printf 'Hello, Research Unix V8.\n' > $uuin
rm -f uudec.out
"$(v8which uuencode)" "$uuin" uudec.out > uu.enc 2>&1
check 'uuencode writes a begin line' 'begin 644 uudec.out' "$(head -1 uu.enc)"
"$(v8which uudecode)" uu.enc >/dev/null 2>&1
check 'and uudecode reproduces the bytes' 'same' \
    "$(cmp -s "$uuin" uudec.out && echo same || echo differs)"

# spline: 100 interpolated points from 4, which is upstream's default -- and it
# is the FP path, so a broken double return or a wrong register class shows up
# as zeros rather than as a crash.  The endpoints are asserted because
# interpolation must pass through the data it was given.
printf '0 0\n1 1\n2 4\n3 9\n' > sp.in
"$(v8which spline)" < sp.in > sp.out 2>&1
check 'spline interpolates 100 points' '100' "$(wc -l < sp.out | tr -d ' ')"
check 'and passes through an endpoint'  '3.000000 9.000000' "$(head -1 sp.out)"

# mc: columnation, and the case that matters is `-20' -- an option in the LAST
# position, which is where mc.c:49's `while(*argv[1]=='-')' walked onto the
# argv terminator.  Measured before the guard: SIGSEGV after reading the width
# and before reading a byte of input.  A VAX read 0x00 at address 0, so the
# loop ended and mc read stdin, which is what this asserts.
mcin='alpha
beta
gamma
delta
epsilon
zeta'
check 'mc columnates to a width'   'alpha	delta' \
    "$(printf '%s\n' "$mcin" | "$(v8which mc)" -20 | head -1)"
check 'and a trailing option does not crash it' '0' \
    "$(printf '%s\n' "$mcin" | "$(v8which mc)" -20 >/dev/null 2>&1; echo $?)"
check 'bare mc reads stdin'        'alpha beta  gamma' \
    "$(printf 'alpha\nbeta\ngamma\n' | "$(v8which mc)")"

# stty: it opens /dev/tty, which in this world is /dev/fd/3 -- so with no fd 3
# the honest answer is that it cannot open it, and that is what a suite gets.
# The case is here rather than absent because stty was recorded as BLOCKED on
# tty_ld/ntty_ld being "genuinely kernel state", which ex/vi disproved: they
# are 24 initialised ints in libc/gen/linedis.c.  Both are `D' symbols in
# libv8c today, so the link is what proves the unblocking.
check 'stty says it cannot open the terminal' 'yes' \
    "$("$(v8which stty)" 2>&1 | grep -c "can't open /dev/tty" | tr -d ' ' |
       sed 's/^1$/yes/')"
check 'and it links tty_ld out of libv8c' '2' \
    "$(nm -g "$ROOT/rootfs/lib/libv8c.a" 2>/dev/null |
       grep -cE '_(n)?tty_ld$' | tr -d ' ')"

# WHERE THE FIVE LANDED, and only stty is not /usr/bin.  Derived from Bell
# Labs' tables by $(call v8dest,...), never chosen here -- stty is in
# Admin/binfiles because V8's /bin is the 56-entry root-filesystem set.
check 'the batch installed where V8 put it' \
    'usr/bin usr/bin usr/bin bin usr/bin' \
    "$(for n in spline uuencode uudecode stty mc; do
         for d in bin usr/bin etc; do
           [ -x "$V8ROOT/$d/$n" ] && { printf '%s ' "$d"; break; }
         done
       done | sed 's/ $//')"

# ---------------------------------------------------------------------------
# Wave A2 batch 2c -- four DIRECTORY programs, each chosen for a build idiom
# rather than for its size, because those idioms are what the other fifty-one
# will need.

# expr: a grammar and nothing else.  Four cases for four halves of the
# language, because expr is really four little interpreters sharing a parser --
# integer arithmetic, string matching, comparison, and the `:' operator's
# return of a MATCH LENGTH rather than a boolean.
exprbin=$(v8which expr)
check 'expr adds'            '7'  "$("$exprbin" 3 + 4)"
check 'expr multiplies'      '70' "$("$exprbin" 10 '*' 7)"
check 'expr compares'        '1'  "$("$exprbin" 2 '<' 3)"
check 'expr : yields a length' '3' "$("$exprbin" abcdef : abc)"

# egrep: expr's shape one step smaller and the FIRST BARE *.y imported -- a
# single file in cmd/ with no directory and no makefile, so Admin/Mk's `*.y'
# arm is its build description and Admin/dest answers /usr/bin by
# FALL-THROUGH, egrep being in none of the four tables.  Asserted as the
# DIRECTORY rather than through v8which, because the fall-through is the claim.
check 'egrep installs to /usr/bin' 'yes' \
    "$([ -x "$V8ROOT/usr/bin/egrep" ] && echo yes)"
egrepbin=$V8ROOT/usr/bin/egrep
check 'egrep matches an alternation' 'alpha gamma' \
    "$(printf 'alpha\nbeta\ngamma\n' | "$egrepbin" 'a(l|m)' | tr '\n' ' ' | sed 's/ $//')"
check 'egrep -v inverts'             'beta' \
    "$(printf 'alpha\nbeta\n' | "$egrepbin" -v 'a(l|m)')"
# The status contract is in egrep.y's own header: 0 matched, 1 ran but did not,
# 2 error.  The middle one is the half a match-only case cannot see, and it is
# what `if egrep ...' in a shell script depends on.
check 'egrep exits 1 having matched nothing' '1' \
    "$(printf 'alpha\n' | "$egrepbin" zzz >/dev/null 2>&1; echo $?)"
# AND THE PROPERTY THAT MADE IT WORTH PORTING: a newline inside a -f pattern is
# an ALTERNATION IN THE GRAMMAR, not a pattern separator, so parentheses may
# span it.  egrep.y:26 defines RIGHT as the newline and :157 is `case RIGHT:
# return (OR);'.  That is what calendar2 emits and what POSIX grep -f refuses.
# Two halves, because an egrep that matched every line would pass the first.
check 'egrep reads a newline in a -f pattern as an alternation' 'yes no' \
    "$(printf '(one\ntwo)\n' > eg.pat
       a=$(printf 'xxtwoxx\n' | "$egrepbin" -f eg.pat >/dev/null 2>&1 && echo yes || echo no)
       b=$(printf 'xxthreexx\n' | "$egrepbin" -f eg.pat >/dev/null 2>&1 && echo yes || echo no)
       echo "$a $b")"

# bc: egrep's shape again -- a bare *.y, /usr/bin by fall-through -- and the
# FIRST PROGRAM HERE WHOSE CORRECTNESS DEPENDS ON A SECOND V8 BINARY.  bc does
# not evaluate: it compiles to dc(1)'s language, forks, and execs the
# interpreter down a pipe.  So a passing case means bc, dc, fork, pipe and the
# jail's exec fall-through all agree -- bc tries /bin/dc first, which this
# world does not have, and /usr/bin/dc second.
bcbin=$(v8which bc)
bcrun() { _in=$1; shift
          printf '%b' "$_in" | perl -e 'alarm 20; exec @ARGV' "$bcbin" "$@" 2>&1 | tr -d '\n'; }
check 'bc installs to /usr/bin' 'yes' "$([ -x "$V8ROOT/usr/bin/bc" ] && echo yes)"
check 'bc adds'                  '5'  "$(bcrun '2+3\n')"
check 'bc keeps scale'      '3.3333'  "$(bcrun 'scale=4\n10/3\n')"
check 'bc has variables'        '42'  "$(bcrun 'x=7\nx*6\n')"
# THE BIGNUM CASE IS THE ONE THAT PROVES dc IS DOING THE WORK: 2^64 does not fit
# in any integer either program has, so a right answer here is arbitrary
# precision arriving through the pipe.
check 'bc is arbitrary precision' '18446744073709551616' "$(bcrun '2^64\n')"
check 'bc changes output base'   'FF'  "$(bcrun 'obase=16\n255\n')"
# A FUNCTION, WHICH IS WHAT bundle() EXISTS FOR.  The whole port of bc is one
# type widening in that arena -- every cell holds a pointer and a VAX pointer
# was four bytes -- and before it bc emitted the EMPTY dc program for every
# input, exit 0.  A body on its own lines is V7's dialect; the one-line form is
# GNU's extension and is not a defect here.
check 'bc defines and calls a function' '81' \
    "$(bcrun 'define f(n) {\n\treturn(n*n)\n}\nf(9)\n')"
check 'bc runs a for loop'      '123'  "$(bcrun 'for(i=1;i<=3;i++) i\n')"
# ...and the math library, which is DATA: bc -l parses /usr/lib/lib.b as bc
# source, so this asserts the install as well as the arithmetic.  a() is used
# rather than e() or s() deliberately -- see src/cmd/bc.PORTING.md and task #25,
# where dc loses any number with an odd count of fraction digits, which bounds
# which of lib.b's functions can be asked.  lib.b opens `scale = 20', even.
check 'bc -l loads the math library' '3.14159265358979323844' \
    "$(bcrun 'a(1)*4\n' -l)"

# m4: define and expand, then a macro with an argument, which is the only case
# that proves the argument stack rather than the symbol table.
m4bin=$(v8which m4)
check 'm4 expands a definition' 'Hello, world.' \
    "$(printf 'define(NAME, world)Hello, NAME.\n' | "$m4bin")"
check 'm4 substitutes an argument' '7 * 7' \
    "$(printf 'define(sq,`$1 * $1'\'')sq(7)\n' | "$m4bin")"

# diff3: THE WHOLE WORLD IN ONE INVOCATION, and that is why it is worth a case.
# /usr/bin/diff3 is a shell SCRIPT with no `#!' line; V8's sh runs it, it calls
# V8's diff twice, and it execs /usr/lib/diff3 by absolute path -- so a pass
# here means sh, diff, the jail's /usr/lib and the helper all agree.  Upstream's
# own install is `mv diff3 /usr/lib; cp diff3.sh /usr/bin/diff3'.
printf 'a\nb\nc\n' > d3.1; printf 'a\nB\nc\n' > d3.2; printf 'a\nb\nC\n' > d3.3
check 'diff3 runs script, sh, diff and the helper' '====|1:2,3c|2:2,3c|3:2,3c' \
    "$("$V8ROOT/bin/sh" "$V8ROOT/usr/bin/diff3" d3.1 d3.2 d3.3 2>&1 |
       grep -E '^(====|[0-9]:)' | tr '\n' '|' | sed 's/|$//')"
check 'and the helper is in /usr/lib, not /usr/bin' 'lib script' \
    "$([ -x "$V8ROOT/usr/lib/diff3" ] && printf 'lib '
       head -1 "$V8ROOT/usr/bin/diff3" | grep -q '^e=' && printf 'script')"
# A BARE diff3 was main's first statement dereferencing argv[1] -- diffh's
# shape, ninth instance.  A VAX read 0x00 there, so the test failed and the
# argc check below it spoke; that message is the answer, not the absence of
# the fault.
check 'bare diff3 reports rather than crashes' '1|diff3: arg count' \
    "$("$V8ROOT/usr/lib/diff3" </dev/null >d3.out 2>&1; printf '%s|%s' "$?" "$(cat d3.out)")"

# pack/unpack/pcat: a ROUND TRIP, for uuencode's reason -- a compressor and its
# decompressor share a tree and can be self-consistently wrong.  The input has
# to be genuinely redundant or pack refuses it ("not packed because of no
# savings"), so it is generated rather than reused.
i=0; while [ $i -lt 2000 ]; do echo "the quick brown fox jumps over the lazy dog $((i % 7))"; i=$((i+1)); done > pk.txt
cp pk.txt pk.orig
check 'pack compresses'   'packed' \
    "$("$(v8which pack)" pk.txt >/dev/null 2>&1; [ -f pk.txt.z ] && echo packed || echo no)"
check 'and it is smaller' 'smaller' \
    "$([ "$(wc -c < pk.txt.z)" -lt "$(wc -c < pk.orig)" ] && echo smaller || echo no)"
"$(v8which unpack)" pk.txt.z >/dev/null 2>&1
check 'unpack round-trips exactly' 'same' \
    "$(cmp -s pk.orig pk.txt && echo same || echo differs)"
"$(v8which pack)" pk.txt >/dev/null 2>&1
check 'pcat writes the original to stdout' 'same' \
    "$("$(v8which pcat)" pk.txt.z 2>/dev/null | cmp -s - pk.orig && echo same || echo differs)"
# pcat IS unpack -- upstream's install is `ln /usr/bin/unpack /usr/bin/pcat',
# and the shipped tree's two copies are byte-identical at 10240 with the link
# lost on extraction.  Compared by INODE, because two identical copies pass a
# cmp and are not what V8 shipped.  Same treatment as ex/vi/view/edit.
check 'pcat and unpack are one inode' '2 1' \
    "$(cd "$V8ROOT/usr/bin" && ls -i pcat unpack 2>/dev/null |
       awk '{n++; ino[$1]=1} END {d=0; for (k in ino) d++; print n+0, d}')"

# THE OTHER THREE LINKS BELL LABS' INSTALL ARMS MAKE, and the two families need
# DIFFERENT cases because only one of them reads argv[0].
#
# compress/uncompress/zcat is the ex/vi shape: compress.c:385-394 strips the
# directory from argv[0] and compares it against "uncompress" and "zcat", so
# the link is LOAD-BEARING and an inode comparison alone would not prove it
# reached the program -- hence a behavioural case per name.  ed/e is the
# opposite: ed reads argv[0] nowhere, so `e' is a pure alias and the inode IS
# the behaviour, with the run case proving only that the link is executable.
i=0; while [ $i -lt 400 ]; do echo "compressible line $((i % 5))"; i=$((i+1)); done > cz.txt
cp cz.txt cz.orig
"$(v8which compress)" cz.txt >/dev/null 2>&1
check 'compress produces a .Z' 'yes' \
    "$([ -f cz.txt.Z ] && echo yes || echo no)"
check 'and it is smaller' 'smaller' \
    "$([ -f cz.txt.Z ] && [ "$(wc -c < cz.txt.Z)" -lt "$(wc -c < cz.orig)" ] \
       && echo smaller || echo no)"
# zcat is the SAME BINARY deciding from its name to write on stdout and leave
# the file alone -- so this asserts the argv[0] arm, not merely that a file
# decompresses.  The .Z must still exist afterwards.
check 'zcat writes the original to stdout' 'same' \
    "$("$(v8which zcat)" cz.txt.Z 2>/dev/null | cmp -s - cz.orig && echo same || echo differs)"
check 'and zcat left the .Z in place' 'yes' \
    "$([ -f cz.txt.Z ] && echo yes || echo no)"
check 'uncompress round-trips exactly' 'same' \
    "$("$(v8which uncompress)" cz.txt.Z >/dev/null 2>&1; cmp -s cz.txt cz.orig && echo same || echo differs)"
check 'compress uncompress zcat are one inode' '3 1' \
    "$(cd "$V8ROOT/usr/bin" && ls -i compress uncompress zcat 2>/dev/null |
       awk '{n++; ino[$1]=1} END {d=0; for (k in ino) d++; print n+0, d}')"
# ed/e -- upstream's install is `rm -f /bin/e; ln /bin/ed /bin/e', and the
# shipped bin/e and bin/ed are byte-identical at 13312 with the link lost on
# extraction.  Same INODE treatment as pcat, and it is the whole of the claim
# here because ed does not branch on its name.
check 'e and ed are one inode' '2 1' \
    "$(cd "$V8ROOT/bin" && ls -i e ed 2>/dev/null |
       awk '{n++; ino[$1]=1} END {d=0; for (k in ino) d++; print n+0, d}')"
printf 'a\nhello\n.\nw ed.out\nq\n' | "$(v8which e)" >/dev/null 2>&1
check 'e edits, so the link is executable' 'hello' \
    "$(cat ed.out 2>/dev/null)"

# ---------------------------------------------------------------------------
# THE FIVE COMMANDS V8 SHIPPED AS SHELL SCRIPTS, and they are the first thing
# installed here whose CORRECTNESS IS A PROPERTY OF $PATH.  Every command
# before them is a self-contained binary whose behaviour is fixed at link time;
# a script is right or wrong depending on which other commands it finds, so
# these run under the PATH v8(1) gives the world rather than the one the tester
# happens to have.
#
# THAT IS NOT A PRECAUTION, IT IS A MEASURED FAULT.  Run with a developer's
# host PATH, `whois root' resolves grep to a homebrew binary that the rootfs
# does not have -- so the jail lets it through by construction, it is unjailed,
# and it greps the MAC's /etc/passwd and prints a completely plausible wrong
# answer (System Administrator:/var/root instead of Superuser:/).  V8JAIL=warn
# reports no escape, correctly, because nothing escaped: a host binary was
# asked and host binaries see the host.
sh8=$(v8which sh)
jail_sh() { PATH=/bin:/usr/bin:/etc "$sh8" -c "$1" 2>/dev/null; }

# INSTALLED AT BELL LABS' OWN DESTINATION, and this case exists because the
# behavioural ones below CANNOT SEE whether the port installed anything at all.
# macOS has every one of these five names -- true, false, dirname, nohup and
# whois are all in the host's /usr/bin -- and /bin and /usr/bin are UNION
# mounts, so a name the rootfs lacks falls through to the host's copy, which
# for four of the five behaves identically.  Measured: dropping
# $(SCRIPT_INSTALL) left `true exits 0', `false exits 1', all four dirname
# cases and both nohup cases GREEN, because the Mac answered them.
#
# So there are two properties and they need two cases: that the PORT INSTALLED
# it (here, at the directory $(call v8dest,...) derives from Admin/binfiles),
# and that it WORKS under the jail's PATH (below).  Neither implies the other.
check 'the five scripts are installed where V8 put them' \
    'bin/true bin/false bin/nohup usr/bin/dirname usr/bin/whois' \
    "$(for n in true false nohup dirname whois; do
         for d in bin usr/bin etc; do
           [ -f "$V8ROOT/$d/$n" ] && { printf '%s/%s ' "$d" "$n"; break; }
         done
       done | sed 's/ $//')"

# true is upstream's ZERO-BYTE file, not a program that exits 0, and the size
# is asserted because "it exits 0" is also true of a broken copy -- and of the
# HOST's true, which is what makes the plain exit-status case vacuous alone.
check 'true is the empty file'  '0' "$(wc -c < "$V8ROOT/bin/true" | tr -d ' ')"
jail_sh 'true'  >/dev/null 2>&1; check 'true exits 0'  '0' "$?"
jail_sh 'false' >/dev/null 2>&1; check 'false exits 1' '1' "$?"

# dirname is an `expr' one-liner, so these also prove expr's regex arms work.
# The two that catch a naive implementation are the last two: a bare name is
# `.' and the root is `/', neither of which is "everything before the last /".
check 'dirname of a path'      '/usr/bin' "$(jail_sh 'dirname /usr/bin/cat')"
check 'dirname is not recursive' '/a/b'   "$(jail_sh 'dirname /a/b/c')"
check 'dirname of a bare name' '.'        "$(jail_sh 'dirname hello')"
check 'dirname of the root'    '/'        "$(jail_sh 'dirname /')"

# nohup redirects ONLY when the stream is a terminal -- `[ -t 1 ]' -- so with
# stdout captured the right answer is that the output arrives on stdout and no
# nohup.out is created at all.  Asserting the file's ABSENCE is the case that
# would catch a nohup which redirects unconditionally.
rm -f nohup.out
check 'nohup passes output through when stdout is not a tty' 'hi' \
    "$(jail_sh 'nohup echo hi' | tail -1)"
check 'and it wrote no nohup.out' 'absent' \
    "$([ -f nohup.out ] && echo present || echo absent)"

# whois IS THE CONTAINMENT CASE, and it is asserted against a value DERIVED
# from the jail's own passwd rather than a transcribed string -- the host's
# root line is a property of whoever's Mac this is, and the jail's is a
# property of the port.  Reading the host's file fails this.
check 'whois reads the jail passwd, not the host' \
    "$(grep '^root' "$V8ROOT/etc/passwd" 2>/dev/null | head -1)" \
    "$(jail_sh 'whois root' | head -1)"
# /usr/adm/usrlist is runtime state V8 never shipped, so both greps for it miss
# and an unknown name falls all the way through -- upstream's own last arm.
check 'whois falls through for an unknown name' 'who indeed is nosuchguy' \
    "$(jail_sh 'whois nosuchguy' | tail -1)"

# ---------------------------------------------------------------------------
# Wave A2 batch 2d -- seven programs and a library.  The batch was scoped as
# ten; graph, plot and prof turned out to share one blocker (usr/src/libplot,
# task #89) and are not here.

# hoc: an INTERPRETER, so the cases walk up from arithmetic to a user-defined
# function.  sqrt() is the one that matters most and it is not about hoc: it
# reaches Symbol.u.ptr, a `double (*)()' called through a function pointer, so
# a pass means V8's own math is being called with the argument in the register
# V8's compiler puts it in.  That is the pair of defects task #28 fixed --
# `float atof()' read s0 where a double comes back in d0, and v8cc passing
# doubles in x0-x7 against AAPCS64's d0-d7 -- and nothing since has exercised
# them through an indirect call.
hocbin=$(v8which hoc)
hoc() { printf '%s\n' "$1" | "$hocbin" 2>&1 | tr -d '\t' | tr '\n' ' ' | sed 's/ $//'; }
check 'hoc adds'                 '3'         "$(hoc '1+2')"
check 'hoc has variables'        '9'         "$(hoc 'x = 3
x*x')"
check 'hoc calls the math library' '1.4142136' "$(hoc 'sqrt(2)')"
check 'hoc defines a function'   '49'        "$(hoc 'func sq() { return $1*$1 }
sq(7)')"
check 'hoc loops'                '1 2 3'     "$(printf 'for (i=1; i<4; i=i+1) print i\n' | "$hocbin" 2>&1 | sed 's/ *$//')"

# ...AND THEN KERNIGHAN AND PIKE'S OWN TESTS, which were in the tree the whole
# time and had never been run.  hoc/tests.a is an `ar' bundle of ten programs
# that came in with the import and was DELETED BY .gitignore -- `*.a', a rule
# for build outputs, matching a source bundle.  Three sibling suites went the
# same way: eqn 37 cases, tbl 54, pic 36.  See the note in .gitignore.
#
# These are worth more than the cases above them because they are ADVERSARIAL
# in a way a port author's own cases are not: ack is Ackermann's function,
# which is 2432 recursive calls deep for A(3,3) and exercises the interpreter's
# argument stack far harder than anything written here would.
mkdir -p hoct && ( cd hoct && ar x "$ROOT/src/cmd/hoc/tests.a" 2>/dev/null )
hoct() { perl -e 'alarm 30; exec @ARGV' "$hocbin" 2>&1 < "hoct/$1"; }
check 'hoc: their ack (Ackermann 3,3)'  '61|2432 calls' \
    "$(hoct ack | tr -d '\t' | tr '\n' '|' | sed 's/|$//')"
check 'hoc: their fac1 (0!, 7!, 10!)'   '1|5040|3628800' \
    "$(hoct fac1 | tr -d '\t' | tr '\n' '|' | sed 's/|$//')"
check 'hoc: their fac2 prints a table'  'factorial of 6 is 720' \
    "$(hoct fac2 | grep '^factorial of 6 ' | sed 's/ *$//')"
check 'hoc: their fib2 reaches 14930352' '14930352' \
    "$(hoct fib2 | tr ' ' '\n' | grep -x 14930352)"
check 'hoc: their double doubles'        '1024' \
    "$(hoct double | tr ' ' '\n' | grep -x 1024)"
# fac and fib define functions and print nothing, which is why fac1/fib2 exist
# to call them; asserting the SILENCE keeps a future change that makes them
# chatter visible.
check 'hoc: their fac is definitions only' '0' "$(hoct fac | wc -c | tr -d ' ')"
# ack1 IS NOT RUN, and the reason is upstream's design rather than a gap.  It is
# `while (read(x)) { read(y); print ack(x,y) }', and varread() at code.c:564 is
# `fscanf(fin, "%lf", ...)' -- fin is the PROGRAM stream, not stdin.  So read()
# competes with the parser for the same bytes, and stdio's block buffering means
# the parser has already consumed the data by the time read() runs.  Measured
# three ways -- data piped after the program, data in a second file argument,
# program as a file with data on stdin -- and all three give "non-number read
# into x near line 12", where 12 is the program's LAST line, which is the tell.
# It needs a terminal, like ex's visual mode.

# p: the pager, and the case that earns its keep is the MISSPELLING, because
# that is the only path reaching spname() -- the third copy of that function in
# the tree, and the one whose newname[] had no bound at all.  See
# src/cmd/p/PORTING.md: raising DIRSIZ 14 -> 254 made sh's copy fail LOUDLY (a
# guard that went negative) and this one fail SILENTLY (no guard to go
# negative), which is why it survived.
#
# fd 3 is /dev/tty here -- V8's /dev/tty is a hard link to /dev/fd/3 -- and it
# must ANSWER, not merely be open: spopen()'s `while(c != '\n') c = getc(tty);'
# spins forever at EOF.  That loop is upstream's and stays (there is no VAX
# answer to restore: a V8 /dev/tty at EOF did the same), so the test supplies a
# newline rather than /dev/null.
pbin=$(v8which p)
printf 'one\ntwo\nthree\n' > p.txt
printf '\n' > p.tty
check 'p without /dev/tty declines' 'p: no /dev/tty' \
    "$("$pbin" p.txt </dev/null 2>&1 | head -1)"
check 'p prints a file'          'one two three' \
    "$("$pbin" -5 p.txt 3<p.tty </dev/null 2>&1 | tr '\n' ' ' | sed 's/ $//')"
check 'p corrects a misspelling through spname' '"p p.txt"?' \
    "$("$pbin" p.tx 3<p.tty </dev/null 2>&1 | head -1 | sed 's/[?].*/?/')"
# ...AND SAY WHAT THIS SECOND ONE CANNOT SEE, because the mutation that should
# have proved it DID NOT FIRE and the reason is worth more than the case.
#
# spname accumulates the corrected path a component at a time into newname[],
# and upstream sized that 80 -- about five components of DIRSIZ 14.  At DIRSIZ
# 254 a SINGLE component overruns it, so this case supplies one: 255
# characters, macOS's NAME_MAX, misspelt in the last character so SPdist scores
# 2 and the unbounded copy runs.  It asserts that spname still WORKS at that
# length, which the five-character case above cannot (revert newname to 80 and
# "p.txt" still fits).
#
# IT DOES NOT ASSERT THE OVERFLOW, and cannot.  Measured: with newname[80] and
# the bound test deleted, this case stays GREEN, and so does a 1017-character
# five-component path.  newname[80] is followed in BSS by guess[255],
# best[255] and nbuf[257] -- 767 bytes of adjacent static storage that absorb
# the write -- so the string ends up contiguous and correct and the program
# behaves.  Third documented reason a mutation does not fire (undefined
# behaviour that happens to give the right answer), with a mechanism worth
# naming: the neighbours soak it.  Same verdict as strncat's overread, where
# the answer was right the whole time.
#
# So the fix rests on arithmetic rather than on this case -- 255 bytes copied
# into 80 is not a judgement call -- and the case guards the half that IS
# observable.  src/cmd/p/PORTING.md records the measurement.
#
# Relative, and run from the suite's own directory, deliberately: an absolute
# host path would be resolved against the rootfs root by the jail, spname would
# correctly find no match for its first component, and the case would assert
# nothing.  Measured -- /private/tmp/... returns "can't open", which is the
# jail working.
mkdir -p pbuf && ( cd pbuf
  long=$(printf 'x%.0s' $(seq 1 250))
  printf 'hi\n' > "${long}abcde"
  echo "${long}abcdX" > ../pbuf.name )
check 'p spname survives a 255-character component' 'suggested' \
    "$(cd pbuf && "$pbin" "$(cat ../pbuf.name)" 3<../p.tty </dev/null 2>&1 |
       head -1 | grep -q '^"p x' && echo suggested || echo no)"

# pp: a lex scanner with NO grammar, which is the idiom, and libl underneath
# it, which is the second library this port has imported.  What the output
# proves is the whole chain -- V8's lex generated the scanner, libl supplied
# yywrap(), and pp read troff's binary font tables out of the jail.
#
# -fR rather than the default: pp's default font is Memphis, and V8 shipped
# Memphis.out as a BINARY with no source table anywhere in the archive (the
# more(1)/pg(1) category).  Our dev202 installs exactly the eleven fonts
# troff's own DESC mounts, derived rather than listed, and R is one of them.
ppbin=$(v8which pp)
printf '/* c */\nint main(){ return 0; }\n' > pp.c
check 'pp emits troff intermediate output' 'x T 202|x init|x font 1 R' \
    "$("$ppbin" -fR pp.c </dev/null 2>&1 |
       grep -E '^x (T|init|font 1 )' | tr '\n' '|' | sed 's/|$//')"

# calendar: five artefacts from one directory, two of them shell scripts, and
# the split is upstream's -- the COMMAND is /usr/bin/calendar and the four
# helpers are in /usr/lib.  None of calendar1..4 is in any Admin table, so
# Admin/dest answers /usr/bin by FALL-THROUGH; the makefile and the shipped
# tree both say /usr/lib, which is two sources against the fall-through and
# exactly cpp's pattern.  The disagreement set below grows from three to six
# for this reason.
check 'calendar helpers are in /usr/lib' '4' \
    "$(n=0; for f in calendar1 calendar2 calendar3 calendar4; do
         [ -f "$V8ROOT/usr/lib/$f" ] && n=$((n+1)); done; echo $n)"
check 'and the command is in /usr/bin' 'script' \
    "$([ -x "$V8ROOT/usr/bin/calendar" ] &&
       grep -q '^PATH=' "$V8ROOT/usr/bin/calendar" && echo script)"
# calendar2 writes the egrep pattern for the next N days.  Asserting the whole
# pattern would freeze today's date into the suite, so what is checked is the
# RELATION: the pattern must match a line naming today and must not match one
# naming a date three months out.  Both months are computed here.
#
# THE CONSUMER IS V8's OWN egrep, WHICH IS WHAT calendar3 RUNS, and reaching
# for the host's grep instead is what made this case go red on 19 August 2026
# after months of passing.  When the window needs more than one day RANGE --
# 19 and 20 do not merge into one character class -- calendar2 puts the
# alternatives on separate LINES with the parentheses spanning the newline,
# which is legal only because egrep's LEXER returns the token OR for a newline
# (egrep.y:26 defines RIGHT as the newline, :157 is `case RIGHT: return
# (OR);').  POSIX grep -f reads each line as a complete pattern and answers
# `parentheses not balanced'.  So the case was right, calendar2 was right, and
# the tool it was measured with was the host's -- the whois shape exactly.
today=$(date '+%b %-d'); far=$(date -v+95d '+%b %-d' 2>/dev/null || echo 'Xxx 1')
"$V8ROOT/usr/lib/calendar2" </dev/null > cal.pat 2>&1
check 'calendar2 matches today' 'yes' \
    "$(printf '%s something\n' "$today" |
       "$V8ROOT/usr/bin/egrep" -i -f cal.pat >/dev/null && echo yes || echo no)"
check 'and not a date months away' 'no' \
    "$(printf '%s something\n' "$far" |
       "$V8ROOT/usr/bin/egrep" -i -f cal.pat >/dev/null && echo yes || echo no)"
# calendar4 filters a list of paths down to the readable-but-not-writable ones,
# which is the access(2) pair.  The negative half is what discriminates.
check 'calendar4 keeps a readable file' '/etc/passwd' \
    "$(printf '/etc/passwd\n/nonexistent\n' | "$V8ROOT/usr/lib/calendar4" 2>&1)"
# AND IT STILL OVERRUNS ITS 100-BYTE BUFFER, WHICH IS ASSERTED SO THAT FIXING
# IT MUST BE A DECISION.  calendar4 is four lines and the middle one is
# `while(gets(s))' into `char s[100]' -- gets(3) has no bound, so a path longer
# than the buffer walks off it.  Measured, with no file involved at all because
# the overrun happens in the READ: 85, 95, 100 and 105 characters exit 0; 115
# and 135 die on signal 10, SIGBUS.  205 here, comfortably past any stack
# slack.
#
# UPSTREAM'S OWN ON UPSTREAM'S HARDWARE, so S1 says record rather than repair:
# a VAX overran the same 100 bytes for the same input, and 100 is a bare number
# rather than arithmetic on DIRSIZ, so raising DIRSIZ did not create it.  Nor
# is it reachable in the world this port ships, where v8launch.sh gives every
# user a home of /usr/<name> -- it was found only because a probe put $HOME
# under the session scratchpad, which is 130 characters.
#
# cb's precedent: a case whose whole purpose is to assert that a bug is STILL
# THERE, so that tidying `gets' into `fgets' has to be a deliberate change to
# authentic source rather than a drive-by.  THE SIGNAL RATHER THAN THE STATUS,
# because a program may exit(138) itself -- the crash probe's own lesson -- and
# `died on a signal' rather than `died on 10', because the claim is that the
# overrun is still there and not which fault the stack layout produces.  The
# case above is the control: a short path must still work, or a calendar4 that
# crashed on everything would pass this one.
check 'calendar4 still overruns its 100-byte buffer' 'signal' \
    "$(long=$(awk 'BEGIN{s="/tmp/"; while(length(s)<205) s=s "c"; print s}')
       ( printf '%s\n' "$long" | "$V8ROOT/usr/lib/calendar4" >/dev/null 2>&1 )
       st=$?
       if [ $((st & 127)) -ne 0 ]; then echo signal; else echo "exited $st"; fi)"

# newgrp, showq, dmesg: the three that install OUTSIDE /usr/bin.  Their
# destinations are DERIVED from Bell Labs' tables (newgrp in Admin/binfiles,
# the other two in Admin/etcfiles); nothing in our Makefile spells them.
check 'newgrp installs to /bin'  'yes' "$([ -x "$V8ROOT/bin/newgrp" ] && echo yes)"
check 'showq installs to /etc'   'yes' "$([ -x "$V8ROOT/etc/showq" ] && echo yes)"
check 'dmesg installs to /etc'   'yes' "$([ -x "$V8ROOT/etc/dmesg" ] && echo yes)"
# THE TWO GROVELERS DECLINE, AND THE DECLINING IS THE POINT.  Both nlist() a
# kernel and read /dev/kmem; libkmemu manufactures a namelist holding _avenrun
# and _bootime and neither program's symbols are in it.  Supplying them would
# mean inventing a STREAMS subsystem and a kernel message buffer.  This is
# load(1) and w(1)'s precedent -- w says `No mem' -- and a correctly built
# program that cannot answer is a correct outcome.
# The message is `can't open /dev/mem' and the apostrophe is NOT quoted away
# here.  tests/wavea's own awk-in-single-quotes block already carries a warning
# that an apostrophe closes the string just as well in a comment as in code;
# the safe form is to match a substring that has none.  grep -c, not the text.
check 'showq says it cannot read memory' '1|1' \
    "$("$V8ROOT/etc/showq" </dev/null >sq.out 2>&1; printf '%s|%s' "$?" \
       "$(grep -c 'open /dev/mem' sq.out | tr -d ' ')")"
# dmesg SIGSEGV'd here until this batch, and the bug was in nlist(3) rather
# than in dmesg: the matching loop walked to the caller's `{ 0 }' terminator
# and read name[0] off address 0, where the counting loop AT THE TOP OF THE
# SAME FUNCTION already had the null test.  Nothing had reached it because the loop breaks at
# the requested symbol first, so every caller whose symbols are PRESENT walks
# past it -- dmesg is the first program here to ask for one that is absent.
# The status is asserted because a case whose expected output is empty cannot
# discriminate a crash, and 139 is what this printed.
check 'dmesg reports no namelist rather than crashing' '1|No namelist' \
    "$("$V8ROOT/etc/dmesg" </dev/null >dm.out 2>&1; printf '%s|%s' "$?" "$(grep -c . dm.out >/dev/null; sed -n '/No namelist/p' dm.out)")"
# ...and the CONTROL for that fix, because a "fix" that made nlist() return
# early would silence the crash and break the present-symbol path.  load(1)
# asks for _avenrun, which libkmemu does manufacture, and must still answer
# three numbers.
check 'load still reads a symbol that IS present' '3' \
    "$("$V8ROOT/usr/bin/load" </dev/null 2>&1 | sed -n 2p | wc -w | tr -d ' ')"
# dmesg IS THE LARGEST FLOOR GROUP AFTER lex -- 51 of 106 entries -- and this
# is the two-invocation guard for it rather than the 53-invocation sweep the
# cb/diffh/cpio block runs.  Fifty-one stack overflows is not a price `make
# test' should pay, and the discriminator needs only two: the option that does
# NOT set wflg and one that does.
#
# The bug is Berkeley's, 1981, and unbounded mutual recursion rather than a bad
# pointer -- done() ends `if (wflg) writebuf();' and writebuf() calls done()
# when /usr/adm/msgbuf will not open, which it never will, because the archive
# ships no /usr/adm and BUFFER is opened with no O_CREAT.  A VAX recursed
# identically, so S1 leaves it; see src/cmd/dmesg/PORTING.md and the group
# comment in tests/crash-probe.floor.
#
# `-i' is the ONE arm of the switch that does not set wflg, which is why the
# count is 51 and not 53.  If a change ever makes these two agree, the floor is
# wrong in one direction or the other and this goes red before the 13-minute
# probe does.
check 'dmesg -i reports and exits, like the bare invocation' '1' \
    "$("$V8ROOT/etc/dmesg" -i </dev/null >/dev/null 2>&1; echo $?)"
check 'dmesg -a still recurses to death, as the floor declares' '139' \
    "$("$V8ROOT/etc/dmesg" -a </dev/null >/dev/null 2>&1; echo $?)"

# libl: pp is its only consumer, and what the archive has to contain is
# yywrap() -- a lex program without one does not link.  Asserted on the
# ARCHIVE rather than on pp's behaviour, because pp linking at all is already
# the behavioural half and this says WHERE the symbol came from.
check 'libl.a is installed' 'yes' "$([ -f "$V8ROOT/usr/lib/libl.a" ] && echo yes)"
check 'libl.a defines yywrap' '1' \
    "$(nm "$V8ROOT/usr/lib/libl.a" 2>/dev/null | grep -c '^[0-9a-f]* T _yywrap')"
# main.o is in the archive and MUST NOT reach pp.  Upstream ships it for
# `lex spec.l && cc lex.yy.c -ll' with no main of your own; a linker searches
# an archive only for symbols still undefined, and pp.o -- an explicit object
# on the link line -- has already satisfied crt0's reference.  If that ever
# stopped being true the link would fail with a duplicate main, so what this
# checks is the premise: the member is really there to collide.
check 'libl.a carries main.o for lex programs without one' '1' \
    "$(ar t "$V8ROOT/usr/lib/libl.a" 2>/dev/null | grep -c '^main\.o$')"

# ---------------------------------------------------------------------------
# libplot: graph, prof, tek -- the three batch 2d deferred, and the finding
# that deferring them was a mistake.
#
# The recorded reason was that `-lplot' could not be satisfied: libplot.a has
# two members and graph calls six primitives it defines none of.  All measured,
# all true, conclusion false -- <iplot.h> is a file of MACROS, so graph's plot
# calls are printf()s and it references no plot function at all.  A grep for
# `line(' cannot tell a macro invocation from a call.  src/libplot/PORTING.md.

# THE CASE THAT DISCRIMINATES IS ON THE OBJECT, NOT THE OUTPUT, because that is
# the instrument that would have prevented the error.  If graph ever stops
# using the macro header its object grows plot symbols, and this goes red
# before anything else notices.
check 'graph.o references no plot function' '0' \
    "$(nm -u "$ROOT/build/stage0/graph/graph.o" 2>/dev/null |
       grep -cE '^_(openpl|closepl|erase|line|move|point|arc|box|circle)$')"
check 'and neither does prof.o, built with upstream -Dplot' '0' \
    "$(nm -u "$ROOT/build/stage0/prof/prof.o" 2>/dev/null |
       grep -cE '^_(openpl|closepl|erase|line|move|point|text|range)$')"
# ...and the CONTROL, which is what says the two above are a real property and
# not an accident of how the sweep is spelled.  driver.c dispatches through a
# table of function pointers, so it references them for real.
check 'plot(1) driver.o DOES reference them' '9' \
    "$(nm -u "$ROOT/build/stage0/plot/driver.o" 2>/dev/null |
       grep -cE '^_(openpl|closepl|erase|line|move|point|arc|box|circle)$')"

# graph emits plot(1)'s textual command language.  Asserted as a SHAPE rather
# than byte-for-byte: the coordinates depend on the data and the scaling, and
# freezing them would make the case a transcript rather than a guard.
gplot=$(printf '0 0\n1 1\n2 4\n3 9\n4 16\n' | "$(v8which graph)" 2>/dev/null)
check 'graph opens, ranges and erases' 'o|ra|e' \
    "$(printf '%s\n' "$gplot" | sed -n '1,3p' | awk '{print $1}' | tr '\n' '|' | sed 's/|$//')"
check 'graph draws lines and vectors' 'yes' \
    "$(printf '%s\n' "$gplot" | grep -q '^li ' && printf '%s\n' "$gplot" | grep -q '^v ' && echo yes)"

# graph | tek -- the whole pipeline, and the one place a DEVICE library is
# genuinely required.  Tektronix 4014 addressing is GS (035) followed by
# coordinate bytes, and erase is ESC FF; asserting those two says the escapes
# are real rather than that some bytes came out.
tekout=$(printf '0 0\n1 1\n2 4\n3 9\n4 16\n' | "$(v8which graph)" 2>/dev/null |
         "$(v8which tek)" 2>/dev/null | od -An -tx1 | tr -d ' \n')
check 'graph | tek emits the 4014 GS address escape' '1' \
    "$(printf '%s' "$tekout" | grep -c '1d')"
check 'graph | tek emits the 4014 ESC FF erase' '1' \
    "$(printf '%s' "$tekout" | grep -c '1b0c')"

# The archives, and what they are for.  libplot.a links NOTHING that graph
# needs -- naming it keeps libpath() from letting -lplot escape to the host
# SDK, which is exactly why shim/libm/dummy.c reproduces V8's empty libm.a.
check 'libplot.a is installed, at V8 two members' '2' \
    "$(ar t "$V8ROOT/usr/lib/libplot.a" 2>/dev/null | grep -c '[.]o$')"
check 'and it defines putnum and whoami, which the macros cannot' '2' \
    "$(nm "$V8ROOT/usr/lib/libplot.a" 2>/dev/null | grep -cE ' T _(putnum|whoami)$')"
# EVERY primitive driver.o wants, derived from driver.o rather than listed:
# the undefined set minus what libc supplies must be a subset of lib4014's
# text symbols.  A hand-written list of 28 names is the two-copies-of-one-list
# trap this suite has already been bitten by.
nm -u "$ROOT/build/stage0/plot/driver.o" 2>/dev/null | sed 's/^_//' | sort > tekneed.txt
nm "$V8ROOT/usr/lib/lib4014.a" 2>/dev/null |
    awk '$2=="T"{sub(/^_/,"",$3); print $3}' | sort -u > tekhave.txt
# Everything ELSE on tek's link line, not just libv8c: crt0 and the syscall
# stubs supply exit(), and _iob and _ctype are DATA rather than text, so a
# filter on T alone reported three false shortfalls.  Take D and C too.
nm "$ROOT/build/stage0/libc/libv8c.a" "$ROOT/build/stage0/v8sys/libv8stubs.a" \
   "$ROOT/build/stage0/v8sys/libv8sys.a" "$ROOT/build/stage0/crt0.o" 2>/dev/null |
    awk '$2=="T"||$2=="D"||$2=="C"||$2=="S"{sub(/^_/,"",$3); print $3}' |
    sort -u > libchave.txt
check 'lib4014 plus libc covers every symbol driver.o needs' '' \
    "$(comm -23 tekneed.txt tekhave.txt | comm -23 - libchave.txt |
       grep -v '^__' | tr '\n' ' ' | sed 's/ $//')"
# ...and the control: lib4014 really is where the plot half comes from, so
# removing it from consideration must leave a non-empty shortfall.
check 'and libc ALONE does not, which is why the device library is needed' 'short' \
    "$([ -n "$(comm -23 tekneed.txt libchave.txt | grep -v '^__')" ] && echo short || echo no)"

# ---------------------------------------------------------------------------
# qed -- Thompson's editor, ed's ancestor, and the second editor here after
# ex/vi.  Driven by a HERE-DOC on stdin and never through a backgrounding
# wrapper: src/cmd/ex/PORTING.md spends a page on a deadline helper that gave
# its argument /dev/null for stdin, which read as an editor executing nothing
# and cost four documents and a test exclusion.
qedbin=$(v8which qed)
mkdir -p qedt && ( cd qedt
  printf 'alpha\nbeta\ngamma\n' > f.txt
  printf 'e f.txt\n1,$p\n2s/beta/BETA/\n$a\ndelta\n.\nw\nq\n' |
      "$qedbin" > out.txt 2>&1 )
check 'qed reads a file and reports its size' '17' \
    "$(sed -n 1p qedt/out.txt)"
check 'qed prints, substitutes and appends' 'alpha BETA gamma delta' \
    "$(tr '\n' ' ' < qedt/f.txt | sed 's/ $//')"
check 'and it wrote the new byte count' '1' \
    "$(grep -c '^23$' qedt/out.txt)"

# THE CASE THAT MATTERS IS THE SHELL ESCAPE, because that is the path the LP64
# fix is on.  main.c:243 installs the real function `interrupt' for SIGINT;
# getfile.c:218 saves the previous handler across a `!command' and :246 puts it
# back.  Upstream stores it in an `int', which on a VAX is exact and here keeps
# the low 32 bits of a text address -- measured, a handler at 0x100ecc660 comes
# back as 0xecc660.  Widened to `long'; src/cmd/qed/vars.h says why it is not a
# function-pointer type (savint doubles as a -1 sentinel).
( cd qedt && printf 'e f.txt\n!echo escaped\n1,$p\nq\n' | "$qedbin" > esc.txt 2>&1 )
check 'qed runs a shell escape through V8 sh' 'escaped' \
    "$(grep -x escaped qedt/esc.txt)"
check 'and the buffer survives it' 'alpha' \
    "$(grep -x alpha qedt/esc.txt | head -1)"
# The truncation itself is not observable from qed's output -- restoring a bad
# handler only bites on a later ^C, which a suite cannot deliver reliably -- so
# it is asserted on the DECLARATIONS, which is where the defect lives and where
# a regression would reappear.  All four, because misc.c re-declares savint and
# widening the definition alone would leave that unit reading four bytes of
# eight.
#
# FIVE, not four: onhup, onquit, onintr, savint, onbpipe.  The first draft of
# this case said four and went red, which is the case correcting the sentence
# above it rather than the code -- onbpipe is a local in Unix() and is easy to
# miss when counting from the header alone.
check 'the five signal-handler variables are pointer-width' '5' \
    "$(cat $(printf '%s ' "$ROOT/src/cmd/qed/vars.h" "$ROOT/src/cmd/qed/getfile.c" \
                          "$ROOT/src/cmd/qed/misc.c") |
       grep -cE '^(long|	long|extern long)[[:blank:]]+(onhup|onquit|onintr|savint|onbpipe)')"

# ---------------------------------------------------------------------------
# THE ARTICLE IS THE ONLY ARTEFACT HERE WITH NO GUARD, AND IT ROTTED.
#
# Citations are swept, build edges are asserted, imported == installed is
# asserted, the libc boundary is swept -- and ARTICLE.md had exactly one thing
# about it that was mechanically checked, its test count, which is precisely
# the one thing that stayed current.  Measured from the log: four consecutive
# commits touched that file and changed NOTHING BUT THAT NUMBER (+1/-1 each),
# while two whole steps went unwritten.  An unexercised rule cannot be seen to
# be incomplete, arriving in this project's own write-up.
#
# So the article now has to agree with a number DERIVED FROM THE TREE rather
# than from other prose.  Three copies of a count agreeing with each other is
# the two-copies-of-a-wrong-list trap; the rootfs is the third thing that is
# neither.
#
# It is the COMMAND COUNT and not the test count, deliberately: the test total
# cannot be had without running every suite, which is circular from inside one
# of them, and the command count is what moves every time something is
# imported -- which is exactly the drift that went unnoticed.
artcount=$(grep -oE 'installs \*\*[0-9]+\*\* of the' "$ROOT/../ARTICLE.md" 2>/dev/null |
           grep -oE '[0-9]+' | head -1)
# EXECUTABLES, NOT DIRECTORY ENTRIES, and the correction is what the sentence
# in the article actually claims -- COMMANDS.  A listing of those three
# directories also counts /etc/passwd, /etc/group, /etc/ttys and /etc/motd,
# which are data.
#
# It is also the only spelling that is STABLE.  libkmemu manufactures
# /etc/utmp lazily when the first reader opens it, so a listing is 139 on a
# tree that has never been used and 140 after anything runs who(1) -- measured,
# and caught on this guard's own first run, where it would have passed or
# failed on whether an earlier suite happened to run who.  That is the question
# this file has now been caught by four times: would this still pass on a tree
# that has never been used?
realcount=$(find "$V8ROOT/bin" "$V8ROOT/usr/bin" "$V8ROOT/etc" -maxdepth 1 \
                 -type f -perm -u+x 2>/dev/null |
            sed 's|.*/||' | sort -u | wc -l | tr -d ' ')
check "ARTICLE.md's command count matches the installed world" "$realcount" "$artcount"

# ---------------------------------------------------------------------------
# struct(1) -- batch 2e.  Brenda Baker's Fortran-to-Ratfor restructurer.
#
# THE FOUR GOTO CASES ARE THE LOAD-BEARING ONES, and they are four rather than
# one on purpose: each asserts a DIFFERENT structured construct, so a
# regression in one arm cannot hide behind the others.  Every one of them
# SIGSEGV'd in fixvalue before batch 2e -- including the straight-line case,
# which is why the plain ones are here too as the floor.
#
# The pipeline is the real test: /usr/bin/struct is a shell script with no
# `#!' line, run by V8's sh, calling /usr/lib/struct/{structure,beautify}.
# So a passing case exercises the script, both binaries, and the jail
# resolving /usr/lib/struct -- which no single-binary case would.
mkdir -p struct && cd struct || exit 1
STRUCTSH="$V8ROOT/usr/bin/struct"
strunt() { # strunt <fortran-on-stdin> -> beautified ratfor, blank lines gone
	cat > s.f
	"$V8ROOT/bin/sh" "$STRUCTSH" s.f 2>/dev/null | sed '/^[[:blank:]]*$/d' |
	    tr '\n' '|' | sed 's/|$//'
}

check 'struct: straight-line code survives' \
    'subroutine a(n)|n = 1|return|end' \
    "$(printf '      SUBROUTINE A(N)\n      N = 1\n      RETURN\n      END\n' | strunt)"

# A backward GOTO is a loop, and REPEAT/UNTIL is what says so.  This is the
# case that exchange()'s half-width swap broke: negate() flips the IF, and
# with a 32-bit swap the undefined child read as +4294967295 so DEFINED() was
# true and mkthen asserted.  src/cmd/struct/PORTING.md defect 6.
check 'struct: backward GOTO becomes REPEAT/UNTIL' \
    'subroutine d(n)|repeat|	n = n-1|	until(n<=0)|return|end' \
    "$(printf '      SUBROUTINE D(N)\n   10 N = N - 1\n      IF (N .GT. 0) GOTO 10\n      RETURN\n      END\n' | strunt)"

# A GOTO out of a DO loop is a break.  Exercises a different arm of getthen
# than the one above -- this one keeps the loop and rewrites the exit.
check 'struct: GOTO out of a DO becomes break' \
    'subroutine e(n,m)|do i = 1,n {|	if (i==m)|		break 1|	m = m+i|	}|return|end' \
    "$(printf '      SUBROUTINE E(N,M)\n      DO 20 I = 1, N\n         IF (I .EQ. M) GOTO 30\n         M = M + I\n   20 CONTINUE\n   30 CONTINUE\n      RETURN\n      END\n' | strunt)"

# Three-way branch -> IF/ELSE IF/ELSE.  Reaches negate() by the OTHER arm
# (BRANCHTYPE on the false child), so it discriminates from the REPEAT case.
check 'struct: three-way GOTO becomes IF/ELSE IF/ELSE' \
    'subroutine f(a,b,n)|if (n<0)|	a = -b|else if (n==0)|	a = 0|else|	a = b|return|end' \
    "$(printf '      SUBROUTINE F(A,B,N)\n      IF (N .LT. 0) GOTO 10\n      IF (N .EQ. 0) GOTO 20\n      A = B\n      GOTO 30\n   10 A = -B\n      GOTO 30\n   20 A = 0\n   30 CONTINUE\n      RETURN\n      END\n' | strunt)"

# A computed GOTO is a switch, and two subroutines in one file prove the
# per-routine state is reset rather than carried.
check 'struct: computed GOTO becomes SWITCH, and 2 routines' \
    'subroutine g(k)|switch(k) {|	case 1:|		k = 1|	case 2:|		k = 2|	case 3:|		k = 3|	}|return|end|subroutine h(j)|j = j+1|return|end' \
    "$(printf '      SUBROUTINE G(K)\n      GOTO (10,20,30), K\n   10 K = 1\n      GOTO 40\n   20 K = 2\n      GOTO 40\n   30 K = 3\n   40 CONTINUE\n      RETURN\n      END\n      SUBROUTINE H(J)\n      J = J + 1\n      RETURN\n      END\n' | strunt)"

# beautify alone, because the pipeline above would pass if structure emitted
# already-beautiful output.  What beautify adds is lowercasing, operator
# rewriting (.gt. -> >) and spacing, so assert those on input it did not make.
check 'struct: beautify lowercases and rewrites operators' \
    'if (a>b)|	x = 1' \
    "$(printf 'IF(a .gt. b)\n\tx = 1\n' | "$V8ROOT/usr/lib/struct/beautify" 2>/dev/null |
       sed '/^[[:blank:]]*$/d' | tr '\n' '|' | sed 's/|$//')"

# The two binaries are in /usr/lib/struct and NOT in a command directory --
# upstream's own `cp structure beautify /usr/lib/struct'.  Asserted because
# $(call v8dest,...) would answer /usr/bin by fall-through, struct being in no
# Admin table at all, and that fall-through is what this rule overrides.
check 'struct: binaries live in /usr/lib/struct' 'beautify structure' \
    "$(ls "$V8ROOT/usr/lib/struct" 2>/dev/null | tr '\n' ' ' | sed 's/ $//')"
check 'struct: and the command is the shell script' 'yes' \
    "$(head -1 "$V8ROOT/usr/bin/struct" 2>/dev/null | grep -q 'trap' && echo yes)"
cd "$TMP" || exit 1


# ---------------------------------------------------------------------------
# csh(1) -- Bill Joy's C shell, 4.1BSD.  Task #93.
#
# THE POINT OF THESE CASES IS A BUG THAT IS INVISIBLE ON HALF THE MACHINES IN
# THE WORLD, so read the pid-width case below as the load-bearing one and these
# as the demonstration.  sh.proc.h declared `short p_pid'; palloc stores what
# fork returned and pchild compares it against what wait3 returns, so above
# 32767 the stored copy goes negative, never matches, PRUNNING is never
# cleared, and pjwait pauses forever.  Measured: palloc recorded 45267 and
# pjwait read back -20269, which is 0xB0D3 as a signed short.
#
# A FRESHLY BOOTED HOST HANDS OUT LOW PIDS, which is the same property that let
# the 16-bit p_pid survive in tests/kmemu, so these cases can pass against a
# broken shell on a machine that has just come up.  The suite says so out loud
# below rather than reporting a green run that proves nothing.
#
# DO NOT READ "CI RUNNER" AS "LOW PIDS", which is what the first version of this
# comment said.  Measured on the runner that caught the NEXT csh defect: pids
# reached 98638.  A runner is a machine with no HISTORY, not a machine with a
# low pid counter, and the two are different properties -- the counter climbs
# with everything the job has already spawned, and a full build plus seventeen
# suites spawns a great many.  The width case below is the guard because it is
# true at every pid, not because runners are predictable.
#
# The deadline is perl's alarm with exec -- NOT a backgrounding wrapper.
# src/cmd/ex/PORTING.md spends a page on why: a backgrounded job in a
# non-interactive shell gets its stdin from /dev/null, and that cost a whole
# false diagnosis.  `exec' keeps the descriptors csh was given.  csh mentions
# neither alarm nor SIGALRM anywhere (measured, 0 hits), so the deadline is
# transparent to it -- unlike V8's sh, which catches SIGALRM on purpose.
#
# FIVE SECONDS, AND THE NUMBER WAS SET BY MUTATION RATHER THAN CHOSEN.  The
# first draft said 20, and restoring the 16-bit p_pid to check that these cases
# can fail made the suite take NINE MINUTES to say so -- 26 invocations each
# waiting out its own deadline.  A bounded hang is not the same as a usable
# failure signal.  A run is ~10ms, so 5s is 500x headroom, and the repeat loop
# below stops at the first failure instead of paying the deadline twenty times.
CSHBIN=$V8ROOT/bin/csh
csh_c() { perl -e 'alarm 5; exec @ARGV' "$CSHBIN" -c "$1" 2>&1; }

# The four fork paths csh has, one case each, because they are different code
# and a single `it runs a command' case would let three of them rot.
check 'csh: an external command finishes'   'external' "$(csh_c '/bin/echo external')"
check 'csh: a pipeline finishes'            'piped'    "$(csh_c '/bin/echo piped | /bin/cat')"
check 'csh: backquotes finish'              'inner'    "$(csh_c 'set v = `/bin/echo inner`; echo $v')"
check 'csh: a sequence of three finishes'   'a|b|c' \
    "$(csh_c '/bin/echo a; /bin/echo b; /bin/echo c' | tr '\n' '|' | sed 's/|$//')"

# $status is the AIMED case: pchild only records p_reason after it has matched
# the pid in proclist (sh.proc.c:54), so a shell that reaped the child without
# matching it could still terminate and would report the wrong status.  The
# four cases above cannot tell those apart; this one can.
check 'csh: $status comes from the reaped child' 'status 1' \
    "$(csh_c '/bin/false ; echo status $status')"

# A builtin takes no fork at all, so it is the negative control: it passed
# throughout the hang, and a `fix' that broke csh entirely would fail it.
check 'csh: a builtin needs no child'       '42'       "$(csh_c '@ y = 6 * 7; echo $y')"

# Twenty in a row.  The hang was deterministic here and this is not aimed at
# flakiness -- it is aimed at a pid COUNTER, since a host sitting just below
# 32767 crosses it during a run and the failure would arrive mid-suite.
cshruns=0 cshi=0
while [ "$cshi" -lt 20 ]; do
	[ "$(csh_c '/bin/echo x')" = x ] || break	# see the deadline note
	cshruns=$((cshruns+1))
	cshi=$((cshi+1))
done
check 'csh: 20 consecutive externals all finish' '20' "$cshruns"

# THE GUARD THAT IS TRUE AT EVERY PID.  The cases above test the value, which
# is only wrong above 32767; this tests the WIDTH, which is wrong always.  Same
# reasoning as tests/kmemu asserting p_pid's field width beside its runtime
# value.
#
# THE FILE SET IS DERIVED FROM THE BUILT OBJECTS, not transcribed, and that is
# what makes it correct about nfunc.c: that file carries the identical `short
# ctpgrp' and upstream's own makefile does not build it (OBJS names sh.func.o),
# so nothing forces a change there and an allow-list naming it would be one
# more entry to go stale.  No object, no claim.  If anything ever compiles it,
# this sweep covers it with no edit.
cshshort= cshscanned=0
for o in "$ROOT"/build/stage0/csh/*.o; do
	[ -f "$o" ] || continue
	c="$ROOT/src/cmd/csh/$(basename "$o" .o).c"
	[ -f "$c" ] || continue
	cshscanned=$((cshscanned+1))
	grep -nE '^[[:blank:]]*(register[[:blank:]]+)?short[[:blank:]]+[a-z_]*(pid|pgrp|jobid)' \
	    "$c" >/dev/null 2>&1 && cshshort="$cshshort $(basename "$c")"
done
# The headers are not objects, so they are named from the Makefile's own
# dependency lines rather than found by the loop.  They are where the bug was.
for h in sh.h sh.proc.h; do
	cshscanned=$((cshscanned+1))
	grep -nE '^[[:blank:]]*short[[:blank:]]+(unsigned[[:blank:]]+)?[a-z_]*(pid|pgrp|jobid)' \
	    "$ROOT/src/cmd/csh/$h" >/dev/null 2>&1 && cshshort="$cshshort $h"
done
check "csh: no built source holds a pid in a short ($cshscanned files scanned)" \
    'none' "${cshshort:-none}"

# SAY WHETHER THIS HOST COULD HAVE SEEN IT.  A green run below 32767 is a green
# run against a shell that would hang on any developer's machine, and the whole
# reason this bug reached a commit is that nothing said so.
cshmaxpid=$(ps -Ao pid= 2>/dev/null | awk '{ if ($1+0 > m) m = $1+0 } END { print m+0 }')
if [ "${cshmaxpid:-0}" -gt 32767 ]; then
	echo "csh: host pids reach $cshmaxpid -- the value cases above are live"
else
	echo "csh: host pids only reach ${cshmaxpid:-0}, under 32767 -- the value" \
	     "cases above CANNOT see a 16-bit p_pid; only the width case can"
fi

# Installed to /bin, not /usr/bin: Admin/binfiles:10, and the shipped tree has
# /bin/csh.  $(call v8dest,...) derives it, so this asserts the derivation.
check 'csh: installed in /bin'          'yes' \
    "$([ -x "$V8ROOT/bin/csh" ] && echo yes)"
check 'csh: libjobs.a installed beside it' 'yes' \
    "$([ -f "$V8ROOT/usr/lib/libjobs.a" ] && echo yes)"
# Nothing from the host.  sigset/sighold/sigrelse all EXIST in -lSystem as
# System V compatibility routines -- measured -- so a csh that lost libjobs
# would link silently and run on macOS's signal semantics instead of V8's.
check 'csh: imports nothing from libSystem' '' \
    "$(nm -u "$V8ROOT/bin/csh" 2>/dev/null | tr -d '[:space:]')"

# ---------------------------------------------------------------------------
# ratfor(1) -- Wave A2 batch 3, the first of the language systems.
#
# The deadline is perl alarm+exec and NOT a backgrounding wrapper, for the
# reason src/cmd/ex/PORTING.md spends a page on: a backgrounded job in a
# non-interactive shell gets its stdin from /dev/null, and ratfor reads stdin.
# ratfor names neither alarm nor SIGALRM (measured, 0 hits), so alarm bounds it.
ratf() { perl -e 'alarm 10; exec @ARGV' "$V8ROOT/usr/bin/ratfor" 2>&1; }

check 'ratfor: installed where V8 put it' 'yes' \
    "$([ -x "$V8ROOT/usr/bin/ratfor" ] && echo yes)"
check 'ratfor: imports nothing from libSystem' '' \
    "$(nm -u "$V8ROOT/usr/bin/ratfor" 2>/dev/null | tr -d '[:space:]')"

# THE yylval HALF.  Text reaching the GOK token is carried through the parser
# as a POINTER in yylval and walked by outcode(xp) char *xp.  With r.h back at
# upstream int, this is a SIGSEGV on the first line -- so what the case asserts
# is that the text arrives at all, not merely that ratfor exits 0.
check 'ratfor: text survives the GOK pointer path' \
    'call small(i)' \
    "$(printf 'if (i < 5)\n\tcall small(i)\nend\n' | ratf | sed -n 's/^ *\(call.*\)/\1/p' | head -1)"

# THE yyval HALF, AND IT NEEDS A RELATION RATHER THAN A LITERAL.  genlab()
# hands out small sequential labels; with r.h back at upstream int, yaccpar
# leaves the upper half of a POINTER in yyval and a 4-byte store replaces only
# the lower half, so a label prints as e.g. 4294990298 -- and ratfor still
# EXITS 0 with Fortran that looks correct apart from the numbers.  A case
# checking the exit status or the statement text cannot see it.  Every label V8
# generates is five digits from 23000 up, so anything wider is the bug.
check 'ratfor: every generated label fits in a V8 int' '' \
    "$(printf 'while (i < 3) {\n\ti = i + 1\n}\nend\n' | ratf |
       awk '{ for (k = 1; k <= NF; k++) if ($k+0 > 99999) print $k }' | head -3 | tr '\n' ' ' | sed 's/ $//')"

# The control for the case above: the labels are not merely small, they are the
# RIGHT ones, and they nest.  A repeat/until is a top label and a trailing test.
check 'ratfor: repeat/until becomes a backward goto' \
    '23000 continue|23001 if(.not.(j .gt. 4))goto 23000' \
    "$(printf 'repeat {\n\tj = j + 1\n} until (j > 4)\nend\n' | ratf |
       sed -n 's/^ *//; /^23/p' | tr '\n' '|' | sed 's/|$//')"

# struct(1) IS ratfor's INVERSE -- it reads Fortran and writes Ratfor -- so the
# pair round-trips, and that is an end-to-end assertion needing no f77.  struct
# is a shell script exec-ing the absolute path /usr/lib/struct/structure, which
# resolves only INSIDE the jail, so it has to run under V8's sh; run from the
# host it exits 127 and the case would be measuring the host shell.
if [ -x "$V8ROOT/usr/bin/struct" ] && [ -x "$V8ROOT/bin/sh" ]; then
	rtin=$TMP/rt.f; rtmid=$TMP/rt.r
	printf '      i = 0\n10    i = i + 1\n      if (i .lt. 10) goto 10\n      end\n' > "$rtin"
	PATH=/bin:/usr/bin:/etc "$V8ROOT/bin/sh" -c "struct < $rtin" > "$rtmid" 2>/dev/null
	check 'struct: Fortran becomes a Ratfor repeat' 'repeat' \
	    "$(sed -n 's/^[ \t]*//; /^repeat/p' "$rtmid" | head -1)"
	check 'ratfor: and ratfor turns it back into Fortran' 'yes' \
	    "$(ratf < "$rtmid" | grep -q 'goto 23000' && echo yes)"
else
	fail=$((fail+1)); echo "FAIL struct or sh missing, so the round trip was not run"
fi

# A CASE WHOSE WHOLE PURPOSE IS TO ASSERT THAT AN UPSTREAM BUG IS STILL THERE,
# which is cb(1)s precedent in this file.  ratfor/BUGS is a mail message from
# Rick Becker dated 4 March 1981 reporting that an unclosed for( sends ratfor
# into a loop; Kernighan shipped it unfixed.  It reproduces here exactly --
# exit 142 is 128+SIGALRM with zero bytes of output -- and S1 forbids repairing
# it, so repairing it has to be a DECISION rather than a tidy-up.
#
# The deadline is what makes this safe to run at all: a hang is the failure
# mode that reports nothing, and without a bound it takes the suite down
# instead of failing it.
check 'ratfor: the 1981 BUGS report still hangs, as upstream shipped it' \
    '142 0' \
    "$(printf 'for(i=1;i<=2;i=i+1\n\t\tii=1\nstop\nend\n' > "$TMP/bugs.r"
       out=$(perl -e 'alarm 5; exec @ARGV' "$V8ROOT/usr/bin/ratfor" < "$TMP/bugs.r" 2>&1); st=$?
       echo "$st $(printf '%s' "$out" | wc -c | tr -d ' ')")"

# efl(1) -- Wave A2 batch 3, the second language system.  Feldman's Extended
# Fortran Language: structured control flow, C-like data structures and generic
# procedures, compiling to Fortran 66.
#
# The deadline is ratfor's, and for ratfor's reason: efl reads stdin when given
# no file, and `perl -e alarm; exec' is the only wrapper that does not eat it.
eflc() { perl -e 'alarm 15; exec @ARGV' "$V8ROOT/usr/bin/efl" "$@" 2>&1; }

check 'efl: installed where V8 put it' 'yes' \
    "$([ -x "$V8ROOT/usr/bin/efl" ] && echo yes)"
check 'efl: imports nothing from libSystem' '' \
    "$(nm -u "$V8ROOT/usr/bin/efl" 2>/dev/null | tr -d '[:space:]')"

# THE INVARIANT WITH NO BUILD STEP LEFT TO PROTECT IT.  gram.c is checked in
# and upstream's yacc rule is commented out -- "gram.c can no longer be made on
# a pdp11 because of yacc limits" -- so the 83 token numbers baked into
# gram.c:1-83 and the 83 the build derives from `tokens' are two hand-written
# copies of one list.  Nothing regenerates either from the other.  Token
# numbers ARE line numbers in `tokens', so inserting one line renumbers every
# token below it and the lexer starts returning numbers the parser reads as
# different tokens -- silently, since both halves still compile.  This is the
# shape that let v8fsd's and p9cl's errno tables agree perfectly about a set
# that was missing seven names; here the third source is the tree itself.
#
# Compared as SETS in both directions, not as a count: two lists of 83 can
# differ and still both be 83.
check 'efl: the checked-in parser agrees with tokens, both ways' '83 83 0 0' \
    "$(sed -n '1,85p' "$ROOT/src/cmd/efl/gram.c" |
         sed -n 's/^#[[:blank:]]*define[[:blank:]]*\([A-Z][A-Z0-9]*\)[[:blank:]]*\([0-9][0-9]*\).*/\1 \2/p' |
         sort > "$TMP/efl.gram"
       grep -n . "$ROOT/src/cmd/efl/tokens" |
         sed 's/\([^:]*\):\(.*\)/\2 \1/' | sort > "$TMP/efl.tok"
       echo "$(wc -l < "$TMP/efl.gram" | tr -d ' ') $(wc -l < "$TMP/efl.tok" | tr -d ' ')" \
            "$(comm -23 "$TMP/efl.gram" "$TMP/efl.tok" | wc -l | tr -d ' ')" \
            "$(comm -13 "$TMP/efl.gram" "$TMP/efl.tok" | wc -l | tr -d ' ')")"

# The generated scanner is lex(1) THROUGH an ed(1) script.  fixuplex makes two
# edits and both are load-bearing: the substitution routes character input
# through efl's own efgetc (so `include' and `define' can push text back), and
# the 27-line append is three global flags the lex skeleton has no way to
# express.  Assert BOTH -- and assert the substitution by the ABSENCE of the
# original text, since a fixuplex that silently did nothing leaves getc(yyin).
# Aimed at the LINE the ed script edits, not at a global count: `efgetc' also
# occurs in lex.l's own action code, so a count of occurrences conflates
# fixuplex's edit with source that was always there.  What fixuplex changes is
# the input() macro, and the absence of getc(yyin) anywhere is what says the
# substitution ran rather than silently failing -- V7 ed prints `?' on a failed
# substitute and keeps going, so a broken script exits 0.
check 'efl: fixuplex rerouted the scanner through efgetc' 'yes 0' \
    "$(printf '%s %s' \
        "$(grep 'define input()' "$ROOT/build/stage0/efl/lex.c" |
             grep -q 'efgetc' && echo yes)" \
        "$(grep -c 'getc(yyin)' "$ROOT/build/stage0/efl/lex.c" | tr -d ' ')")"
check 'efl: fixuplex appended the pushback block' 'yes' \
    "$(grep -q 'if(pushlex)' "$ROOT/build/stage0/efl/lex.c" && echo yes)"

# The language itself.  Feldman's for takes COMMAS, not semicolons.
check 'efl: a procedure becomes a subroutine' 'subroutine main' \
    "$(printf 'procedure main\ninteger i\ni = 1\nend\n' > "$TMP/e1.e"
       eflc "$TMP/e1.e" | sed -n 's/^ *\(subroutine main\)$/\1/p')"

# for/next/while/repeat-until all lower to gotos, which is the whole point of
# the program.  Asserting the STRUCTURE rather than the text: a backward goto
# must exist, and the relational must have become Fortran's .gt. form.
check 'efl: for becomes a counted loop with a backward goto' 'yes' \
    "$(printf 'procedure main\ninteger i, s\ns = 0\nfor(i = 1, i <= 3, i = i + 1)\n\ts = s + i\nend\n' > "$TMP/e2.e"
       eflc "$TMP/e2.e" | grep -q '\.gt\.' && echo yes)"
check 'efl: repeat/until becomes a backward goto' 'yes' \
    "$(printf 'procedure main\ninteger s\ns = 0\nrepeat\n\t{\n\ts = s + 1\n\t}\nuntil(s > 5)\nend\n' > "$TMP/e3.e"
       eflc "$TMP/e3.e" | grep -q '\.le\.' && echo yes)"

# STRUCTURES ARE THE HEADLINE FEATURE and the one ratfor does not have, so this
# is the case that says efl is efl.  An array of a mixed-type struct becomes a
# 2-D array plus an EQUIVALENCE aliasing the same words at a second type --
# there is no other way to say it in Fortran 66 -- and that is also the hardest
# exercise of the block allocator and symbol table in the program.
check 'efl: a struct array becomes an equivalenced 2-D array' 'yes yes yes' \
    "$(printf 'procedure main\n\t{\n\tstruct point\n\t\t{\n\t\treal x, y\n\t\tinteger tag\n\t\t}\n\tpoint p(8)\n\tinteger i\n\tfor(i = 1, i <= 8, i = i + 1)\n\t\t{\n\t\tp(i).x = i\n\t\tp(i).tag = 0\n\t\t}\n\t}\nend\n' > "$TMP/e4.e"
       o=$(eflc "$TMP/e4.e")
       printf '%s %s %s' \
         "$(printf '%s' "$o" | grep -q 'integer p(3, 8)' && echo yes)" \
         "$(printf '%s' "$o" | grep -q 'equivalence' && echo yes)" \
         "$(printf '%s' "$o" | grep -q 'p(3, i) = 0' && echo yes)")"

# THE CASE THIS PORT OWES A COMPILER BUG.  efl's misc.c:417 is
# `p->vproc = q->vproc = v' -- a chained assignment to two bit fields -- and it
# crashed at address 0x80, which is the spliced field value stored AS an
# address.  tests/v8ccom carries the aimed unit cases; this is the end-to-end
# one, and it is here rather than there because what it proves is that the
# 12k-line program built on the fixed compiler actually runs.  Any efl output
# at all exercises setvproc: `procedure main' alone reaches it through
# extname().
check 'efl: the chained-bitfield path runs (compiler bug 0x80)' '0 yes' \
    "$(printf 'procedure main\nend\n' > "$TMP/e5.e"
       o=$(eflc "$TMP/e5.e"); st=$?
       printf '%s %s' "$st" "$(printf '%s' "$o" | grep -q 'subroutine main' && echo yes)")"

# efl leaves eflc/efld/efle.PID scratch files ONLY when it dies before its own
# cleanup -- which is how tests/deps caught the crash above, as litter in the
# repo root.  A clean compile must leave none, and the case runs in its own
# directory so it measures efl rather than whatever else has run.
check 'efl: a clean compile leaves no scratch files' '' \
    "$(mkdir -p "$TMP/eflw" && cd "$TMP/eflw" && rm -f efl[cde].*
       printf 'procedure main\ninteger i\ni = 1\nend\n' > c.e
       perl -e 'alarm 15; exec @ARGV' "$V8ROOT/usr/bin/efl" c.e >/dev/null 2>&1
       ls efl[cde].* 2>/dev/null | tr '\n' ' ' | sed 's/ $//')"

# ---------------------------------------------------------------------------
# libF77 and libI77 -- the Fortran runtime, and REACHED BY NOTHING INSTALLED,
# so a probe is the only instrument.  Same shape as tgotoprobe above, and for
# the same recorded reason: when a mutation would not fire because there is no
# CONSUMER rather than because the code is dead or the case is vacuous, a probe
# is what fixes it.  f77 itself is three more programs -- the driver, the front
# end, and /lib/f1, a SECOND code generator in pcc1's architecture -- and none
# of them exists yet.
#
# THE FIRST CASE IS THE LINK, and it is the one carrying the finding.  V8's own
# /usr/lib/libI77.a references setvbuf and _bufendtab, and neither appears in
# any other archive Bell Labs shipped -- measured across all of them -- so no
# Fortran program could reach ld's exit on a real V8.  shim/libI77/sysv.c is
# what closes that, and a link that succeeds is the whole assertion.
#
# The archives are named as -lF77 -lI77 rather than by path DELIBERATELY: that
# is the driver's own liblist from f77's drivedefs, in its order, so the case
# also exercises libpath() in src/cmd/cc.c resolving -lNAME against the rootfs
# instead of escaping to the macOS SDK.
if "$CC" -o f77probe "$ROOT/tests/wavea/f77probe.c" -lF77 -lI77 -lm \
	>f77p.log 2>&1; then
	check 'f77 runtime: links, which V8s own libI77.a could not' 'linked' \
	    "$([ -x f77probe ] && echo linked)"
	# nm -u empty is the same assertion tests/kmemu makes tree-wide: nothing
	# leaked to -lSystem.  It is worth repeating here because libF77 defines
	# cabs, sinh, cosh and tanh and libI77 defines ecvt, all five of which
	# libv8c ALSO defines -- so a link that quietly took the host's would
	# still run and would still be wrong.
	check 'f77 runtime: imports nothing from the host' '' \
	    "$(nm -u f77probe 2>/dev/null | tr '\n' ' ' | sed 's/ $//')"
	f77out=$(./f77probe 2>&1); f77st=$?
	# The intrinsics.  One token, because a per-check breakdown is what the
	# probe's own stderr gives when it goes wrong.
	check 'f77 runtime: the libF77 intrinsics agree with Fortran' 'checks ok' \
	    "$(printf '%s\n' "$f77out" | grep '^checks')"
	# THE FORMATTED-WRITE PATH, which is the only thing that reaches the two
	# objects built with -D_bufend='(unsigned char *)v8_bufend'.  XB rather
	# than AB is the load-bearing token: the format is (A2,T1,A1), so T1
	# moves the cursor BACK to column 1 over an already-written AB, and that
	# backward move is the sole caller of _bufend at wrtfmt.c:48,58 and
	# wsfe.c:41.  A purely forward write never reaches it, so `plain' alone
	# would assert nothing about the rename.
	check 'f77 runtime: a formatted WRITE, cursor moved backward by T' 'XB' \
	    "$(printf '%s\n' "$f77out" | sed -n 1p)"
	check 'f77 runtime: and a second record follows it' 'plain' \
	    "$(printf '%s\n' "$f77out" | sed -n 2p)"
	# The status as well as the output: a probe that SIGSEGVs after printing
	# has printed the right thing.  CLAUDE.md's fourth kind of vacuous case.
	check 'f77 runtime: the probe exits 0' '0' "$f77st"
else
	fail=$((fail+6)); echo "FAIL f77probe (build)"; head -5 f77p.log
fi

# ---------------------------------------------------------------------------
# f77(1) -- the driver, stage 2 of four.  f77pass1 and /lib/f1 do not exist yet,
# so what is testable is everything EXCEPT compiling: the option handling, the
# machine description the build generates, and the whole link path.
#
# THE STATUS IS USELESS HERE AND UPSTREAM IS WHY.  doload() ends `await(waitpid);'
# -- a bare statement, no if -- so f77 exits 0 on a FAILED link, on a VAX as
# much as here.  Measured: a bare `f77' reports an undefined _MAIN__ and exits 0.
# That is upstream's defect on upstream's hardware and S1 leaves it, so every
# case below asserts the ARTEFACT.  Same shape as rm(1) not being an instrument
# for write errors.
if [ -x "$V8ROOT/usr/bin/f77" ]; then
	check 'f77: installed, and imports nothing from the host' 'yes' \
	    "$([ -z "$(nm -u "$V8ROOT/usr/bin/f77" 2>/dev/null)" ] && echo yes)"

	# THE END-TO-END CASE, and it is the one worth having: an object that
	# defines MAIN__, linked BY THE DRIVER against stage 1's libraries, run.
	# f77probe.c is the stage-1 probe, reused -- it is shaped like a Fortran
	# program already, because libF77's own main() calls MAIN__.
	mkdir -p f77dw && (cd f77dw && rm -f prog f77probe.o
	  "$CC" -c "$ROOT/tests/wavea/f77probe.c" >cc.log 2>&1
	  "$V8ROOT/usr/bin/f77" f77probe.o -o prog >ld.log 2>&1) >/dev/null 2>&1
	# The ftnlen line is filtered out here and asserted on its own below, so
	# that a width change fails the WIDTH case rather than this one -- a case
	# that fails for two unrelated reasons names neither.
	check 'f77: links an object against libF77/libI77 and it runs' 'XB plain checks ok' \
	    "$(cd f77dw && ./prog 2>/dev/null | grep -v '^ftnlen' | tr '\n' ' ' | sed 's/ $//')"
	check 'f77: and what it produced is a V8 binary, not a host one' '' \
	    "$(nm -u f77dw/prog 2>/dev/null | tr '\n' ' ' | sed 's/ $//')"

	# NOTHING ON THE LINK LINE MAY ESCAPE THE ROOTFS, and this is a RELATION
	# rather than a transcription: -d prints the command, and the assertion is
	# that the only -l left unresolved is -lSystem.  Upstream's liblist is
	# { -lF77, -lI77, -lm, -lc }; clang resolves -l against the macOS SDK, so
	# an unresolved one is a hole in the jail -- the bug cc.c documents, where
	# -lm answered with a libSystem re-export and the link died on an _errno
	# with no address.  Counting rather than matching paths, because the paths
	# hold $V8ROOT and that differs per machine.
	check 'f77: every -l but -lSystem resolves inside the rootfs' '0' \
	    "$(cd f77dw && "$V8ROOT/usr/bin/f77" -d f77probe.o -o /dev/null 2>&1 |
	       tr ' ' '\n' | grep '^-l' | grep -vx '\-lSystem' | wc -l | tr -d ' ')"
	# ...and the archives it DID name are all under $V8ROOT.  The pair matters:
	# the case above would also pass if the liblist had simply been emptied.
	#
	# SIX, not four, and getting that wrong first is the point: upstream's
	# liblist has four entries but -lc is not an archive -- it EXPANDS to
	# libv8c, libv8stubs and libv8sys, which is what V8's libc is here.  So
	# three resolved from the liblist (F77, I77, m) plus three from that
	# expansion.  A transcribed number would have hidden the expansion.
	check 'f77: and the six archives it names are all V8s' '6' \
	    "$(cd f77dw && "$V8ROOT/usr/bin/f77" -d f77probe.o -o /dev/null 2>&1 |
	       tr ' ' '\n' | grep "^$V8ROOT.*\.a$" | wc -l | tr -d ' ')"
	# The loader flags reach ld through clang.  -X warns, a bare -x is read by
	# CLANG as "specify language" and kills the link naming a different flag,
	# and -Wl, is right.  Asserted because the middle one looked like a fix.
	check 'f77: the loader flags are routed through clang with -Wl' '-Wl,-x -Wl,-u -Wl,_MAIN__' \
	    "$(cd f77dw && "$V8ROOT/usr/bin/f77" -d f77probe.o -o /dev/null 2>&1 |
	       tr ' ' '\n' | grep '^-Wl,' | tr '\n' ' ' | sed 's/ $//')"

	# ---------------------------------------------------------------
	# f77pass1 -- stage 3.  It EMITS AND IS NOT YET CONSUMED: the pcc
	# intermediate it writes needs /lib/f1 to become assembly.  So the
	# cases below assert what is verifiable today, and the last one
	# asserts where the pipeline STOPS, so stage 4 arriving is a
	# decision rather than a discovery -- the same reason tests/kmemu
	# asserts that w(1) says `No mem'.
	check 'f77pass1: installed, and imports nothing from the host' 'yes' \
	    "$([ -x "$V8ROOT/usr/lib/f77pass1" ] &&
	       [ -z "$(nm -u "$V8ROOT/usr/lib/f77pass1" 2>/dev/null)" ] && echo yes)"

	# It compiles a Fortran program.  The two progress lines are the
	# HERE==ARM64 arm of driver.c:393 working -- upstream spells "is this
	# a Unix" as a list of the three Unixes V8 knew, and without this
	# machine in that list there would be no output at all.
	rm -rf f77p1w && mkdir -p f77p1w && (cd f77p1w
	  printf '      program hello\n      integer i\n      i = 2\n      write(6,10) i\n   10 format(1x,i3)\n      end\n' > h.f
	  "$V8ROOT/usr/lib/f77pass1" - h.f h.s h.d h.x) >p1.log 2>&1
	check 'f77pass1: compiles a Fortran program' 'h.f: MAIN hello:' \
	    "$(tr -s ' \n' ' ' < p1.log | sed 's/^ //; s/ $//')"

	# THE ASSEMBLY IT WRITES DIRECTLY IS arm64/Mach-O, NOT VAX, and this is
	# what says arm64.c is in the link rather than vax.c.  Four directives
	# that only this target uses: the Mach-O section name, p2align (VAX
	# says .align), and quad (VAX has no eight-byte directive because it
	# has no eight-byte pointer).
	#
	# THE SECTION IS __DATA,__const AND NOT __TEXT,__const, which this case
	# spelled until a computed GOTO needed a jump table.  A table of `.quad
	# L16' is a table of ADDRESSES, and arm64 Mach-O refuses a relocation in
	# __TEXT: `ld: Found illegal text-relocations', with f77 reporting
	# success over the top of it because doload() discards the status.  Both
	# names are equally Mach-O and equally un-VAX, so the case still
	# discriminates exactly what it says it does; see arm64defs.
	check 'f77pass1: and the assembly is arm64 Mach-O, not VAX' 'yes' \
	    "$(cd f77p1w && grep -q '__DATA,__const' h.s &&
	       grep -q 'p2align' h.s && ! grep -q '\.word\|LWM' h.s && echo yes)"
	# It reaches libI77 by name, which is the two halves of the port
	# meeting: these are the same entry points stage 1's probe called by
	# hand before any compiler existed.
	#
	# ALL THREE, and the program has to have an output LIST for that: a bare
	# `write(6,10)' emits only the record start and end, because there is
	# nothing for do_fio to transfer.  The first draft of this case used one
	# and expected three, and what it measured was my reading of the program
	# rather than the compiler -- so the program grew an `i' instead.
	check 'f77pass1: emits calls to the libI77 entry points' 'do_fio e_wsfe s_wsfe' \
	    "$(cd f77p1w && grep -oE '_(s_wsfe|do_fio|e_wsfe)' h.s |
	       sed 's/^_//' | sort -u | tr '\n' ' ' | sed 's/ $//')"
	# The intermediate is BINARY -- putpcc.c's second line says "NEW
	# VERSION USING BINARY POLISH POSTFIX INTERMEDIATE" -- so it is
	# asserted as bytes rather than as text.  A nonempty h.x is what
	# stage 4 will have to read.
	check 'f77pass1: writes a nonempty binary intermediate' 'yes' \
	    "$(cd f77p1w && [ -s h.x ] && echo yes)"

	# ---------------------------------------------------------------
	# /lib/f1 -- stage 4.  It READS AND DOES NOT YET COMPILE, so what is
	# asserted is the format decoder, which is the part that had to be
	# right before anything else could be.
	check 'f1: installed, and imports nothing from the host' 'yes' \
	    "$([ -x "$V8ROOT/lib/f1" ] &&
	       [ -z "$(nm -u "$V8ROOT/lib/f1" 2>/dev/null)" ] && echo yes)"

	# THE DECODER, AGAINST AN INDEPENDENT DECODE.  This sequence was read
	# out of a hexdump by hand before f1 existed -- p2triple encodes every
	# record as op | var<<8 | type<<16 -- so the case compares a reader
	# against a decode of the same bytes done a different way, rather than
	# against itself.  You can read the Fortran back out of it: NAME/ICON/
	# ASSIGN is `i = 2', and the three ICONs joined by two LISTOPs are
	# do_fio's three arguments.  The eight PASS records before GOTO are the
	# epilogue and entry stub: prsave() and goret() in arm64.c emit three
	# lines each as literal text, which is where the frame lives.
	#
	# A RUN OF PASS RECORDS IS COLLAPSED TO ONE `PASS*', because their COUNT
	# is a property of a different file.  This case pinned eight of them and
	# went red when prsave() grew from three instructions to seventeen --
	# correctly, but for a reason that has nothing to do with decoding a
	# stream: the frame gained the callee-saved registers and the spill of
	# x0-x7.  What the hand decode is evidence about is the SEQUENCE of
	# operators, and a frame this pass merely copies through should not be
	# able to invalidate it.  The frame has two cases of its own below.
	check 'f1: decodes the intermediate as an independent hand decode did' \
	    'PASS* LBRACKET STMT LABEL NAME ICON ASSIGN STMT ICON ICON CALL STMT ICON ICON ICON LISTOP ICON LISTOP CALL STMT ICON CALL0 STMT LABEL PASS* GOTO STMT RBRACKET PASS*' \
	    "$(cd f77p1w && "$V8ROOT/lib/f1" -d h.x | awk '{print $1}' |
	       awk '$0=="PASS"{if(!p)printf "PASS*\n"; p=1; next} {p=0; print}' |
	       tr '\n' ' ' | sed 's/ $//')"
	# The type word is pcc1's four-bits-per-level encoding, and a function
	# address is the base type under PTR and FUNCT -- 148 for an int.  That
	# is the one field an adapter to this port's pcc2 back end would have
	# had to re-shift, so it is asserted rather than assumed.
	check 'f1: reads pcc1s stacked type word, so a callee is int * ()' 'int * ()' \
	    "$(cd f77p1w && "$V8ROOT/lib/f1" -d h.x |
	       sed -n 's/.*type \(int \* ()\) name "_s_wsfe".*/\1/p' | head -1)"
	# ...and it COMPILES: the twelve operators a whole program uses each
	# emit arm64, and the three that carry the program's meaning are
	# asserted by name.  A store for `i = 2', an adrp/add pair for each
	# address argument, and a bl per call.
	check 'f1: compiles the intermediate to arm64' 'adrp bl str' \
	    "$(cd f77p1w && "$V8ROOT/lib/f1" h.x |
	       grep -oE '^\t(bl|str|adrp)' | sed 's/^\t//' | sort -u |
	       tr '\n' ' ' | sed 's/ $//')"
	# The frame is arm64.c's, not f1's, and the ORDER is what proves it:
	# the epilogue must precede the entry stub, because putbracket() writes
	# the stub last.  An epilogue after the stub's branch is unreachable and
	# the body runs off the end -- which is what the first working version
	# did, so the order is asserted rather than the presence.
	check 'f1: the epilogue precedes the entry stub, not follows it' 'yes' \
	    "$(cd f77p1w && "$V8ROOT/lib/f1" h.x > o.s
	       e=$(grep -n 'ldp' o.s | head -1 | cut -d: -f1)
	       m=$(grep -n '^_MAIN__:' o.s | head -1 | cut -d: -f1)
	       [ -n "$e" ] && [ -n "$m" ] && [ "$e" -lt "$m" ] && echo yes)"

	# TWO COPIES OF ONE LIST, which is the trap this tree keeps finding.
	# shim/f1/f1.c respells pccdefs rather than including it, because
	# including f77's copy would put its whole defs chain on the include
	# path -- so the values are DERIVED from src/cmd/f77/pccdefs here and
	# compared, and a divergence is a failure rather than a silent
	# disagreement about what an opcode means.
	# THE LIST IS EVERY OPCODE f1.c SPELLS, derived from f1.c itself rather
	# than typed out again: a hand-written list is a third copy, and it goes
	# stale the moment an operator is implemented without anyone adding it
	# here -- which is what happened, thirteen names covering a file that now
	# names thirty-nine.  A "#define P2X n" in f1.c that pccdefs does not
	# have, or disagrees with, is what this must catch.
	#
	# THE BLANK AFTER THE NAME IS REQUIRED, NOT OPTIONAL, and the widened
	# list is what exposed it: a trailing [[:blank:]]* after P2STAR also
	# matches the line defining P2STAREQ, because zero blanks and zero digits
	# both satisfy it and the trailing .* eats the rest -- so the capture
	# picked up an extra EMPTY line and P2STAR compared 11 against a two-line
	# value.  The thirteen-name list this replaced contained neither P2STAR
	# nor P2STAREQ, so no input it was ever given could show it: a parser
	# correct for everything it had been handed, which is the third instance
	# of that shape in this suite.
	#
	# AND THIS COMMENT LIVES OUTSIDE THE SUBSTITUTION, which cost a run to
	# learn.  Written inside it in the Bell Labs quoting style, the opening
	# backquote of a quoted name STARTS A COMMAND SUBSTITUTION and the whole
	# file stops parsing -- "unexpected EOF while looking for matching
	# backquote", reported 3400 lines away from the comment.  CLAUDE.md
	# records the apostrophe version of this for an awk program inside single
	# quotes; a backquote inside a substitution is the same hazard one
	# character over, and being a comment is no shelter from either.
	check 'f1: its opcode numbers agree with f77s pccdefs' '' \
	    "$(for n in $(sed -n 's/^#define[[:blank:]]*\(P2[A-Z0-9]*\)[[:blank:]].*/\1/p' \
	                    "$ROOT/shim/f1/f1.c"); do
	         grep -q "^#define $n " "$ROOT/src/cmd/f77/pccdefs" || continue
	         a=$(sed -n "s/^#define $n[[:blank:]][[:blank:]]*\([0-9]*\).*/\1/p" "$ROOT/src/cmd/f77/pccdefs")
	         b=$(sed -n "s/^#define $n[[:blank:]][[:blank:]]*\([0-9]*\).*/\1/p" "$ROOT/shim/f1/f1.c")
	         [ "$a" = "$b" ] || printf '%s(%s!=%s) ' "$n" "$a" "$b"
	       done)"
	# AND THE COUNT, because a derived list that derives NOTHING passes the
	# case above without comparing anything -- 0 disagreements over 0 names
	# reads exactly like a clean tree.  This is tests/cpp's `if [ -d ]' skip
	# and cites.awk's 0-stale-over-0-checked, arriving in a sweep written
	# three minutes ago.  The floor is well under the real number so that
	# implementing an operator does not edit a test.
	check 'f1: and that comparison was not vacuous' 'yes' \
	    "$(n=$(sed -n 's/^#define[[:blank:]]*\(P2[A-Z0-9]*\)[[:blank:]].*/\1/p' \
	              "$ROOT/shim/f1/f1.c" | wc -l)
	       [ "$n" -ge 30 ] && echo yes)"

	# WHERE THE PIPELINE STOPS.  With pass 1 present the driver gets one
	# step further than it did before stage 3, and this case is what makes
	# that visible: the message names /lib/f1 rather than /usr/lib/f77pass1.
	# THE WHOLE PIPELINE.  f77 runs all four programs -- driver, f77pass1,
	# f1, clang -- and produces a linked executable.  This case has been
	# rewritten three times, each time the thing it named arrived: it
	# asserted `Cannot load /usr/lib/f77pass1', then `Cannot load /lib/f1',
	# and now the artefact.  Re-derive what a case discriminates rather than
	# deleting it as stale.
	#
	# AND IT RUNS.  Five programs, each adding one thing the one before it
	# did not have, so a failure names which construct broke rather than
	# only that Fortran is broken.  Every one of them found a real defect
	# while being written -- the character literal found a char * stored in
	# an int, the addition found a NAME being addressed rather than loaded,
	# and the loop found CBRANCH's inverted sense.
	f77run() {
		( cd f77p1w && printf '%s\n' "$2" > r.f && rm -f r
		  "$V8ROOT/usr/bin/f77" r.f -o r >/dev/null 2>&1
		  [ -x r ] && ./r 2>&1 | tr '\n' '|' )
	}
	check 'f77: a Hollerith format' 'hello|' \
	    "$(f77run h '      program h
      write(6,10)
   10 format(5hhello)
      end')"
	check 'f77: a character literal and an integer' ' n = 42|' \
	    "$(f77run q "      program q
      integer n
      n = 42
      write(6,10) n
   10 format(1x,'n = ',i2)
      end")"
	check 'f77: arithmetic on two variables' '  5|' \
	    "$(f77run a '      program a
      integer i,j
      i = 2
      j = 3
      write(6,10) i+j
   10 format(1x,i2)
      end')"
	check 'f77: multiplication and three output items' '  6 7 42|' \
	    "$(f77run m '      program m
      integer i,j
      i = 6
      j = 7
      write(6,10) i, j, i*j
   10 format(1x,i2,i2,i3)
      end')"
	# THE LOOP IS THE ONE THAT MATTERS: it exercises the induction variable
	# in a register, a compound assignment used as an EXPRESSION, and the
	# conditional branch -- and it printed 1 rather than 15 until CBRANCH's
	# sense was inverted, which is a plausible wrong answer rather than a
	# crash.  A case asserting only that it ran would have passed.
	check 'f77: a DO loop, summing 1 to 5' ' sum = 15|' \
	    "$(f77run l "      program l
      integer k, s
      s = 0
      do 20 k = 1, 5
         s = s + k
   20 continue
      write(6,10) s
   10 format(1x,'sum = ',i2)
      end")"

	# EVERYTHING ABOVE IS ONE PROCEDURE WITH NO SUBSCRIPT AND NO FLOAT, which
	# is what the code generator could do when it was written.  The cases
	# below are the rest of Fortran, and each one names a defect that was
	# live when it was added -- none is a demonstration of something that
	# already worked.
	#
	# THE FIRST TWO ARE ABOUT THE FRAME, and they fail in different halves.
	# `mvarg' was an empty function while a comment in the same file said
	# pass 2 spilled x0-x7; nothing did, so a parameter was read out of the
	# saved frame pointer and `call greet(7)' compiled clean and SIGSEGV'd.
	check 'f77: a subroutine with a by-reference parameter' ' 7|' \
	    "$(f77run s '      program s
      call greet(7)
      end
      subroutine greet(n)
      integer n
      write(6,*) n
      end')"
	# AND THIS ONE COULD NOT HAVE EXISTED BEFORE, which is why the defect
	# survived: arm64defs gave AUTOREG and ARGREG the same register with no
	# ARGOFFSET, so a parameter and a temporary both lived at [x29+0].  Only
	# a procedure holding BOTH can show it -- measured, the dump had `OREG
	# reg 29 offset 0 type int' and `OREG reg 29 offset 0 type int *' in one
	# procedure, two objects at one address with nothing in the record to
	# tell them apart.  A function call inside a WRITE is what forces the
	# temporary, so the shape of this program is the assertion.
	check 'f77: a parameter and a temporary do not share an address' ' 4 9|' \
	    "$(f77run t '      program t
      call two(2, 3)
      end
      subroutine two(p, q)
      integer p, q, sq
      write(6,*) sq(p), sq(q)
      end
      integer function sq(n)
      integer n
      sq = n * n
      return
      end')"
	# ARRAYS, which needed three things at once: P2LSHIFT (the `operator 64'
	# every array refused on -- a subscript is scaled by a shift), P2INDIRECT
	# to reach the element, and a `PLUS type int *' computed in x rather than
	# w.  The last is the port's dominant bug class arriving inside the code
	# generator: a(i) is (&a - 4) + (i<<2), so a 32-bit add truncates the
	# address.  Summed rather than printed elementwise so a single wrong
	# element cannot hide.
	check 'f77: an array, subscripted and summed' ' 385|' \
	    "$(f77run r '      program r
      integer a(10), i, s
      s = 0
      do 10 i = 1, 10
         a(i) = i * i
   10 continue
      do 20 i = 1, 10
         s = s + a(i)
   20 continue
      write(6,*) s
      end')"
	# A RECURSIVE FUNCTION, which is really a case about ASSIGN.  Fortran
	# passes by reference, so an argument that is an expression needs
	# storage, and f77 builds that as `(temp = n-1, &temp)' -- an ASSIGN
	# with a COMOP over it, INSIDE a call's argument list.  Treating ASSIGN
	# as a statement and clearing the stack after it threw away the callee
	# and the half-built expression underneath it.
	check 'f77: a recursive function' ' 120|' \
	    "$(f77run f '      program f
      integer n, fact
      n = 5
      write(6,*) fact(n)
      end
      integer function fact(n)
      integer n
      if (n .le. 1) then
         fact = 1
      else
         fact = n * fact(n-1)
      endif
      return
      end')"
	# MIXED WIDTHS, and the half that is IMPLIED is the one that broke.  f77
	# emits an explicit `CONV type double' over an INTEGER and emits NOTHING
	# over a REAL -- pcc lets an operand be narrower than its operator -- so
	# a pass honouring only the stated conversions read a single-precision
	# bit pattern with `fadd d'.  2.5 + 1.5 came out 2.5 and the program
	# printed 7.5 where it should print 12: a plausible number, from an
	# expression in which every operator was one this pass knows.
	check 'f77: mixed DOUBLE PRECISION, REAL and INTEGER' '  1.200000000e+01|' \
	    "$(f77run w '      program w
      double precision d
      real r
      integer i
      d = 2.5d0
      r = 1.5
      i = 3
      d = d + r
      d = d * i
      write(6,*) d
      end')"
	# AND THE OTHER DIRECTION, which a store rather than an operator takes:
	# without it materas() handed back a FLOAT register and the integer
	# store named the same NUMBER in the other bank -- so `i = r' would have
	# stored whatever was in x8.
	check 'f77: REAL to INTEGER and back' ' 7|  7.00000000|' \
	    "$(f77run v '      program v
      real r
      integer i
      r = 7.5
      i = r
      write(6,*) i
      r = i
      write(6,*) r
      end')"
	# .AND. AND .OR., which found a defect that predates them.  A comparison
	# was left in the FLAGS for the branch to read, which is right until a
	# second comparison arrives before the first has been used -- and
	# `i .gt. 3 .and. j .lt. 4' is exactly that.  Nothing had produced two
	# comparisons in one expression, so nothing could show it.
	check 'f77: .AND. and .OR.' ' 1| 2|' \
	    "$(f77run n '      program n
      integer i, j
      i = 17
      j = 2
      if (i .gt. 3 .and. j .lt. 4) write(6,*) 1
      if (i .lt. 3 .or.  j .lt. 4) write(6,*) 2
      end')"
	# MOD has no arm64 instruction -- the VAX divide returned a remainder
	# and this one does not -- so it is sdiv plus msub, and it needs a third
	# register because both operands are still live at the multiply.
	check 'f77: MOD and unary minus' ' 2| -17|' \
	    "$(f77run o '      program o
      integer i, j
      i = 17
      j = mod(i, 5)
      write(6,*) j
      write(6,*) -i
      end')"
	# A COMPUTED GOTO, which crashed f77pass1 itself.  prcmgoto() was
	# written taking `struct Labelblock *labs[]' when putcmgo() passes `int
	# labarray' -- the label of a table it has ALREADY emitted -- so the
	# array subscript dereferenced a label number and pass 1 died at address
	# 0x13, which was 19, which was the label.  Then the table it does emit
	# is `.quad L16', an ADDRESS, and arm64 Mach-O refuses a relocation in
	# __TEXT: the link failed and f77 reported success over the top of it.
	# Three targets and a fall-through, so a table off by one is visible.
	check 'f77: a computed GOTO' ' 11| 22| 33| 99|' \
	    "$(f77run g '      program g
      integer i
      do 10 i = 1, 4
         goto (1,2,3), i
         write(6,*) 99
         goto 10
    1    write(6,*) 11
         goto 10
    2    write(6,*) 22
         goto 10
    3    write(6,*) 33
   10 continue
      end')"
	# THE EXPONENT OF A LIST-DIRECTED REAL, which is libI77's defect and not
	# the compiler's -- reproduced from C, with /lib/f1 nowhere in the
	# picture, before it was fixed.  wrt_E chose its value by length and
	# then asked `p->pf != 0', the FLOAT arm of the union, to decide whether
	# to apply the scale.  On a VAX that is correct and only there:
	# D_floating's first 32 bits have the identical layout to F_floating, so
	# a double read as a float is the same value.  On IEEE little-endian
	# those four bytes are the LOW mantissa bits.
	#
	# SO IT BROKE FOR TIDY NUMBERS AND WORKED FOR UNTIDY ONES, which is why
	# these are TWO cases.  0.375 has 29 zero mantissa bits and printed
	# e+00; 0.0375 does not and printed e-02 correctly.  A fix that simply
	# always applied the scale would pass the first and fail the second, so
	# the untidy value is the control rather than a duplicate.
	check 'f77: a tidy REAL carries its exponent' '-01' \
	    "$(f77run e '      program e
      write(6,*) 0.375
      end' | sed 's/.*e//; s/|//')"
	check 'f77: and an untidy REAL still does' '-02' \
	    "$(f77run u '      program u
      write(6,*) 0.0375
      end' | sed 's/.*e//; s/|//')"

	# THE FRAME IS A RELATION, NOT A COUNT.  Every callee-saved pair the
	# entry stub pushes must come back off in the epilogue: an unbalanced
	# pair does not fail here, it corrupts the CALLER, which is the failure
	# mode nothing downstream can attribute.  Stated as a set comparison so
	# that adding or removing a saved register is covered without anyone
	# editing a number -- the count of PASS records used to be asserted and
	# went red for a frame change that was entirely correct.
	check 'f1: the frame restores exactly the registers it saved' '' \
	    "$(cd f77p1w && "$V8ROOT/lib/f1" h.x > b.s 2>/dev/null
	       s=$(sed -n 's/.*stp	\([dx][0-9]*\), \([dx][0-9]*\), \[sp, #-16\]!.*/\1 \2/p' b.s | sort)
	       r=$(sed -n 's/.*ldp	\([dx][0-9]*\), \([dx][0-9]*\), \[sp\], #16.*/\1 \2/p' b.s | sort)
	       [ -n "$s" ] || echo 'no saves found'
	       [ "$s" = "$r" ] || echo 'saved and restored sets differ')"

	# ---------------------------------------------------------------
	# STAGE 6.  A twenty-program corpus of ordinary Fortran found eight
	# more defects, and only ONE of them announced itself the way stage 5's
	# did -- the rest were a wrong answer, a desynchronised reader, or a
	# refusal naming a construct the program did not contain.  Each case
	# below is aimed at exactly one of them, with a control wherever a
	# one-sided fix would have passed.

	# A -- THE FLAGS DO NOT SURVIVE A CALL.  flushcc()'s survey said only
	# comparisons write NZCV, which was true of the code that existed when
	# it was written; `bl' arrived later.  So the first comparison's cset
	# ran on flags the CALLEE had left.  The value case needs a callee that
	# leaves LT set on the way out, or the wrong answer happens to be right
	# -- measured, an ordinary callee gave `false' either way.
	check 'f77: FALSE .and. a call is false, whatever the callee left in NZCV' ' right|' \
	    "$(f77run cc '      program cc
      integer fn
      i = 5
      if (i .lt. 3 .and. fn(i) .gt. 1) then
         write(6,*) "wrong"
      else
         write(6,*) "right"
      endif
      end
      integer function fn(k)
      integer m
      m = 1
      if (m .lt. 999) m = m + 1
      fn = k * 10
      return
      end')"
	# and the control, so a fix that simply always said false is caught
	check 'f77: TRUE .and. a call is still true' ' right|' \
	    "$(f77run cd '      program cd
      integer fn
      i = 5
      if (i .gt. 3 .and. fn(i) .gt. 1) then
         write(6,*) "right"
      else
         write(6,*) "wrong"
      endif
      end
      integer function fn(k)
      integer m
      m = 1
      if (m .lt. 999) m = m + 1
      fn = k * 10
      return
      end')"
	# THE STRUCTURAL CASE IS THE ONE THAT IS TRUE AT EVERY CALLEE.  The two
	# above depend on what fn happens to leave in the flags; this one does
	# not.  Between a `cmp' and the `bl' that destroys it there must be a
	# cset, or the comparison is being read after the call.
	check 'f77: a pending comparison is materialised before the bl' 'cmp cset bl' \
	    "$(cd f77p1w && printf '      program cs\n      integer fn\n      i = 5\n      if (i .lt. 3 .and. fn(i) .gt. 1) i = 9\n      end\n      integer function fn(k)\n      fn = k\n      return\n      end\n' > cs.f
	       "$V8ROOT/usr/lib/f77pass1" cs.f cs.s cs.d cs.x >/dev/null 2>&1
	       "$V8ROOT/lib/f1" cs.x 2>/dev/null |
	       grep -E '^	(cmp|cset|bl)	' | awk '{print $1}' |
	       head -3 | tr '\n' ' ' | sed 's/ $//')"

	# B -- A NAME'S OFFSET WORD.  putpcc.c:1232-1235 writes an extra word
	# before the name when the offset is nonzero, and the reader went
	# straight for the name -- so it read the offset's four bytes as the
	# head of the string and the stream desynchronised.  The first thing
	# the misalignment produced was opcode 0, which is not an operator, so
	# the diagnostic named a construct no program contains.
	#
	# THE TRIGGER IS ANY CONSTANT SUBSCRIPT PAST THE FIRST, in a plain
	# local array -- a(1) has offset 0 and set no flag, which is why it
	# worked throughout and is the control here.
	check 'f77: a constant subscript past the first' ' 22 44|' \
	    "$(f77run cb '      program cb
      integer a(4)
      a(2) = 22
      a(4) = 44
      write(6,*) a(2), a(4)
      end')"
	check 'f77: and element one, whose offset is zero, still works' ' 11|' \
	    "$(f77run cn '      program cn
      integer a(4)
      a(1) = 11
      write(6,*) a(1)
      end')"
	check 'f77: a COMMON variable at a nonzero offset' '  1.00000000  2.00000000|' \
	    "$(f77run cm '      program cm
      common /c/ p, q
      p = 1.0
      q = 2.0
      write(6,*) p, q
      end')"

	# C -- QUEST AND COLON, which Fortran reaches through ABS and MIN/MAX
	# rather than through any operator a programmer writes.  intr.c:672
	# expands ABS as `0 <= t ? t : -t', so the commonest intrinsic in the
	# language was refused with `operator 22 (COLON)'.  In a postfix stream
	# both arms are already evaluated when COLON arrives, so this is csel.
	#
	# BOTH SIGNS, because each selects a different arm of the one csel.
	check 'f77: IABS of a negative and a positive' ' 3 7|' \
	    "$(f77run ca '      program ca
      i = -3
      j = 7
      write(6,*) iabs(i), iabs(j)
      end')"
	check 'f77: MAX0 and MIN0' ' 9 3|' \
	    "$(f77run cx '      program cx
      write(6,*) max0(3,9), min0(3,9)
      end')"
	# the float arm is fcsel and not csel, so it is a separate case
	check 'f77: ABS of a REAL, which is fcsel' '  3.50000000|' \
	    "$(f77run cr '      program cr
      x = -3.5
      write(6,*) abs(x)
      end')"

	# D -- A FLOATING VALUE CROSSING A CALL IS ALWAYS A DOUBLE, which is
	# K&R's `no float return' rule.  Not one of the typed functions in
	# libF77, libI77 or libc/math returns `float'; putpcc.c:551-552 forces
	# a TYREAL result as P2DREAL too.  f77's own table calls these results
	# real anyway, and on a VAX that cost nothing because D_floating's
	# leading word has F_floating's layout -- the same coincidence that
	# broke wrt_E, meeting IEEE a second time.
	#
	# THE PRECISION IS THE DISCRIMINATOR, not the value: a single and a
	# double of the same quantity must DIFFER, and both must be right.  A
	# pass that read s0 got neither.
	check 'f77: SQRT single and double, each right for its width' '  1.41421354   1.41421356' \
	    "$(f77run cq '      program cq
      double precision d
      x = 2.0
      d = 2.0d0
      write(6,*) sqrt(x)
      write(6,*) dsqrt(d)
      end' | tr '|' ' ' | sed 's/  *$//')"
	# the by-REFERENCE half: pow_dd is called with two double * and still
	# returns a double.  `x ** 0.5' printed 3.01e+23 before this.
	check 'f77: REAL ** REAL, whose callee takes pointers and returns a double' '  1.41421354|' \
	    "$(f77run cp '      program cp
      x = 2.0
      write(6,*) x ** 0.5
      end')"
	# the FORCE half: a Fortran REAL FUNCTION must return a double too
	check 'f77: a REAL FUNCTIONs own result' '  9.00000000|' \
	    "$(f77run cf '      program cf
      real sq
      x = 3.0
      write(6,*) sq(x)
      end
      real function sq(y)
      sq = y * y
      return
      end')"

	# E -- THE INTEGER POOL IS SIZED FROM WHAT PASS 1 SAYS IT TOOK.  Five
	# registers was the worst case assumed for every procedure, while the
	# LBRACKET record states the real number and it is 0 in every program
	# measured -- so four callee-saved registers sat idle while expressions
	# were refused for want of a fifth.  Character concatenation is what
	# found it.
	check 'f77: character concatenation, which needs six live values' ' foobar    |' \
	    "$(f77run cc2 '      program cc2
      character*10 a, b, c
      a = "foo"
      b = "bar"
      c = a(1:3) // b(1:3)
      write(6,*) c
      end')"
	# AND A FOUR-WAY ONE REACHES THE FOUR THAT PASS 1 DECLARED IT DID NOT
	# TAKE, which is what makes the regvars half of the change exercised
	# rather than merely written.  Measured, in registers live at once:
	# one-way 1, two-way 6, three-way 8, four-way 10, five-way refuses.
	# The old fixed pool was five, so it could not do a two-way -- `a // b',
	# the simplest concatenation there is.
	check 'f77: a four-way concatenation, which needs all ten' ' abcd      |' \
	    "$(f77run c4 '      program c4
      character*10 a, b, c, d, e
      a = "a"
      b = "b"
      c = "c"
      d = "d"
      e = a(1:1) // b(1:1) // c(1:1) // d(1:1)
      write(6,*) e
      end')"

	# AND THE POOL MUST NOT OUTGROW THE PROLOGUE.  A relation rather than a
	# list: every x register f1 materialises into has to be one the entry
	# stub saved, or the pass corrupts its caller.  This is what bounds
	# widening the pool, and it needs no number in the case.
	check 'f1: every register it allocates is one the prologue saves' '' \
	    "$(cd f77p1w && "$V8ROOT/lib/f1" cs.x > p.s 2>/dev/null
	       sed -n 's/.*stp	x\([0-9]*\), x\([0-9]*\), \[sp, #-16\]!.*/\1 \2/p' p.s |
	         tr ' ' '\n' | sort -u > sv.txt
	       grep -v -E '^	(stp|ldp)	' p.s |
	         sed -n 's/^	[a-z][a-z]*	x\([0-9]*\),.*/\1/p' | sort -u |
	         awk '$1 >= 19 && $1 <= 28' > us.txt
	       comm -23 us.txt sv.txt)"

	# G -- THE INTERNAL-I/O CONTROL BLOCK.  io.c already carried IOALIGN
	# and a comment deferring the internal list because nothing had reached
	# it; `read(buf,...)' reached it, and ld refused the object outright.
	check 'f77: an internal READ from a CHARACTER buffer' ' 2468|' \
	    "$(f77run cI '      program cI
      character*20 buf
      buf = "  1234"
      read(buf,"(i6)") n
      write(6,*) n*2
      end')"
	# STATED AS A RELATION OVER THE INIT RECORDS, so the OPEN and INQUIRE
	# lists are covered the day they are corrected and nobody edits this
	# case: every pointer f77 places in a control block must sit on an
	# eight-byte boundary, because that is what the linker requires and
	# what the C struct does.
	check 'f77: every pointer in an I/O control block is 8-aligned' '' \
	    "$(cd f77p1w && printf '      program ci\n      character*20 b\n      b = "  12"\n      read(b,"(i4)") n\n      write(6,*) n\n      end\n' > ci.f
	       "$V8ROOT/usr/lib/f77pass1" ci.f ci.s ci.d ci.x >/dev/null 2>&1
	       grep '\.quad' ci.d | awk '{ off = $2 + 0; if (off % 8) print off }')"

	# H -- ASSIGN.  f77pass1 SIGSEGV'd on `assign 20 to lbl' alone, before
	# any GOTO: putop descends a conversion chain and refreshes lp before
	# re-testing p's tag, so at a leaf it read leftp out of a Constblock.
	# On a VAX that offset was past a 24-byte block and the garbage byte
	# went into a variable the loop then stopped using; here it is a NULL
	# field and `lp->vtype' reads address 1.
	#
	# REACHABLE ONLY BECAUSE SZADDR STOPPED EQUALLING SZLONG -- expr.c
	# skips the conversion when typesize[ltype] >= typesize[rtype], which
	# was 4 >= 4 on a VAX and is 4 >= 8 here, so the OPCONV that trips the
	# loop was never built before.
	check 'f77: ASSIGN alone compiles and runs' ' ok|' \
	    "$(f77run cg '      program cg
      assign 20 to lbl
      write(6,*) "ok"
   20 continue
      end')"
	# THE ASSIGNED GOTO BRANCHES, AND WHAT IT STORES IS NOT AN ADDRESS.  It
	# needs a code address in a Fortran INTEGER; that is four bytes here and
	# Mach-O loads text above 4GB, so what the variable holds is the
	# DISTANCE from a label /lib/f1 emits at the head of each procedure, and
	# the branch adds it back.  Both ends compute it at run time from
	# adrp/add pairs, so it does not depend on the two labels sharing a
	# section.  Before the GOTO record's `var' flag was read this emitted
	# `b L4' -- P2INT is 4 -- and the assembler reported an undefined label,
	# naming something the program never had.
	#
	# THREE TARGETS AND NOT ONE, because a single-target case cannot tell a
	# working branch from one that falls through to the next statement --
	# which is where the only label is.  Reassignment is what makes it a
	# branch on the VARIABLE rather than on the last ASSIGN seen.
	check 'f77: an assigned GOTO branches, to each of three targets' ' 1 1| 2 2| 3 3|' \
	    "$(f77run ag '      do 5 i = 1,3
         call hop(i)
    5 continue
      end
      subroutine hop(k)
      integer lbl, k, n
      assign 30 to lbl
      if (k .eq. 1) assign 10 to lbl
      if (k .eq. 2) assign 20 to lbl
      goto lbl, (10,20,30)
   10 n = 1
      goto 40
   20 n = 2
      goto 40
   30 n = 3
   40 write(6,*) k, n
      return
      end')"
	# AND THE STRUCTURAL HALF: what reaches the variable must not be an
	# address.  The value case above cannot see a truncating store -- a
	# `str w' of a text address keeps the low half, and if the branch added
	# the base to THAT it would still be wrong by a whole 4GB, which faults
	# rather than printing a wrong number, so a green value case and a
	# crash are the only two outcomes and the case cannot distinguish the
	# mechanism.  What discriminates is that the difference is taken at all:
	# a `sub' of two run-time addresses must precede the store, and the
	# branch must add one back.  Derived from the emitted code.
	check 'f77: the assigned label is stored as a difference, not an address' 'sub 1 add 1 br 1' \
	    "$(cd f77p1w && printf '      integer lbl\n      assign 20 to lbl\n      goto lbl\n   20 continue\n      end\n' > chs.f
	       rm -f chs.s
	       "$V8ROOT/usr/bin/f77" -S chs.f >/dev/null 2>&1
	       awk '/^Lf1b[0-9]+:/ { b = 1 }
	            /adrp[[:blank:]]+x[0-9]+, Lf1b[0-9]+@PAGE/ { seen = 1; next }
	            seen && /sub[[:blank:]]+x[0-9]+, x[0-9]+, x[0-9]+/ { nsub++; seen = 0; next }
	            seen && /add[[:blank:]]+x[0-9]+, x[0-9]+, x[0-9]+/ { nadd++; seen = 0; next }
	            /br[[:blank:]]+x[0-9]+/ { nbr++ }
	            END { if (!b) print "no base label emitted"
	                  else printf "sub %d add %d br %d\n", nsub+0, nadd+0, nbr+0 }' chs.s)"
	# the two controls: the direct and computed forms still branch
	check 'f77: a direct GOTO still works' ' right|' \
	    "$(f77run cj '      program cj
      goto 20
   10 write(6,*) "wrong"
      goto 30
   20 write(6,*) "right"
   30 continue
      end')"
	check 'f77: a computed GOTO still works' ' 22|' \
	    "$(f77run ck '      program ck
      i = 2
      goto (10,20,30), i
   10 write(6,*) 11
      goto 40
   20 write(6,*) 22
      goto 40
   30 write(6,*) 33
   40 continue
      end')"

	# A SECOND CORPUS OF TEN FOUND FOUR MORE, so the first twenty were not
	# exhaustive and this is where the sweep stopped rather than where the
	# defects did.  Three are LP64 width arriving in a LAYOUT rather than in
	# a value, and the fourth is a feature Fortran has had since 1966.

	# I -- AN ARGUMENT OCCUPIES A SLOT, NOT ITS OWN WIDTH.  proc.c's
	# nextarg() advanced by typesize[type], which is right when every
	# argument type is the same size, and on a VAX they all were.  Here a
	# pointer is 8 and a hidden character length is 4, so the two part
	# company the first time a length PRECEDES a pointer -- which is what a
	# CHARACTER FUNCTION is: result pointer, result length, then the
	# argument.  f77 addressed the third at ARGOFFSET+12 where prsave()
	# spills it at +16.
	#
	# THE ASSEMBLER CAUGHT IT, WHICH IS THE GOOD DIRECTION: an offset that
	# is not a multiple of 8 has no scaled encoding, the unscaled form is
	# limited to [-256,255], and clang refused the file.  A packed offset
	# that happened to land 8-aligned would have read the wrong argument in
	# silence.
	check 'f77: a CHARACTER FUNCTION, whose hidden length precedes a pointer' ' abc   |' \
	    "$(f77run cu '      program cu
      character*6 up
      write(6,*) up("abc")
      end
      character*6 function up(s)
      character*(*) s
      up = s
      return
      end')"
	# the control: ONE character argument, where packed and slotted agree,
	# and which therefore worked throughout
	check 'f77: and one CHARACTER argument, where the two layouts agree' ' abcde|' \
	    "$(f77run cv '      program cv
      character*5 s
      s = "abcde"
      call show(s)
      end
      subroutine show(t)
      character*(*) t
      write(6,*) t
      return
      end')"

	# J -- A DATA BLOCK'S ALIGNMENT CAME FROM ITS FIRST RECORD'S TYPE.
	# dodata() decides on the first record it sees, and on a VAX that was
	# always enough because ALIADDR, ALILONG and ALIINT were all 4.  Here a
	# block whose first record is a .long gets 4, so a 12-byte DATA array
	# put the next block at 12 and the one after at 36 -- and that one's
	# format pointer, correctly at offset 16 WITHIN its block, landed at
	# address 52.  `ld: pointer not aligned in v.1+0x34', naming the
	# nearest preceding symbol rather than the block at fault.
	check 'f77: an odd-sized DATA block before two FORMATs' '    1   2   3|' \
	    "$(f77run cw '      program cw
      integer a(3)
      data a /1,2,3/
      write(6,10) a
   10 format(1x,3i4)
      write(6,20) 1.5
   20 format(1x,e12.4)
      end' | sed 's/|.*/|/')"

	# K -- A PROCEDURE PASSED AS AN ARGUMENT IS `blr'.  The callee arrives
	# as an OREG -- a spilled parameter holding an address -- rather than
	# as the ICON-with-a-name a direct call gives, and f1 refused it as "a
	# call through a value rather than a name".  That was an accurate
	# description of EXTERNAL, which Fortran has had since 1966.
	check 'f77: a procedure passed as an argument' '  9.00000000|' \
	    "$(f77run cy '      program cy
      external sq
      call apply(sq, 3.0)
      end
      subroutine apply(f, x)
      external f
      call f(x)
      return
      end
      subroutine sq(x)
      write(6,*) x*x
      return
      end')"

	# L -- OPEN, CLOSE AND INQUIRE, whose deferral named the wrong
	# instrument.  io.c said the first program to OPEN a file "will refuse
	# to link"; it does not.  A READ/WRITE block is INITIALISED DATA, so
	# its pointers are relocations and ld checks them -- but an OPEN block
	# is filled in by ioset() at RUN TIME, so there is no relocation and
	# nothing to check.  Measured: it linked cleanly and SIGSEGV'd.
	check 'f77: OPEN, WRITE, CLOSE and READ round-trip a file' ' 1234|' \
	    "$(f77run cz '      program cz
      open(unit=9, file="wq.tmp", status="unknown")
      write(9,*) 1234
      close(9)
      open(unit=9, file="wq.tmp", status="old")
      read(9,*) n
      close(9)
      write(6,*) n
      end')"
	check 'f77: INQUIRE finds a file that exists' ' t|' \
	    "$(f77run cA '      program cA
      logical ex
      inquire(file="/etc/passwd", exist=ex)
      write(6,*) ex
      end')"
	# AND THE RELATION FOR THE RUN-TIME-FILLED BLOCK, which the init-record
	# sweep above structurally cannot see: every POINTER field f77 stores
	# into an I/O control block must sit on an eight-byte boundary, because
	# that is where the C struct in src/libI77/fio.h reads it.  Derived
	# from the intermediate, so OPEN, CLOSE and INQUIRE are all covered by
	# one case and none of the offsets is transcribed here.
	check 'f77: every pointer f77 stores in an OPEN block is 8-aligned' '' \
	    "$(cd f77p1w && printf '      open(unit=9, file="z.tmp", status="unknown")\n      close(9)\n      end\n' > op.f
	       "$V8ROOT/usr/lib/f77pass1" op.f op.s op.d op.x >/dev/null 2>&1
	       "$V8ROOT/lib/f1" -d op.x 2>/dev/null |
	       awk '/^OREG/ && /\*/ { for(i=1;i<=NF;i++) if($i=="offset") o=$(i+1)+0; if (o%8) print "offset", o }')"

	# EVERY REFUSAL THE COMPILER CAN STILL MAKE IS ASSERTED, not merely
	# written down.  A refusal nothing tests is an unexercised rule, and the
	# failure mode is that it quietly stops refusing: `ralloc' handing back a
	# bogus register instead of exiting, or a ninth argument going somewhere
	# plausible.  Same reason tests/kmemu asserts that w(1) says `No mem'.
	# Each names its own reason, so crossing one of these is a decision.
	#
	# THE ALTERNATION IS A SECOND COPY OF A LIST THAT LIVES IN arm64.c AND
	# f1.c, so it goes stale in both directions and neither is loud.  It
	# carried `multiple ENTRY points are not implemented on this target'
	# for a step after that fatal() was deleted -- an arm that can never
	# match, which reads as documentation that the refusal survives -- and
	# it omitted argdest()'s, which fires BEFORE prolog()'s because argdest
	# runs inside prsave().  A refusal with no arm here captures nothing and
	# the case reports `got []', which is indistinguishable from a compiler
	# that produced no output.  Checked against the sources by the
	# `every refusal f77 can emit has an arm here' case below.
	f77refuse() {	# $2 = source; prints the reason, or the output if it RAN
		( cd f77p1w && printf '%s\n' "$2" > x$1.f && rm -f x$1
		  e=$("$V8ROOT/usr/bin/f77" x$1.f -o x$1 2>&1)
		  if [ -x x$1 ]; then echo "RAN: $(./x$1 2>&1 | tr '\n' '|')"
		  else printf '%s\n' "$e" | grep -oE 'f1: [0-9]+ arguments, more than the frame holds|procedure has [0-9]+ argument slots, more than the frame holds|entry has more than [0-9]+ argument slots|procedure needs [0-9]+ bytes of temporaries, more than the frame holds|an ASSIGNed label needs a 4-byte INTEGER|more integer registers than this pass allocates|ASSIGNed FORMAT specifier' | head -1
		  fi )
	}

	# ---- the argument area, whose bound is the FRAME's and not AAPCS64's.
	# A ninth argument used to be refused here.  Both ends had to move: the
	# caller writes the slots past the eighth into the call area prsave()
	# reserves below x29, and prsave() copies the incoming ones back down
	# beside the register arguments so that putpcc.c's single rule --
	# parameter n at ARGOFFSET+n -- stays true of all of them.
	#
	# THE CALLEE HALF WAS SILENTLY WRONG AND THE CALLER'S REFUSAL IS WHAT
	# HID IT.  f77pass1 addressed the ninth parameter at ARGOFFSET+64 and
	# prsave() wrote only eight slots, so `subroutine s9(a..i)' compiled
	# clean and read its ninth parameter's ADDRESS out of entry_sp-160 --
	# the slot holding the saved d14/d15 -- then stored through it.  A guard
	# on the caller is not a guard on the callee.
	check 'f77: a ninth argument is passed and received' 'RAN:  45|' \
	    "$(f77refuse 9 '      call s9(1,2,3,4,5,6,7,8,9)
      end
      subroutine s9(a,b,c,d,e,f,g,h,i)
      integer a,b,c,d,e,f,g,h,i
      write(6,*) a+b+c+d+e+f+g+h+i
      return
      end')"
	# AND THE STRUCTURAL HALF, WHICH GUARDS A DIFFERENT PROPERTY FROM THE
	# VALUE CASES -- measured, not assumed.  The first draft of this comment
	# said the value cases could not see the defect because a wild pointer
	# faults rather than computing a wrong number.  Mutation says otherwise:
	# dropping the copy loop fires all three value cases and leaves THIS one
	# green, because with the frame now large enough the ninth parameter sits
	# INSIDE it whether or not anything wrote there.
	#
	# So the two halves are: the arguments are actually placed (the value
	# cases), and the frame is big enough to hold them (this one).  The
	# second is what the original defect violated -- the ninth parameter was
	# addressed at exactly the offset goret unwinds from, which is why it
	# read the saved d14/d15 -- and the mutation that fires it is shrinking
	# F77FRAME back to the eight-slot size, which fires this case and one
	# other and nothing else.
	#
	# Nothing is transcribed: goret() unwinds with `add sp, x29, #N', so N is
	# the top of this frame BY CONSTRUCTION, and no reference the body makes
	# may reach it.  Both numbers move together if the frame changes.
	check 'f77: no parameter is addressed above the frame goret unwinds' 'ok' \
	    "$(cd f77p1w && printf '%s\n' '      subroutine s9(a,b,c,d,e,f,g,h,i)
      integer a,b,c,d,e,f,g,h,i
      i = a + i
      return
      end' > x9s.f && rm -f x9s.s
	       "$V8ROOT/usr/bin/f77" -S x9s.f >/dev/null 2>&1
	       awk '/add[[:blank:]]+sp, x29, #/ { t=$0; sub(/.*#/,"",t); n=t+0 }
	            { s=$0
	              while (match(s, /\[x29, #-?[0-9]+\]/)) {
	                t=substr(s,RSTART,RLENGTH); sub(/\[x29, #/,"",t); sub(/\]/,"",t)
	                if (t+0 > m) m=t+0
	                s=substr(s,RSTART+RLENGTH) } }
	            END { if (n==0) print "no epilogue found"
	                  else if (m >= n) print "parameter at", m, ">= frame top", n
	                  else print "ok" }' x9s.s)"
	# CHARACTER arguments are the shape that makes the slot rule bite: each
	# costs TWO slots, an address and a hidden length, so six arguments are
	# eleven slots and five of them arrive on the stack.
	check 'f77: CHARACTER arguments past the eighth slot' 'RAN:  aaaa bbbb cccc dddd eeee 7|' \
	    "$(f77refuse c '      character*4 p,q,r,s,t
      p = "aaaa"
      q = "bbbb"
      r = "cccc"
      s = "dddd"
      t = "eeee"
      call sc(p,q,r,s,t,7)
      end
      subroutine sc(a,b,c,d,e,n)
      character*(*) a,b,c,d,e
      integer n
      write(6,*) a, b, c, d, e, n
      return
      end')"
	# and the control: EIGHT arguments still go through
	check 'f77: and eight arguments still pass' ' 36|' \
	    "$(f77run c8 '      program c8
      call s8(1,2,3,4,5,6,7,8)
      end
      subroutine s8(a,b,c,d,e,f,g,h)
      integer a,b,c,d,e,f,g,h
      write(6,*) a+b+c+d+e+f+g+h
      return
      end')"
	# ---- MULTIPLE ENTRY POINTS, WHICH ARE THREE SEPARATE MECHANISMS.
	# doentry() gives a slot only to a parameter not already declared, so
	# the layout is the UNION of every entry in first-appearance order and a
	# later entry's x0 belongs somewhere other than slot 0.  prsave() takes
	# the entry now and spills straight to the right slot -- out of
	# registers, which are still live and are not themselves destinations,
	# so unlike the VAX it needs no staging area and cannot alias.
	check 'f77: a second ENTRY point is called by its own name' 'RAN:  1| 2|' \
	    "$(f77refuse e '      call one
      call two
      end
      subroutine one
      write(6,*) 1
      return
      entry two
      write(6,*) 2
      return
      end')"
	# THE REORDERED ENTRY IS THE ONE THAT DISCRIMINATES.  `entry t(b,a)'
	# maps incoming 0 to bs slot and incoming 1 to as, which is the
	# IDENTITY REVERSED -- so a relocation that copied slot to slot after a
	# blind spill would put a in both.  Nothing else in Fortran produces a
	# permutation of one frame layout onto itself.
	check 'f77: entries with reordered and disjoint argument lists' \
	    'RAN:  sab 1 2| tba 4 3| ucde 5 6 7|' \
	    "$(f77refuse er '      call s(1,2)
      call t(3,4)
      call u(5,6,7)
      end
      subroutine s(a,b)
      integer a,b,c,d,e
      write(6,*) "sab", a, b
      return
      entry t(b,a)
      write(6,*) "tba", a, b
      return
      entry u(c,d,e)
      write(6,*) "ucde", c, d, e
      return
      end')"
	# AND ENTRIES OF DIFFERENT TYPES, which is what the indirect exit is
	# FOR: proc.c gives each TYPE an epilogue label, every RETURN branches
	# to one common exit, and that exit jumps through an auto naming the
	# right epilogue.  vax.c:466-467 stores it and this file had no such
	# line -- so the exit branched through whatever the frame held, and an
	# INTEGER FUNCTION with an ENTRY printed nothing and exited 0.
	check 'f77: an INTEGER FUNCTION and a REAL ENTRY, each through its own epilogue' \
	    'RAN:  30  4.50000000|' \
	    "$(f77refuse et '      integer f
      real q
      write(6,*) f(3), q(4,5)
      end
      integer function f(n)
      integer n, m, k
      real q
      f = n * 10
      return
      entry q(m,k)
      q = (m + k) / 2.0
      return
      end')"
	# AND THE STRUCTURAL HALF, because a value case cannot say WHY a
	# permutation came out right.  Each entry stub must spill its own x0 to
	# its own slot, and for the reordered pair those two slots are swapped
	# -- derived from the emitted code, so it cannot encode todays offsets.
	check 'f77: each entry stub spills x0 to its own argument slot' 's 0 t 8' \
	    "$(cd f77p1w && printf '%s\n' '      subroutine s(a,b)
      integer a,b
      a = b
      return
      entry t(b,a)
      b = a
      return
      end' > xes.f && rm -f xes.s
	       "$V8ROOT/usr/bin/f77" -S xes.f >/dev/null 2>&1
	       awk '/^_s_:/ { e = "s" } /^_t_:/ { e = "t" }
	            e != "" && /str[[:blank:]]+x0, \[x9, #/ {
	                t = $0; sub(/.*#/, "", t); sub(/\].*/, "", t)
	                printf "%s %s ", e, t; e = "" }
	            END { printf "\n" }' xes.s | sed 's/ *$//')"
	# THE CONCATENATION BOUNDARY IS GONE, AND IT WAS NEVER THE POOL SIZE.
	# f77 builds a concatenation as an ASSIGN PER OPERAND -- a length and a
	# pointer into the frame for each piece, then s_cat over the two arrays
	# -- and doassign() returned the register the value passed through, which
	# nothing pops until the statement ends.  So the pass held 2n registers
	# and used none of them.  Returning the OBJECT ASSIGNED TO instead is the
	# same number by a re-load and frees the register at once.
	# Measured, distinct pool registers: 1/6/8/10/refused before, and a flat
	# 2 at every width after.
	check 'f77: a five-way concatenation, which used to need eleven registers' \
	    'RAN:  11111                                   |' \
	    "$(f77refuse 5 '      character*40 a, g
      a = "1"
      g = a(1:1)//a(1:1)//a(1:1)//a(1:1)//a(1:1)
      write(6,*) g
      end')"
	# AND A TEN-WAY WITH DISTINCT PIECES, because five copies of one
	# character cannot tell a dropped operand from a duplicated one, and
	# cannot see an order the frame arrays got wrong.
	check 'f77: a ten-way concatenation keeps its operands in order' \
	    'RAN:  jaecbgabcdefghij                        |' \
	    "$(f77refuse 10 '      character*40 a, g
      a = "abcdefghij"
      g = a(10:10)//a(1:1)//a(5:5)//a(3:3)//a(2:2)//a(7:7)//
     *    a(1:1)//a(2:2)//a(3:3)//a(4:4)//a(5:5)//a(6:6)//a(7:7)//
     *    a(8:8)//a(9:9)//a(10:10)
      write(6,*) g
      end')"
	# AND THE STRUCTURAL HALF IS A RELATION RATHER THAN A NUMBER: the pass
	# must use no more registers for a wide concatenation than for a narrow
	# one.  A specific count would encode todays allocator; what changed is
	# that the cost stopped growing with the width.
	#
	# The save/restore lines are excluded because prsave() names x19-x28 in
	# its `stp' pairs whatever the body does -- a first draft counted those
	# and reported all ten registers in use at every width, including one.
	check 'f77: concatenation register pressure does not grow with width' 'equal' \
	    "$(cd f77p1w && for n in 2 9; do
	         { echo '      character*80 a, g'
	           echo '      a = "1"'
	           awk -v n="$n" 'BEGIN{ s = "      g = a(1:1)"
	                for (i = 1; i < n; i++) s = s "//a(1:1)"
	                while (length(s) > 66) {
	                  print substr(s, 1, 66); s = "     *" substr(s, 67) }
	                print s }'
	           echo '      write(6,*) g'; echo '      end'; } > w$n.f
	         rm -f w$n.s
	         "$V8ROOT/usr/bin/f77" -S w$n.f >/dev/null 2>&1
	         if [ -s w$n.s ]; then
	           grep -vE 'stp|ldp' w$n.s | grep -oE 'x(19|2[0-8])' | sort -u | wc -l
	         else echo "no-output-$n"; fi
	       done | tr -d ' ' | tr '\012' ' ' |
	       awk '{ if ($1 == $2 && $1 != "" && $1+0 > 0) print "equal"
	              else print "narrow " $1 " wide " $2 }')"

	# ---- THE ARGUMENT BOUND ITSELF, WHICH IS TWO REFUSALS AND NOT ONE.
	# MAXARGSLOT is a property of the frame, so both ends have to state it:
	# /lib/f1 refuses a CALL it cannot place and prolog() refuses a
	# PROCEDURE it cannot spill.  They are separate programs reached by
	# separate inputs -- a call to an external the file does not define
	# reaches only the first -- so each needs its own case, and the two
	# messages are deliberately distinguishable.
	f77wide() {	# $1 = n: a program calling an n-argument subroutine
		awk -v n="$1" '
		function emit(s,   first, ch) {
			first = 1
			while (length(s) > 0) {
				ch = substr(s, 1, 66); s = substr(s, 67)
				printf "%s%s\n", (first ? "      " : "     *"), ch
				first = 0
			}
		}
		BEGIN {
			for (i = 1; i <= n; i++) {
				a = a (i > 1 ? "," : "") "a" i
				v = v (i > 1 ? "+" : "") "a" i
				c = c (i > 1 ? "," : "") i
			}
			emit("subroutine s" n "(" a ")")
			emit("integer " a)
			emit("write(6,*) " v)
			print "      return"
			print "      end"
			emit("call s" n "(" c ")")
			print "      end"
		}'
	}
	f77widecall() {	# $1 = n: a call to an external, so only the CALLER is reached
		awk -v n="$1" '
		function emit(s,   first, ch) {
			first = 1
			while (length(s) > 0) {
				ch = substr(s, 1, 66); s = substr(s, 67)
				printf "%s%s\n", (first ? "      " : "     *"), ch
				first = 0
			}
		}
		BEGIN {
			for (i = 1; i <= n; i++) c = c (i > 1 ? "," : "") i
			emit("call ext(" c ")")
			print "      end"
		}'
	}
	# The boundary is RUN rather than merely compiled, and the expected
	# value is the sum 1..64, which no partial placement can produce: an
	# argument written to the wrong slot changes the total.
	check 'f77: sixty-four argument slots, the frame bound, runs' 'RAN:  2080|' \
	    "$(f77refuse 64 "$(f77wide 64)")"
	check 'f77: a sixty-fifth slot is refused at the callee' \
	    'procedure has 65 argument slots, more than the frame holds' \
	    "$(f77refuse 65 "$(f77wide 65)")"
	check 'f77: a sixty-fifth argument is refused at the caller' \
	    'f1: 65 arguments, more than the frame holds' \
	    "$(cd f77p1w && f77widecall 65 > xw65.f
	       "$V8ROOT/usr/bin/f77" -c xw65.f 2>&1 |
	       grep -oE 'f1: [0-9]+ arguments, more than the frame holds' | head -1)"
	# AND THE TWO SPELLINGS OF THE NUMBER MUST AGREE.  f1.c and f77pass1 are
	# separate programs sharing no header -- including f77's copy would put
	# its whole defs chain on f1's include path -- so the frame constants
	# are stated twice: the same arrangement the opcode numbers have, and
	# checked the same way.  (This said f1.c was compiled by clang.  Both
	# are v8cc's; the Makefile is the authority and CLAUDE.md says to know
	# it before editing f1.c.)  The blank is required twice in the
	# pattern for the reason the opcode case records: with zero blanks and
	# zero digits both allowed, MAXARGSLOT would also match a longer name
	# beginning with it.
	check 'f77: f1.c and arm64defs state the same argument area' 'agree' \
	    "$(d=$ROOT/src/cmd/f77/arm64defs; f=$ROOT/shim/f1/f1.c
	       g() { sed -n "s/^#define $2[[:blank:]][[:blank:]]*\([0-9]*\).*/\1/p" "$1"; }
	       a=$(g "$d" MAXARGSLOT); b=$(g "$f" MAXARGSLOT)
	       c=$(g "$d" SZADDR);     e=$(g "$f" ARGSLOT)
	       if [ -n "$a" ] && [ "$a" = "$b" ] && [ -n "$c" ] && [ "$c" = "$e" ]
	       then echo agree
	       else echo "MAXARGSLOT $a/$b SZADDR $c/$e"; fi)"

	# AND f77refuse's ARM LIST IS CHECKED AGAINST THE SOURCES.  It is a
	# second hand-written copy of a list that lives in arm64.c and f1.c, so
	# it drifts in two directions and neither is loud.  Measured, both had
	# happened: it kept `multiple ENTRY points' for a step after arm64.c
	# stopped emitting it, and it omitted argdest()'s and prolog()'s
	# temporaries bound the whole time.  The sources are the third thing
	# that is neither copy.
	#
	# ONLY THE MISSING-ARM DIRECTION IS CHECKED, and the reason is worth
	# stating rather than leaving as a gap.  A refusal with no arm captures
	# nothing, so the case reports `got []' -- indistinguishable from a
	# compiler that produced no output, which this project has paid for
	# twice.  That direction is exact: every fatal()/fatali() string in the
	# sources, with its %-conversions filled in, must match the alternation.
	# The DEAD-arm direction cannot be written this simply, because an arm
	# is deliberately a FRAGMENT of a message and may spell out a word the
	# message leaves to a %s -- `more integer registers than this pass
	# allocates' against `expression needs more %s registers ...'.  Testing
	# it needs the substitution's possible values, which only the call site
	# knows.  Its failure mode is a line that reads as documentation for a
	# refusal that is gone, which is a worse comment rather than a worse
	# test, so it is left to review.
	check 'f77: every refusal in the sources has an arm in f77refuse' 'ok 1 skipped' \
	    "$(alt=$(sed -n '/^	f77refuse() {/,/^	}$/p' "$SELF" |
	                grep -oE "grep -oE '[^']*'" | sed "s/grep -oE '//; s/'$//")
	       [ -n "$alt" ] || { echo 'no alternation extracted'; exit; }
	       msgs=$(cat "$ROOT/src/cmd/f77/arm64.c" "$ROOT/shim/f1/f1.c" \
	                  "$ROOT/src/cmd/f77/io.c" |
	         grep -ohE '"[^"]*(more than|allocates|needs a|on this target)[^"]*"' |
	         sed -e 's/^"//' -e 's/"$//' -e 's/\\n$//' | sort -u)
	       [ -n "$msgs" ] || { echo 'no messages extracted'; exit; }
	       skip=$(printf '%s\n' "$msgs" | grep -c '%s')
	       miss=$(printf '%s\n' "$msgs" | grep -v '%s' |
	         sed 's/%[a-z]/[0-9]+/g' | while read -r m; do
	           hit=no; OI=$IFS; IFS='|'
	           for a in $alt; do
	             printf '%s\n' "$m" | grep -qF "$a" && hit=yes
	           done
	           IFS=$OI
	           [ "$hit" = yes ] || printf 'no-arm{%s}' "$m"
	         done)
	       [ -z "$miss" ] && echo "ok $skip skipped" || echo "$miss")"

	# ---- THE ASSIGNed FORMAT SPECIFIER, WHICH IS REFUSED RATHER THAN
	# MISCOMPILED.  Fortran 77 lets an ASSIGNed variable be a FORMAT as well
	# as a branch target, and the two need opposite things from the four
	# bytes.  /lib/f1 stores `target - Lf1b<proc>' and P2GOTO adds the base
	# back, so a GOTO never needs the distance to be a pointer; libI77
	# DEREFERENCES the format field, and no consumer adds anything.  So this
	# compiled clean and SIGSEGVd.
	#
	# THE SECOND CASE IS NOT A DUPLICATE -- it is the control that says this
	# is not a source-ORDER bug.  fmtstmt() reallocates the label number when
	# it first learns the label is a FORMAT, so writing the FORMAT statement
	# above the ASSIGN makes the front end pick the right object; measured, it
	# crashed anyway, because the encoding is what breaks it.  A fix that only
	# handled the forward reference would pass the first case and fail this.
	check 'f77: an ASSIGNed FORMAT specifier is refused' 'ASSIGNed FORMAT specifier' \
	    "$(f77refuse afmt '      program afmt
      assign 100 to nf
      write(6,nf) 42
      stop
  100 format(1x,i5)
      end')"
	check 'f77: and refused with the FORMAT statement written first' 'ASSIGNed FORMAT specifier' \
	    "$(f77refuse bfmt '      program bfmt
  100 format(1x,i5)
      assign 100 to nf
      write(6,nf) 42
      stop
      end')"
	# and the control: an ordinary FORMAT label is untouched.  Without it a
	# refusal that fired on every FORMAT would pass both cases above.
	check 'f77: an ordinary FORMAT label still works' '    42|' \
	    "$(f77run ofmt '      program ofmt
      write(6,100) 42
  100 format(1x,i5)
      end')"

	# ---- THE FRAME IS SIZED PER PROCEDURE.  It was SZADDR*MAXARGSLOT for
	# every procedure, so `subroutine nop' carried 512 bytes of spilled-
	# argument area it could not name and 2144 bytes of frame; measured,
	# recursion reached 3899 frames where the pre-ninth-argument compiler
	# reached 6699.
	#
	# THE ASSERTION IS A RELATION AND BOTH NUMBERS ARE READ OUT OF THE
	# EMITTED CODE, because a transcribed frame size is a constant that goes
	# stale the next time the layout moves.  goret unwinds with
	# `add sp, x29, #N', so N is this procedure's frame top by construction.
	check 'f77: the frame is sized per procedure, not to the worst case' 'ok' \
	    "$(cd f77p1w && rm -f xnop.s xw12.s
	       printf '%s\n' '      subroutine xnop' '      end' > xnop.f
	       printf '%s\n' '      subroutine xw12(a,b,c,d,e,f,g,h,i,j,k,l)' \
	           '      integer a,b,c,d,e,f,g,h,i,j,k,l' '      l = a + l' \
	           '      return' '      end' > xw12.f
	       "$V8ROOT/usr/bin/f77" -S xnop.f >/dev/null 2>&1
	       "$V8ROOT/usr/bin/f77" -S xw12.f >/dev/null 2>&1
	       top() { grep -oE 'add[[:blank:]]+sp, x29, #[0-9]+' "$1" |
	               head -1 | grep -oE '[0-9]+$'; }
	       n=$(top xnop.s); w=$(top xw12.s)
	       if [ -z "$n" ] || [ -z "$w" ]; then echo "no epilogue n=[$n] w=[$w]"
	       elif [ "$n" -lt "$w" ]; then echo ok
	       else echo "leaf frame $n is not smaller than the 12-argument frame $w"; fi)"
	# AND THE FLOOR, WHICH IS A DIFFERENT PROPERTY AND FAILS SILENTLY.
	# prsave() spills x0-x7 unconditionally, four stp pairs at x9 = x29 +
	# ARGOFFSET, whatever the procedure declared -- so a frame sized from
	# lastargslot ALONE gives a no-argument procedure a top of x9 exactly,
	# and the spill writes 64 bytes above its own frame, over the caller.
	# The structural case beside this one cannot see it: it reads [x29, #N]
	# references and the spill is written through x9.
	check 'f77: and a leaf frame still covers the unconditional x0-x7 spill' 'ok' \
	    "$(cd f77p1w && rm -f xnop.s
	       printf '%s\n' '      subroutine xnop' '      end' > xnop.f
	       "$V8ROOT/usr/bin/f77" -S xnop.f >/dev/null 2>&1
	       g() { grep -oE "add[[:blank:]]+$1, x29, #[0-9]+" xnop.s |
	             head -1 | grep -oE '[0-9]+$'; }
	       a=$(g x9); n=$(g sp)
	       if [ -z "$a" ] || [ -z "$n" ]; then echo "not found a=[$a] n=[$n]"
	       elif [ "$n" -ge $((a + 64)) ]; then echo ok
	       else echo "frame top $n is below the spill area end $((a + 64))"; fi)"

	# ---- AND THE OUTGOING CALL AREA IS SIZED PER PROCEDURE TOO, which is
	# the other half of the frame and the half with no VAX counterpart at
	# all: AAPCS64 puts a call's ninth and later arguments at [sp,#0], so
	# the CALLER reserves room for them where a VAX simply pushes.  It was
	# SZADDR*(MAXARGSLOT-8) -- 448 bytes -- on every procedure including a
	# leaf that makes no call.  Measured: `subroutine nop' went from 1536
	# bytes of frame to 1088, and leaf recursion on an 8176 KiB stack from
	# 4929 frames to 6699, which is 1696/1248 to four figures.
	#
	# THREE PROPERTIES THAT FAIL DIFFERENTLY, SO THREE CASES.  The count
	# comes from pass 1 -- putpcc.c's putcall() is the only place that
	# knows how wide a call is -- and arm64.c's f77call() keeps the running
	# maximum, resetting on procno.
	#
	# ONE: nothing is reserved by a procedure that makes no call, or one
	# whose widest call fits in x0-x7.  All three areas are read out of
	# `add x29, sp, #C' in each procedure's own entry stub, in source
	# order, and all three procedures are in ONE compilation deliberately:
	# what this discriminates is a maximum left over from the procedure
	# before, which cannot happen across files because each f77pass1 run
	# starts clean.  Names are five characters because F77 truncates an
	# identifier at six and says so, which is not a port defect.
	check 'f77: the outgoing call area is sized per procedure' 'ok' \
	    "$(cd f77p1w && rm -f xthr.s
	       printf '%s\n' '      subroutine xwide(a,b,c,d,e,f,g,h,i,j,k,l)' \
	           '      integer a,b,c,d,e,f,g,h,i,j,k,l' \
	           '      call xsink(a,b,c,d,e,f,g,h,i,j,k,l)' '      end' \
	           '      subroutine xleaf' '      end' \
	           '      subroutine xnarr(a)' '      integer a' \
	           '      call xone(a)' '      end' > xthr.f
	       "$V8ROOT/usr/bin/f77" -S xthr.f >/dev/null 2>&1
	       set -- $(awk '/^Lf1b[0-9]*:/ { if (seen) printf "%s ", area
	                                      seen = 1; area = "none" }
	                     /add[[:blank:]]+x29, sp, #/ {
	                         if (area == "none") { area = $0; sub(/.*#/, "", area) } }
	                     END { if (seen) printf "%s\n", area }' xthr.s)
	       if [ $# -ne 3 ]; then echo "want three areas, got [$*]"
	       elif [ "$1" -gt 0 ] && [ "$2" = 0 ] && [ "$3" = 0 ]; then echo ok
	       else echo "areas [$*], want wide>0 then leaf=0 then narrow=0"; fi)"
	# TWO: and it is big enough, which is the half that corrupts the
	# CALLEE'S stack rather than merely wasting space.  /lib/f1 stages the
	# ninth argument onward through `str x11, [sp, #K]' and that is the
	# only variable sp-relative store either half of the compiler emits --
	# arm64.c's are all [sp, #-16]! and [sp], #16 -- so K + SZADDR <= C is
	# exact, with both numbers read back out of the emitted code.  A
	# fourteen-argument call is six slots past x0-x7, and requiring K to be
	# found is what stops the case passing on a program that never stores.
	check 'f77: and the outgoing area covers every store into it' 'ok' \
	    "$(cd f77p1w && rm -f xw14.s
	       printf '%s\n' '      subroutine xw14(a,b,c,d,e,f,g,h,i,j,k,l,m,n)' \
	           '      integer a,b,c,d,e,f,g,h,i,j,k,l,m,n' \
	           '      call xs14(a,b,c,d,e,f,g,h,i,j,k,l,m,n)' '      end' > xw14.f
	       "$V8ROOT/usr/bin/f77" -S xw14.f >/dev/null 2>&1
	       c=$(grep -oE 'add[[:blank:]]+x29, sp, #[0-9]+' xw14.s |
	           head -1 | grep -oE '[0-9]+$')
	       k=$(grep -oE 'sp, #[0-9]+\]' xw14.s | grep -oE '[0-9]+' |
	           sort -n | tail -1)
	       if [ -z "$c" ] || [ -z "$k" ]; then echo "not found c=[$c] k=[$k]"
	       elif [ $((k + 8)) -le "$c" ]; then echo ok
	       else echo "stores reach $((k + 8)), area is $c"; fi)"
	# THREE: AND EVERY ENTRY POINT OF ONE PROCEDURE MUST AGREE ABOUT IT,
	# which is why the reset is keyed on procno and not done in prsave().
	# proc.c:333-334 calls prolog() once per ENTRY and prolog() calls
	# prsave(), so a reset there leaves the second entry with a different
	# frame from the first -- and both stubs branch into ONE body, which
	# stores its outgoing arguments at one set of sp-relative addresses.
	# Requiring two stubs and a non-zero area is what keeps the case from
	# passing on a procedure that has neither.
	check 'f77: and every ENTRY point of a procedure agrees about it' 'ok' \
	    "$(cd f77p1w && rm -f xent.s
	       printf '%s\n' '      subroutine xent1(a,b)' '      integer a,b' \
	           '      call xz2(a,b)' '      return' \
	           '      entry xent2(b,a)' \
	           '      call xz12(a,b,a,b,a,b,a,b,a,b,a,b)' '      end' > xent.f
	       "$V8ROOT/usr/bin/f77" -S xent.f >/dev/null 2>&1
	       n=$(grep -cE 'add[[:blank:]]+x29, sp, #[0-9]+' xent.s)
	       u=$(grep -oE 'add[[:blank:]]+x29, sp, #[0-9]+' xent.s |
	           grep -oE '[0-9]+$' | sort -u | tr '\n' ' ')
	       set -- $u
	       if [ "$n" -ne 2 ]; then echo "want two entry stubs, got $n"
	       elif [ $# -ne 1 ]; then echo "the two entries disagree: [$u]"
	       elif [ "$1" -le 0 ]; then echo "both entries reserve nothing"
	       else echo ok; fi)"

	# ---- ADJUSTABLE DIMENSIONS, WHICH prolog() HAD NEVER EVALUATED.
	# proc.c:1120 allocates a temporary per run-time dimension and leaves
	# the machine file to store the expression into it; arm64.c had no such
	# loop, so the temporary was read and never written.  pdp11.c:378-388
	# is the loop, and this file transcribed only its last two statements.
	#
	# TWO CASES BECAUSE THERE ARE TWO STORES AND THEY FAIL DIFFERENTLY.  A
	# missing dimsize store faults -- an extent read out of a stale frame
	# slot -- and a missing baseoffset store does not: it gives exit 0 and
	# a plausible wrong number, and is right by coincidence for some
	# bounds, so only a case that names the values can see it.  A 1-D
	# adjustable array is correct without either, because nothing
	# multiplies by the first extent; that is why a twenty-program corpus
	# walked past this.
	check 'f77: a 2-D adjustable dummy array addresses correctly' \
	    '11 21 31 12 22 32 13 23 33 14 24 34' \
	    "$(cd f77p1w && { echo '      program main'
	         echo '      integer q(3,4)'
	         echo '      integer i, j'
	         echo '      do 10 j = 1, 4'
	         echo '        do 20 i = 1, 3'
	         echo '          q(i,j) = i*10 + j'
	         echo '   20   continue'
	         echo '   10 continue'
	         echo '      call show(q,3,4)'
	         echo '      end'
	         echo '      subroutine show(a,m,n)'
	         echo '      integer m, n'
	         echo '      integer a(m,n)'
	         echo '      integer i, j'
	         echo '      do 30 j = 1, n'
	         echo '        do 40 i = 1, m'
	         echo '          write(6,*) a(i,j)'
	         echo '   40   continue'
	         echo '   30 continue'
	         echo '      end'; } > xdim.f
	       rm -f xdim
	       "$V8ROOT/usr/bin/f77" xdim.f -o xdim >/dev/null 2>&1
	       [ -x xdim ] && ./xdim 2>&1 | tr -d ' ' | tr '\012' ' ' |
	         sed 's/ $//' || echo 'did not build')"

	check 'f77: an adjustable LOWER bound offsets correctly' '33 44' \
	    "$(cd f77p1w && { echo '      program main'
	         echo '      integer q(10)'
	         echo '      integer i'
	         echo '      do 10 i = 1, 10'
	         echo '        q(i) = i*11'
	         echo '   10 continue'
	         echo '      call lo(q(3),3)'
	         echo '      end'
	         echo '      subroutine lo(a,k)'
	         echo '      integer k'
	         echo '      integer a(k:10)'
	         echo '      write(6,*) a(k), a(k+1)'
	         echo '      end'; } > xlb.f
	       rm -f xlb
	       "$V8ROOT/usr/bin/f77" xlb.f -o xlb >/dev/null 2>&1
	       [ -x xlb ] && ./xlb 2>&1 | tr -s ' ' | sed 's/^ //;s/ $//' ||
	         echo 'did not build')"

	# ---- AN ENTRY'S HIDDEN RESULT SLOT IS ITS OWN TYPE'S, NOT THE
	# PROCEDURE'S.  argdest() keyed off the global `proctype', which is set
	# from the FIRST entry (proc.c:173), while doentry() -- which allocated
	# the slots argdest maps onto -- reads each entry's own (proc.c:367).
	# Only COMPLEX-ness can differ: proc.c:372-380 refuses a CHARACTER
	# entry of a non-CHARACTER function outright, so the TYCHAR arm agrees
	# identically and is a correctness no-op.
	#
	# THE CASE MUST NAME THE VALUES, because the broken form exits 0.  It
	# crossed two operands -- the hidden result pointer went into y's slot
	# and the real &y was dropped -- and printed a pair of denormals.
	check 'f77: a COMPLEX ENTRY of a REAL function gets its own slots' \
	    '6.00000000 8.00000000' \
	    "$(cd f77p1w && { echo '      program main'
	         echo '      complex r, g'
	         echo '      real f'
	         echo '      complex y'
	         echo '      r = g((3.0,4.0))'
	         echo '      write(6,*) real(r), aimag(r)'
	         echo '      end'
	         echo '      real function f(x)'
	         echo '      real x'
	         echo '      complex g, y'
	         echo '      f = x'
	         echo '      return'
	         echo '      entry g(y)'
	         echo '      g = y + y'
	         echo '      return'
	         echo '      end'; } > xcx.f
	       rm -f xcx
	       "$V8ROOT/usr/bin/f77" xcx.f -o xcx >/dev/null 2>&1
	       [ -x xcx ] && ./xcx 2>&1 | tr -s ' ' | sed 's/^ //;s/ $//' ||
	         echo 'did not build')"

	# ---- A V_MEM's REGISTER IS A BASE, SO ITS POOL IS THE INTEGER ONE
	# WHATEVER IT POINTS AT.  vfree() chose the pool from `vtype', so a
	# float-typed V_MEM sent its integer base to rfree(1, ...), which
	# searches fpool, does not find it -- the pools are disjoint by
	# register number -- and returns silently, because rfree has no
	# not-found arm.  One register leaked per subscripted REAL reference,
	# so ten of them exhausted the ten-entry pool and f1 REFUSED A LEGAL
	# PROGRAM.  Thirty terms is three times the pool, and the INTEGER
	# control is what says the leak is on the float path.
	#
	# THE SUBSCRIPT MUST BE A VARIABLE, which a first draft got wrong and
	# only mutation said so: with a CONSTANT subscript f1 addresses the
	# element as an OREG at a constant offset and materialises no base at
	# all, so there is no owned V_MEM to mis-free and the case passed
	# against the leak it was written for.
	#
	# AND `i' IS SET EXPLICITLY rather than left where the DO loop put it,
	# which is 41 -- a(41) is past the end of a(40), and an out-of-bounds
	# read would have made the answer a property of the frame rather than
	# of the pool.  The terms are a(1)..a(30), so the sum is 465.
	check 'f77: many subscripted REAL references do not exhaust the pool' \
	    'real 465 int 465' \
	    "$(cd f77p1w && for t in real int; do
	         { echo '      program main'
	           if [ $t = real ]; then echo '      real a(40), s'
	           else echo '      integer a(40), s'; fi
	           echo '      integer i'
	           echo '      do 10 i = 1, 40'
	           echo '        a(i) = i'
	           echo '   10 continue'
	           echo '      i = 1'
	           awk 'BEGIN{ s = "      s = a(i)"
	                for (i = 1; i <= 29; i++) s = s " + a(i+" i ")"
	                while (length(s) > 66) {
	                  print substr(s, 1, 66); s = "     *" substr(s, 67) }
	                print s }'
	           echo '      write(6,*) s'; echo '      end'; } > xp$t.f
	         rm -f xp$t
	         "$V8ROOT/usr/bin/f77" xp$t.f -o xp$t >/dev/null 2>&1
	         if [ -x xp$t ]; then
	           printf '%s %d ' "$t" "$(./xp$t 2>&1 |
	             awk '{ printf "%d", $1 + 0.5 }')"
	         else printf '%s refused ' "$t"; fi
	       done | sed 's/ $//')"

	# ---- AND AN ASSIGNed LABEL NEEDS A FOUR-BYTE INTEGER.  exec.c gates
	# ASSIGN on MSKINT, which admits TYSHORT, so `integer*2 lbl' reached
	# f1 and the store was `strh' -- sixteen bits of a thirty-two-bit
	# distance, read back through `ldrsh'.  It is right until the procedure
	# grows: measured, 28880 bytes away it printed the right answer and
	# 108080 bytes away it SIGSEGV'd, with no diagnostic at either end.
	#
	# THE PAIR IS THE GUARD.  A refusal alone passes against an f1 that
	# refuses every ASSIGN, so the plain INTEGER control is what says the
	# discriminator is the width.
	check 'f77: an INTEGER*2 ASSIGN is refused' \
	    'an ASSIGNed label needs a 4-byte INTEGER' \
	    "$(f77refuse i2 '      program main
      integer*2 lbl
      assign 20 to lbl
      goto lbl
   20 write(6,*) 1
      end')"
	check 'f77: a plain INTEGER ASSIGN still branches' 'RAN:  1|' \
	    "$(f77refuse i4 '      program main
      integer lbl
      assign 20 to lbl
      goto lbl
   20 write(6,*) 1
      end')"

	# THE HONEST REPORT THAT STAGE 3 IS ABSENT.  Asserted so that f77pass1
	# arriving is a decision rather than a discovery -- the same reason
	# tests/kmemu asserts that w(1) says `No mem'.

else
	# DERIVED, NOT TRANSCRIBED.  This number stands for the cases the block
	# above would have run, so that a missing prerequisite costs the same as
	# a failure -- and a hand-maintained copy of a count has never once been
	# right here: it read 61 against 67, then 68/73, then 69/74, then 75/79,
	# drifting silently every time a case was added.  Counting the `check'
	# lines between the `if' and this `else' in $0 is the same discipline as
	# deriving the command count from the rootfs rather than from prose.
	n=$(awk '/^if \[ -x "\$V8ROOT\/usr\/bin\/f77" \]; then$/ { i = 1; next }
	         i && /^else$/ { exit }
	         i && /^	check / { c++ }
	         END { print c+0 }' "$SELF")
	[ "$n" -gt 0 ] || n=1	# a zero here would silently report nothing
	fail=$((fail+n)); echo "FAIL f77 driver is not installed ($n cases)"
fi

# The machine description the build GENERATES, which is upstream's own mechanism
# -- its makefile has `machdefs : vaxdefs / cp vaxdefs machdefs' -- and the
# discriminator is SZADDR, because a pointer is the one thing on this target that
# is not the VAX's width.  Both directions, so neither case can be vacuous: the
# source directory still holds upstream's machdefs, and it must NOT be the one
# the compile opened.
check 'f77: the build generated an arm64 machdefs (SZADDR 8)' '8' \
    "$(sed -n 's/^#define[[:blank:]]*SZADDR[[:blank:]]*\([0-9]*\).*/\1/p' \
         "$ROOT/build/stage0/f77/machdefs")"
check 'f77: and the source directory still has upstream(s) (SZADDR 4)' '4' \
    "$(sed -n 's/^#define[[:blank:]]*SZADDR[[:blank:]]*\([0-9]*\).*/\1/p' \
         "$ROOT/src/cmd/f77/machdefs")"
# SZLONG IS 4 AND IT IS PINNED BY THE FLOAT LAYOUT, which is the opposite of what
# LP64 suggests and the single most consequential number in the f77 port.
# typesize[] -- at driver.c:1032 and again identically at init.c:45 -- makes
# typesize[TYREAL] SZLONG and typesize[TYDREAL] 2*SZLONG, and libF77's r_nint
# takes `float *' while d_nint takes `double *'.  So REAL is 4 and DOUBLE
# PRECISION is 8, and with ftypes offering only TYSHORT and TYLONG that fixes
# Fortran's INTEGER and hidden character length at 4 as well.  See task #12.
check 'f77: SZLONG is 4, pinned by TYREAL and not by Cs long' '4' \
    "$(sed -n 's/^#define[[:blank:]]*SZLONG[[:blank:]]*\([0-9]*\).*/\1/p' \
         "$ROOT/build/stage0/f77/machdefs")"

# THE FORTRAN ABI WIDTH, CHECKED AGAINST f77's OWN MACHINE DESCRIPTION.
#
# This is the guard for task #12, and it is a relation between THREE things, none
# transcribed: what the probe's sizeof() reports (the C side of the convention),
# what the built arm64defs says SZLENG is (f77's side), and the fact that both
# must be 4.  Two copies of a number agreeing is the trap this tree keeps
# finding, so the third is f77's own machdefs read off disk.
#
# It is 4 because typesize[TYREAL] is SZLONG and libF77's r_nint takes `float *',
# and because lengtype() at proc.c:951 hardcodes INTEGER*4 -> TYLONG.  V8's own
# compiler had C's long at 32 bits (`# define NOLONG', ccom/vax/macdefs.h:20),
# which is why libI77's fio.h spelled ftnlen `long' and was right to.
f77szleng=$(sed -n 's/^#define[[:blank:]]*SZLONG[[:blank:]]*\([0-9]*\).*/\1/p' \
             "$ROOT/build/stage0/f77/machdefs")
check 'f77 runtime: ftnlen and ftnint match f77s own SZLENG' "ftnlen $f77szleng ftnint $f77szleng" \
    "$(cd f77dw 2>/dev/null && ./prog 2>/dev/null | sed -n 's/^\(ftnlen .*\)/\1/p')"

# ecvt.o is the one object given an -I, and what it must NOT have picked up is
# libI77's own values.h -- which has three machine arms (u3b, vax, gcos) and no
# arm64, so _DEXPLEN and _HIDDENBIT would be undefined.  The patched copy is
# src/include/values.h.  Asserting the ARM rather than the compile: a build
# that read the wrong header does not link, so the compile is already checked
# by the case above; what is worth stating is that our arm is the one present.
check 'values.h: this port has an arm64 arm, and it is IEEE' '11 1 1' \
    "$(sed -n '/^#if arm64/,/^#endif/p' "$ROOT/src/include/values.h" |
       sed -n 's/^#define[[:blank:]]*_DEXPLEN[[:blank:]]*\([0-9]*\).*/\1/p;
               s/^#define[[:blank:]]*_HIDDENBIT[[:blank:]]*\([0-9]*\).*/\1/p;
               s/^#define[[:blank:]]*_IEEE[[:blank:]]*\([0-9]*\).*/\1/p' |
       sort -rn | tr '\n' ' ' | sed 's/ $//')"

echo "wavea: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

# ---------------------------------------------------------------------------
# SOLVED, kept because the search was long and the shape of it is the point:
#
#   `cat nosuchfile` printed "cat: input nosuchfile is output" -- its dev/ino
#   guard -- instead of a perror message.
#
#   Six hypotheses were eliminated by direct test, each correct in isolation:
#   argc, fflg, `**++argv`, the `&&`/`||` lowering on cat's exact expression,
#   open()'s return value and errno, and assignment-in-condition against -1 in
#   both `int` and `register int`.
#
#   What finally settled it was instrumenting cat's own decision points instead
#   of writing a seventh test case:
#
#       DBG fi=-1
#       DBG fstat=-1 dev=15 ino=23002 sd=15 si=23002
#       cat: input nosuchfile is output
#
#   open() returned -1 and `fi < 0` was FALSE; fstat() returned -1 and `>= 0`
#   was TRUE. Not one bug in cat's argument loop -- every signed comparison
#   against a syscall result was inverted, and statb still held stdout's stat
#   from the top of main, so the dev/ino guard matched.
#
#   Cause: AAPCS64 defines only w0 for a function returning int; the top half of
#   x0 is unspecified. This back end computes at 64-bit width and compares with
#   an x-form `cmp`, so -1 arriving as 0x00000000ffffffff tested as positive.
#   The whole shim is clang-compiled, so EVERY syscall error check in EVERY V8
#   program was broken -- and could not show up in a self-contained test,
#   because v8cc's own callees return a properly extended x0 and only disagree
#   with foreign ones.
#
#   The first fix -- sign-extend at the call site -- was WRONG, and instructive
#   about why. V8 calls malloc undeclared (opendir.c: `(DIR *)malloc(...)` with
#   nothing in scope), so K&R types the call int, and sign-extending it from 32
#   bits truncated the pointer: opendir went from working to segfaulting. The
#   compiler cannot tell a declared int from an undeclared one, because they are
#   the same node. So the seam adapts instead -- every wrapper in
#   shim/v8sys/stubs.c returns long -- and the compiler narrows only types that
#   must have been declared. The two halves are documented together, in
#   stubs.c and in gencall().
#
#   Worth remembering: the recorded next step was to read the generated assembly
#   and diff it against clang's. Instrumenting the program was faster and
#   pointed straight at the seam.

# ---------------------------------------------------------------------------
# tr and cmp: both were listed here as broken, and neither turned out to be a
# bug in the program.
#
#   `tr -d b` took a SIGBUS.  V8's nextc() ends with `if(c==0) *--s->p = 0;`,
#   pushing the NUL back after reading past the end, and with one argument the
#   other string is the literal "".  String literals were being emitted into
#   __TEXT,__cstring; V8's own VAX back end puts them in `.data 1`/`.data 2`
#   (src/cmd/ccom/vax/lcatch2.c), because 1985 C had no `const` and programs
#   wrote to literals freely.  They now go to writable data here too.
#
#   `cmp` did not compile, then wanted _ctype.  The table is authentic V8 --
#   libc/gen/ctype.c -- and was simply not imported yet.
#
# The general shape again: neither was a defect in the ported program.  One was
# a target-model decision made wrongly, one was a missing library file.
# ---------------------------------------------------------------------------


# ---------------------------------------------------------------------------
# NOT PORTED, and why. Compiling every command in usr/src/cmd leaves seven that
# do not build and two that do not link; none of them is a compiler defect.
#
#   512restor, fsck  filesystem repair on a raw device. PLAN.md S1 rule 5 puts
#                    the V8 filesystem out of scope, and these read disk
#                    structures the shim does not present.
#   ld               embeds VAX assembly: `movc3 r8,(r11),(r7)`. The host link
#                    editor is used by design (S1 rule 2).
#   fstat            a /dev/kmem groveler, and its own `long getw()` collides
#                    with stdio's. Phase 4.
#   mc               a jerq (Blit) client. Phase 5.
#   tcat             `char *asctab[128] { ... }` -- an initialiser with no `=`,
#                    which V8's own grammar rejects too. An upstream defect, not
#                    a porting one.
#   init, stty       want tty_ld and ntty_ld, the kernel's line-discipline
#                    numbers. Genuinely kernel state; a case-by-case decision
#                    under S7 rather than something to emulate.
#
# ls(1) needed one source change, marked in src/cmd/ls.c: it includes
# "/usr/jerq/include/jioctl.h" by absolute path, which is the one form of
# include no -I can redirect. The header itself is unchanged.
# ---------------------------------------------------------------------------
