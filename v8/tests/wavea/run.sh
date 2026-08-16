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
mkdest() {	# where a program's OWN makefile installs it, or nothing
	_m=$(ls "$ROOT/src/cmd/$1"/[Mm]akefile 2>/dev/null | head -1)
	[ -n "$_m" ] || return 1
	awk -v p="$1" '
	    { line[NR] = $0 }
	    /^[A-Za-z_][A-Za-z0-9_]*[ \t]*=/ {
		n = $0; sub(/[ \t]*=.*/, "", n); gsub(/[ \t]/, "", n)
		v = $0; sub(/^[^=]*=[ \t]*/, "", v); sub(/[ \t]*$/, "", v)
		var[n] = v }
	    END {
		for (i = 1; i <= NR; i++) {
			s = line[i]
			gsub(/\$\(DESTDIR\)/, "", s)
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
			} else if (nf >= 3 && (f[1] == "mv" || f[1] == "cp") &&
				   f[2] == p) {
				d = f[3]
			} else continue
			sub("/" p "$", "", d); sub(/^\//, "", d)
			if (d != "") { print d; exit }
		} }
	' "$_m"
}
mkdiffer= mkseen=
for d in "$ROOT"/src/cmd/*/; do
	name=$(basename "$d")
	want=$(mkdest "$name"); [ -n "$want" ] || continue
	mkseen="$mkseen $name"
	got=$(adest "$name")
	[ "$want" = "$got" ] || mkdiffer="$mkdiffer $name($want,dest=$got)"
done
# The sweep has to have found them, or the disagreement set is empty for the
# wrong reason -- tests/cpp's failure exactly.
if [ "$(echo $mkseen | wc -w | tr -d ' ')" -ge 10 ]; then pass=$((pass+1))
else fail=$((fail+1)); echo "FAIL only $(echo $mkseen | wc -w) makefiles state a destination:$mkseen"; fi
# TWO, and BOTH are programs that appear in no table at all, so Admin/dest is
# answering by fall-through in each -- which is what makes the third source
# worth consulting rather than a curiosity.  `cpp' is the one that shows the
# fall-through is simply wrong: its makefile says /lib, the SHIPPED TREE says
# /lib, and dest says /usr/bin.  This port already puts cpp in /lib, but by
# accident rather than by derivation -- it is a toolchain target with its own
# rule and never goes through $(call v8dest,...).  dump had no such accident
# available, so the Makefile's $(MKFILEETC) follows the makefile deliberately.
check 'an imported makefile and Admin/dest disagree about exactly these two' \
   ' cpp(lib,dest=usr/bin) dump(etc,dest=usr/bin)' "$mkdiffer"
check 'and for cpp the shipped tree sides with the makefile' 'lib' "$(shipfile cpp)"
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
# Paired: -b must still take its number, create, list and extract.
mkdir -p tarx && printf 'hello\n' > tf1 && printf 'world\n' > tf2
"$(v8which tar)" cbf 2 tar.a tf1 tf2 >/dev/null 2>&1
check 'tar cbf 2 still creates an archive with that blocksize' 'tf1 tf2' \
    "$("$(v8which tar)" tbf 2 tar.a 2>/dev/null | tr '\n' ' ' | sed 's/ *$//')"
check 'and it still extracts what went in' 'hello world' \
    "$(cd tarx && "$(v8which tar)" xbf 2 ../tar.a >/dev/null 2>&1; \
       cat tf1 tf2 2>/dev/null | tr '\n' ' ' | sed 's/ *$//')"

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

# THE FLOOR, DERIVED RATHER THAN TRANSCRIBED -- one perl over 4 x 53
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
# The expected value is a LIST, not a count, so a fix that broke one program
# and repaired another cannot cancel out.  cpio -i is deliberately NOT fixed:
# chgreel()'s fopen("/dev/tty") is unchecked and the fgets that follows is a
# getc, which is `--(p)->_cnt' -- a WRITE to virtual 0, and V8 binaries are
# ZMAGIC whose text is read-only shared, so a VAX faulted there too.  Third
# member of that family after pr.c's Ttyin and troff/hc.c's rcf, both of which
# S1 also leaves alone.  src/cmd/cpio.PORTING.md has the measurement.
mkdir -p a0sweep
check 'the only surviving crash in the four is the one we chose to keep' \
    'cpio -i' \
    "$(cd a0sweep && perl -e '
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
			push @bad, $n . ($o eq "" ? " (bare)" : " $o");
		}
	}
	print join(", ", @bad);
    ' "$(v8which cb)" "$DIFFH" "$(v8which tar)" "$(v8which cpio)")"

# AND THE WHOLE-TREE FLOOR IS IN CI NOW, which is the other half of the same
# lesson -- but a 13-minute job is a slow way to learn that its expectation
# file is malformed.  These three cases cost milliseconds and catch the ways
# tests/crash-probe.floor can be wrong without any program misbehaving.
FLOORF=$ROOT/tests/crash-probe.floor
check 'the crash-probe floor file exists' 'yes' \
    "$([ -f "$FLOORF" ] && echo yes || echo no)"
# Every entry is "<signal> <program> ...".  A malformed line would be compared
# literally against the probe's output and could never match, so the failure
# would be a 13-minute CI run reporting a phantom regression.
check 'every floor entry is <signal> <label>' '0' \
    "$(grep -v '^[[:blank:]]*#' "$FLOORF" 2>/dev/null | grep -v '^[[:blank:]]*$' |
       grep -cvE '^[0-9]+ [a-zA-Z0-9_.-]+( .*)?$')"
# AND EVERY PROGRAM IT NAMES MUST STILL BE INSTALLED.  A floor naming a
# program that has been removed can never be satisfied -- the probe would
# report it "gone" forever -- and that is a stale-allow-list failure, the
# shape tests/kmemu's import list is kept honest against.
check 'every program named in the floor is installed' '' \
    "$(grep -v '^[[:blank:]]*#' "$FLOORF" 2>/dev/null | grep -v '^[[:blank:]]*$' |
       awk '{print $2}' | sort -u | while read -r p; do
           find "$V8ROOT/bin" "$V8ROOT/usr/bin" "$V8ROOT/etc" "$V8ROOT/lib" \
                "$V8ROOT/usr/lib" -type f -name "$p" -perm -100 2>/dev/null |
               grep -q . || printf '%s ' "$p"
       done | sed 's/ *$//')"

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
