#!/bin/sh
# The Bourne shell, compiled by v8cc from authentic V8 source.
#
# Every case runs with a hard wall-clock limit, because the failure mode this
# shell has shown is a HANG rather than a crash: a fault inside a handler that
# stdsigs() installed returns, the instruction retries, and it spins. A test
# that simply waited would hang the suite instead of failing it.

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SH=$ROOT/build/stage0/sh/sh
TMP=${TMPDIR:-/tmp}/shtest.$$
mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
cd "$TMP" || exit 1

pass=0 fail=0

try() {	# try <label> <expected> <shell source>
	label=$1; want=$2; src=$3
	"$SH" -c "$src" > out.txt 2>&1 &
	p=$!
	i=0
	while [ $i -lt 5 ]; do
		kill -0 $p 2>/dev/null || break
		sleep 1; i=$((i+1))
	done
	if kill -0 $p 2>/dev/null; then
		kill -9 $p 2>/dev/null
		fail=$((fail+1)); echo "FAIL $label (hung)"; return
	fi
	got=$(tr '\n' ' ' < out.txt | sed 's/ $//')
	if [ "$got" = "$want" ]; then pass=$((pass+1))
	else fail=$((fail+1)); echo "FAIL $label"; echo "  want [$want]"; echo "  got  [$got]"; fi
}

[ -x "$SH" ] || { echo "missing $SH -- run make sh"; exit 1; }

try 'echo'              'hello world'   'echo hello world'
try 'assignment'        '42'            'x=42; echo $x'
try 'two assignments'   '12'            'a=1 b=2; echo $a$b'
try 'for loop'          'a b c'         'for i in a b c; do echo $i; done'
try 'while loop'        'w0 w1 w2'      'i=0; while [ $i -lt 3 ]; do echo w$i; i=`expr $i + 1`; done'
try 'if/then'           'yes'           'if true; then echo yes; fi'
try 'if/else'           'no'            'if false; then echo yes; else echo no; fi'
try 'case'              'matched'       'case abc in a*) echo matched;; esac'
try 'pipeline'          'ONE'           'echo one | tr a-z A-Z'
try 'redirect out/in'   'a'             'echo a > f1; cat < f1'
try 'append'            'a b'           'echo a > f2; echo b >> f2; cat f2'
try 'backquotes'        'bq'            'echo `echo bq`'
try 'positional params' '2 p q'         'set -- p q; echo $# $1 $2'
try 'function'          'infunc'        'f() { echo infunc; }; f'
try 'function args'     'arg'           'f() { echo $1; }; f arg'
try 'brace expansion'   "$HOME"         'echo ${HOME}'
try 'export to a child' 'hi'            'x=hi; export x; sh -c "echo \$x"'
try 'and/or'            't'             'true && echo t || echo f'
try 'exit status'       '1'             'false; echo $?'
try 'test builtin'      'isdir'         'test -d /tmp && echo isdir'

# Arithmetic expansion postdates 1985, so this SHOULD be a syntax error.
"$SH" -c 'echo $((1))' > out.txt 2>&1
if grep -q 'syntax error' out.txt; then pass=$((pass+1))
else fail=$((fail+1)); echo "FAIL \$((...)) should not parse in a 1985 shell"; fi

# ---------------------------------------------------------------------------
# spname(), which nothing above can reach.
#
# xec.c:712 calls it only under flags&ttyflg, so `sh -c' never does, and the
# whole file was untested until a DIRSIZ count found a global-buffer-overflow
# in it.  The probe links the real spname.o; src/cmd/sh/PORTING.md has the bug.
#
# These are behavioural on purpose.  The overflow itself is a WRITE whose bytes
# happen to be right, so no value can see it -- ASan can, and did, but not in
# the V8 world.  What IS observable is the truncation that comes with it: at
# DIRSIZ 14 the guess is cut to 14 characters, so a long name cannot be
# corrected at all.  That is the case that goes red if the number regresses,
# and it goes red the other way too -- raising DIRSIZ without raising newname
# makes newname[1024-DIRSIZ-2] negative and spname returns null for everything.
#
# Compiled from the UNMODIFIED src/cmd/sh/spname.c, with tests/sh/hostndir/
# first on the include path supplying <ndir.h>; that header says why it is a
# host probe rather than a V8 one.  -fsanitize=address is what sees the write
# itself, since the bytes it writes are the right bytes.
echo
SPP=$TMP/spnameprobe
if clang -w -std=gnu89 -Wno-implicit-int -fsanitize=address -g \
	-I"$ROOT/tests/sh/hostndir" -o "$SPP" \
	"$ROOT/tests/sh/spnameprobe.c" "$ROOT/src/cmd/sh/spname.c" \
	> "$TMP/spbuild.log" 2>&1; then
	mkdir -p "$TMP/sp"
	long=$(awk 'BEGIN{s="";while(length(s)<200)s=s "b";print s}')
	sp() {	# sp <label> <entry to create> <name to ask about> <expected tail>
		rm -rf "$TMP/sp"; mkdir -p "$TMP/sp"; : > "$TMP/sp/$2"
		got=$("$SPP" "$TMP/sp/$3" 2>&1)
		st=$?
		if [ $st -ne 0 ]; then
			fail=$((fail+1)); echo "FAIL spname $1 (exit $st: $got)"; return
		fi
		case "$got" in
		*"/sp/$4 "*) pass=$((pass+1));;
		*) fail=$((fail+1)); echo "FAIL spname $1"
		   echo "  want a correction to [$4]"
		   echo "  got  [$(echo "$got" | cut -c1-100)]";;
		esac
	}
	# the ordinary case: one letter dropped, well inside 14
	sp 'short correction' hello helo hello
	# DIRSIZ+1 characters -- the entry length that overran best[DIRSIZ+1]
	sp '15-char entry'    aaaaaaaaaaaaaaa aaaaaaaaaaaaaa aaaaaaaaaaaaaaa
	# 200 characters: impossible at DIRSIZ 14, because guess is cut to 14 and
	# SPdist then scores 3 against anything longer
	sp 'long correction'  "$long" "${long%b}" "$long"
	# and a name that is nothing like anything present must NOT be corrected
	rm -rf "$TMP/sp"; mkdir -p "$TMP/sp"; : > "$TMP/sp/hello"
	got=$("$SPP" "$TMP/sp/zzzzzzzz" 2>&1)
	case "$got" in
	'(null) '*) pass=$((pass+1));;
	*) fail=$((fail+1)); echo "FAIL spname no false correction"; echo "  got [$got]";;
	esac
else
	fail=$((fail+1)); echo "FAIL spname probe did not build"
	sed 's/^/  /' "$TMP/spbuild.log" 2>/dev/null | head -5
fi

echo "sh: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
