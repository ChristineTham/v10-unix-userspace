#!/bin/sh
# Wave A: real V8 commands, compiled by v8cc, linked freestanding, run.
#
# These are authentic V8 sources from src/cmd, not test fixtures. Every compiler
# bug found so far has lived in combinations of features that real code uses and
# synthetic tests do not, so this suite leads with real programs by design.

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
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
build() {
	if [ -x "$V8ROOT/bin/$1" ]; then
		cp "$V8ROOT/bin/$1" "$TMP/$1" || return 1
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
try pwd    'pwd'             "$(/bin/pwd -P)" "./pwd"

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
# /etc is checked as well as /bin, and only since mkfs.  V8 splits its commands
# by manual section and keeps section 8 -- the ones that operate on a
# filesystem or a machine rather than on files -- in /etc; mkfs(8)'s own
# synopsis says `/etc/mkfs'.  Every single-file command imported before it
# happened to be a /bin command, which is why this said `bin' alone.  It is a
# widening of WHERE, not of WHETHER: the case below pins mkfs to /etc
# specifically, so the pair still says something a plain `find' would not.
missing=
for src in "$ROOT"/src/cmd/*.c; do
	name=$(basename "$src" .c)
	[ "$name" = cc ] && continue
	[ -x "$V8ROOT/bin/$name" ] || [ -x "$V8ROOT/etc/$name" ] ||
		missing="$missing $name"
done
if [ -z "$missing" ]; then
	pass=$((pass+1))
else
	fail=$((fail+1))
	echo "FAIL these src/cmd commands are built but not installed:$missing"
fi

# ...and the section-8 ones are in /etc rather than /bin, which is the half of
# the statement the widened check above can no longer make on its own.
if [ -x "$V8ROOT/etc/mkfs" ] && [ ! -e "$V8ROOT/bin/mkfs" ]; then
	pass=$((pass+1))
else
	fail=$((fail+1))
	echo "FAIL mkfs(8) belongs in /etc and not /bin"
fi

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
    "$("$V8ROOT/bin/cal" 2 1985 | head -1 | sed 's/^ *//;s/ *$//')"
check 'installed vis runs'   'a\001' "$(printf 'a\001\n' | "$V8ROOT/bin/vis")"
check 'installed ascii runs' '1'     "$("$V8ROOT/bin/ascii" | grep -c 'nul')"
# bcd and morse read STDIN, not argv.
check 'installed bcd punches a card' '1' \
    "$(echo AB | "$V8ROOT/bin/bcd" | grep -c '^/AB')"
check 'installed morse runs' '1' "$(echo e | "$V8ROOT/bin/morse" | grep -c 'dit')"
# units reads /usr/lib/units, and answers "no table" without it.  1 inch is
# 2.54 cm, so this checks the table was actually consulted rather than opened.
check 'installed units converts' '1' \
    "$(printf 'inch\ncm\n' | "$V8ROOT/bin/units" 2>&1 | grep -c '2\.54')"
# ptx reads /usr/lib/eign (the "ignore" word list) and emits one .xx line per
# rotation.  Without the file it exits with "Cannot open  file /usr/lib/eign".
check 'installed ptx permutes' '3' \
    "$(printf 'alpha beta gamma\n' | "$V8ROOT/bin/ptx" | grep -c '^\.xx')"

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
    "$(printf 'a\nb\nc\n' | "$V8ROOT/bin/sed" -n '2p')"
check 'installed sed substitutes'    'xbc' \
    "$(printf 'abc\n' | "$V8ROOT/bin/sed" 's/a/x/')"

# `sed -n l' ON A HIGH BYTE, which crashed. char is signed, so a byte >= 0200 is
# negative, `if(*p1 >= 040)' sent it down the control-character arm, and
# `trans[*p1]' indexed a 32-entry array of POINTERS with a negative subscript.
# The out-of-bounds read is upstream's; LP64 doubled the stride and Mach-O maps
# nothing there, so the VAX printed nonsense and this faulted. Newly reachable
# too -- the byte that does it is any UTF-8 continuation. src/cmd/sed/PORTING.md.
check 'sed -n l survives a UTF-8 byte' 'éx' \
    "$(printf '\303\251x\n' | "$V8ROOT/bin/sed" -n l)"
utf8rc=$(printf '\303\251x\n' | "$V8ROOT/bin/sed" -n l >/dev/null 2>&1; echo $?)
check '...and exits cleanly rather than crashing' '0' "$utf8rc"
# ...while the control characters it DOES have escapes for still get them: the
# fix must not have sent 0..037 down the printable arm as well. Asserted as a
# property rather than a literal, because trans[011] is ">\\t" -- a prefix
# character and then the escape -- and hard-coding that spelling tests the table
# rather than the branch. What matters is that no raw tab reaches the output.
tabout=$(printf 'a\tb\n' | "$V8ROOT/bin/sed" -n l)
case "$tabout" in
*'\t'*) pass=$((pass+1)) ;;
*) fail=$((fail+1)); echo "FAIL sed -n l did not escape a tab: [$tabout]" ;;
esac
# ...and no RAW tab survives. od -c renders a real tab as the two adjacent
# characters \t, while the escape sed emits is a backslash and a t with od's
# column padding between them -- so a match here means the byte came through
# unescaped, which is the opposite of what the first case asks.
if printf 'a\tb\n' | "$V8ROOT/bin/sed" -n l | od -An -c | grep -q '\\t'; then
	fail=$((fail+1)); echo "FAIL sed -n l let a raw tab through"
else
	pass=$((pass+1))
fi
# tsort is a real algorithm, so a cycle-free order is a real answer.
check 'installed tsort orders'  'a b c' \
    "$(printf 'a b\nb c\n' | "$V8ROOT/bin/tsort" | tr '\n' ' ' | sed 's/ $//')"
# factor echoes the number, then each factor on its own indented line, then a
# blank one -- so the factors are lines 2..n-1 with the indentation stripped.
check 'installed factor factors' '7 13' \
    "$("$V8ROOT/bin/factor" 91 | tail -n +2 | tr -d ' \t' | grep -v '^$' |
       tr '\n' ' ' | sed 's/ $//')"
check 'installed dc computes'   '42'   "$(echo '6 7 * p' | "$V8ROOT/bin/dc")"
check 'installed fmt reflows'   'one two three' \
    "$(printf 'one\ntwo\nthree\n' | "$V8ROOT/bin/fmt" | head -1)"
# ed is a line editor driven entirely by stdin; append, write, quit.
printf 'a\nhello ed\n.\nw %s/e.txt\nq\n' "$W" | "$V8ROOT/bin/ed" >/dev/null 2>&1
check 'installed ed writes a file' 'hello ed' "$(cat "$W/e.txt" 2>/dev/null)"
# primes takes a starting value and counts up.
check 'installed primes generates' '11 13 17' \
    "$(echo 11 | "$V8ROOT/bin/primes" 2>/dev/null | head -3 | tr '\n' ' ' | sed 's/ $//')"
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
