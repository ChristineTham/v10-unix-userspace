#!/bin/sh
# Wave A: real V8 commands, compiled by v8cc, linked freestanding, run.
#
# These are authentic V8 sources from src/cmd, not test fixtures. Every compiler
# bug found so far has lived in combinations of features that real code uses and
# synthetic tests do not, so this suite leads with real programs by design.

ROOT=$(cd "$(dirname "$0")/../.." && pwd)	# the release tree, v8/
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
		for (i = 1; i <= NR; i++) {
			s = line[i]
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
				found = 0
				for (k = 2; k < nf; k++) if (f[k] == p) found = 1
				if (!found) continue
				d = f[nf]
			} else continue
			sub("/" p "$", "", d); sub(/^\//, "", d)
			if (d != "") { print d; exit }
		} }
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
	# struct's exemption is GONE: it is built and installed now, so the
	# guard verifies it like anything else.  A skip left behind after its
	# reason expires is a hole, and this list is a set of claims.
	#
	# csh IS BUILT AND DELIBERATELY NOT INSTALLED, which is a narrower
	# claim than the one that stood here before.  It compiles (19 objects),
	# links with an EMPTY nm -u, and runs the whole language -- @ arithmetic,
	# foreach, while, switch, globbing, if -e.  What it cannot do is finish
	# an external command: the output is correct and then it waits forever.
	# Sampled, the stack is exact and names one function:
	#
	#   process -> execute -> pwait -> pjwait -> sigpause -> sigsys
	#           -> v8s_sigsys -> v8s_sigpause_wait -> sigsuspend
	#
	# so pjwait blocks for a SIGCHLD that never wakes it.  Two candidate
	# causes were measured and are NOT it -- an unblock/suspend race (the
	# hang is deterministic, 12 of 12) and an invalid sigprocmask `how' of 0
	# (a real bug, fixed, no change).  src/cmd/csh/PORTING.md, task #93.
	csh) continue ;;
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
check 'ex vi view edit are one inode' '1' \
    "$(cd "$V8ROOT/usr/bin" && ls -i ex vi view edit 2>/dev/null |
       awk '{print $1}' | sort -u | wc -l | tr -d ' ')"

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
check 'pcat and unpack are one inode' '1' \
    "$(cd "$V8ROOT/usr/bin" && ls -i pcat unpack 2>/dev/null |
       awk '{print $1}' | sort -u | wc -l | tr -d ' ')"

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
today=$(date '+%b %-d'); far=$(date -v+95d '+%b %-d' 2>/dev/null || echo 'Xxx 1')
"$V8ROOT/usr/lib/calendar2" </dev/null > cal.pat 2>&1
check 'calendar2 matches today' 'yes' \
    "$(printf '%s something\n' "$today" | grep -Ei -f cal.pat >/dev/null && echo yes || echo no)"
check 'and not a date months away' 'no' \
    "$(printf '%s something\n' "$far" | grep -Ei -f cal.pat >/dev/null && echo yes || echo no)"
# calendar4 filters a list of paths down to the readable-but-not-writable ones,
# which is the access(2) pair.  The negative half is what discriminates.
check 'calendar4 keeps a readable file' '/etc/passwd' \
    "$(printf '/etc/passwd\n/nonexistent\n' | "$V8ROOT/usr/lib/calendar4" 2>&1)"

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
