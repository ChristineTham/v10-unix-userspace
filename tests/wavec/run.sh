#!/bin/sh
# Wave C: the document tools.

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NROFF=$ROOT/build/stage0/nroff/nroff
V8ROOT=$ROOT/rootfs; export V8ROOT
TMP=${TMPDIR:-/tmp}/wavec.$$
mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT; cd "$TMP" || exit 1

pass=0 fail=0
check() {
	if [ "$2" = "$3" ]; then pass=$((pass+1))
	else fail=$((fail+1)); echo "FAIL $1"; echo "  want [$2]"; echo "  got  [$3]"; fi
}
[ -x "$NROFF" ] || { echo "missing $NROFF -- run make nroff"; exit 1; }

# nroff reads its terminal table from /usr/lib/term/tab.37, which only exists
# because the shim resolves V8 data paths inside $V8ROOT -- see rootpath() in
# shim/v8sys/syscall.c.  Running it at all proves that seam works.
printf 'hello nroff world\n.br\nsecond line here\n' > t1
check 'nroff formats text' 'hello nroff world second line here' \
    "$("$NROFF" t1 | tr -s ' \n' ' ' | sed 's/^ //;s/ $//')"

printf '.ll 20\nalpha beta gamma delta epsilon\n' > t2
check 'nroff honours .ll' '2' "$("$NROFF" t2 | grep -c .)"

printf '.ce\ncentred\n' > t3
check 'nroff centres' 'centred' "$("$NROFF" t3 | tr -d ' \n')"

printf 'a\n.sp\nb\n' > t4
check 'nroff spaces' '2' "$("$NROFF" t4 | grep -c .)"

printf '.na\none two\n' > t5
check 'nroff no-adjust' 'one two' "$("$NROFF" t5 | tr -s ' \n' ' ' | sed 's/^ //;s/ $//')"


# troff, whose device tables `make rootfs` compiles with makedev.  Its output is
# the device-independent stream a typesetter driver consumes, not text, so the
# test looks at the header it must always emit.
TROFF=$ROOT/build/stage0/troff/troff
if [ -x "$TROFF" ]; then
	"$TROFF" t1 > tr.out 2>tr.err
	check 'troff names the device' 'x T 202' "$(head -1 tr.out)"
	check 'troff states resolution' 'x res 972 1 2' "$(sed -n 2p tr.out)"
	check 'troff initialises' 'x init' "$(sed -n 3p tr.out)"
	check 'troff emits no errors' '' "$(cat tr.err)"
else
	fail=$((fail+4)); echo "FAIL troff not built"
fi


# tbl, and then the whole pipeline.  tbl emits troff macros, not text, so the
# only honest check is to run its output through nroff and read the table.
TBL=$ROOT/build/stage0/tbl/tbl
if [ -x "$TBL" ]; then
	printf '.TS\nc c\nl l.\nName\tValue\nalpha\t1\nbeta\t2\n.TE\n' > tb.in
	"$TBL" tb.in > tb.tr 2>tb.err
	check 'tbl emits troff macros' '.TS' "$(grep -m1 '^\.TS' tb.tr)"
	check 'tbl is silent on stderr' '' "$(cat tb.err)"
	check 'tbl | nroff formats the table' 'Name Value alpha 1 beta 2' \
	    "$("$NROFF" tb.tr | grep -v '^$' | tr -s ' \n' ' ' | sed 's/^ //;s/ $//')"
else
	fail=$((fail+3)); echo "FAIL tbl not built"
fi


# eqn, built with V8's OWN yacc -- modern bison emits #elif, which Reiser's cpp
# does not understand.
EQN=$ROOT/build/stage0/eqn/eqn
if [ -x "$EQN" ]; then
	printf '.EQ\nx sup 2 + y sup 2 = z sup 2\n.EN\n' > eq.in
	"$EQN" eq.in > eq.tr 2>eq.err
	check 'eqn is silent on stderr' '' "$(cat eq.err)"
	check 'eqn emits the .EQ block' '.EQ' "$(grep -m1 '^\.EQ' eq.tr)"
	# the superscript shift: this was an infinity until the caller-save area
	# stopped overlapping the outgoing-argument slots.
	check 'eqn computes the superscript shift' '3' \
	    "$(grep -c "v'-0\.4" eq.tr)"
	# nroff overstrikes italics with backspaces, so keep only the graphic
	# characters rather than trying to match the control ones.
	check 'eqn | nroff sets the equation' 'x2+y2=z2' \
	    "$("$NROFF" eq.tr | tr -dc 'a-zA-Z0-9+=')"
else
	fail=$((fail+4)); echo "FAIL eqn not built"
fi

# pic -- the first program needing BOTH generators, V8's yacc and V8's lex.
PIC=$ROOT/build/stage0/pic/pic
if [ -x "$PIC" ]; then
	printf '.PS\nbox "hello"; arrow; circle "world"\n.PE\n' > pc.in
	"$PIC" pc.in > pc.tr 2>pc.err
	check 'pic is silent on stderr' '' "$(cat pc.err)"
	check 'pic emits the .PS block with dimensions' '.PS 0.500i 1.750i' \
	    "$(grep -m1 '^\.PS ' pc.tr | sed 's/ *$//')"
	# Four sides of the box, the arrow shaft, and two for its head.
	check 'pic draws the box and arrow' '7' "$(grep -c "D'l" pc.tr)"
	check 'pic draws the circle' '1' "$(grep -c "D'c" pc.tr)"
	# The regression that matters.  YYSTYPE went from 4 bytes to 8 under
	# LP64, so makeiattr's `val.i = 0` no longer filled the union it passes
	# by value, the attribute table stopped being cleared between
	# statements, and every object inherited the text of the ones before it
	# -- "hello" appeared three times and "world" once.  See
	# src/cmd/pic/PORTING.md.
	check 'each label appears exactly once' '1 1' \
	    "$(grep -c 'hello' pc.tr) $(grep -c 'world' pc.tr)"
	# Each label occurs three times on its own line -- troff needs
	# \w'hello'u twice to centre it -- so collapse runs before comparing.
	check 'the label goes with its own object' 'hello world' \
	    "$(grep -o "hello\|world" pc.tr | uniq | tr '\n' ' ' | sed 's/ $//')"
else
	fail=$((fail+6)); echo "FAIL pic not built"
fi

echo "wavec: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
