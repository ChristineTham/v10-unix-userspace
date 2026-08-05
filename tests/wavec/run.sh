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

# spell -- hashmake -> spellin -> spellprog, the whole pipeline.
SPELLD=$ROOT/build/stage0/spell
if [ -x "$SPELLD/spellprog" ] && [ -x "$SPELLD/spellin" ] && [ -x "$SPELLD/hashmake" ]; then
	printf 'apple\nbanana\ncherry\ndog\nelephant\n' > sp.words
	"$SPELLD/hashmake" < sp.words 2>sp.err | sort -u > sp.hash
	check 'hashmake emits one octal code per word' '5' "$(wc -l < sp.hash | tr -d ' ')"
	"$SPELLD/spellin" "$(wc -l < sp.hash)" < sp.hash > sp.hlist 2>>sp.err
	check 'spellin builds a non-empty hash list' 'yes' \
	    "$([ -s sp.hlist ] && echo yes || echo no)"
	# The point of the whole thing: known words pass, unknown ones are named.
	printf 'apple\nbanana\nzzzqqq\ncherry\nwibble\n' > sp.in
	check 'spellprog reports exactly the unknown words' 'zzzqqq wibble' \
	    "$("$SPELLD/spellprog" sp.hlist /dev/null < sp.in | tr '\n' ' ' | sed 's/ $//')"
	# hashcheck reverses spellin, so the codes must survive the round trip.
	# This is what the half-word fetch macro gets wrong when it is the VAX one:
	# LP64 has sizeof(unsigned) == sizeof(long)/2, the PDP-11's case, and
	# hashlook.c aborts at startup if the wrong macro was compiled in.
	# See src/cmd/spell/PORTING.md.
	if [ -x "$SPELLD/hashcheck" ]; then
		check 'hashcheck recovers every code spellin stored' 'same' \
		    "$("$SPELLD/hashcheck" < sp.hlist 2>/dev/null | sort -u > sp.back
		       cmp -s sp.hash sp.back && echo same || echo differ)"
	else
		fail=$((fail+1)); echo "FAIL hashcheck not built"
	fi
else
	fail=$((fail+4)); echo "FAIL spell not built"
fi

# --- man, end to end ---------------------------------------------------------
# man.sh is run by V8's sh EXPLICITLY, not by its #! line: a shebang is resolved
# by the host kernel before the shim sees it, so `./man` would be run by the
# Mac's shell and look for the Mac's /usr/man.  PLAN.md S4d has the detail; this
# invokes it the way that works so the rest of man is actually covered.
manout=$("$V8ROOT/bin/sh" "$V8ROOT/usr/bin/man" cat 2>&1)
case "$manout" in
*"Eighth Edition"*) pass=$((pass+1)) ;;
*) fail=$((fail+1)); echo "FAIL man cat did not render a page"
   echo "  got [$(echo "$manout" | head -2)]" ;;
esac
case "$manout" in
*"catenate and print"*) pass=$((pass+1)) ;;
*) fail=$((fail+1)); echo "FAIL man cat rendered the wrong page" ;;
esac

# --- spell against UPSTREAM's own word list -----------------------------------
# The cases above use a list this build generates.  That is necessary but not
# sufficient: a reader and writer can agree with each other and both be wrong
# about the format, which is exactly what four attempts at this bug produced.
# hlista is Bell Labs' binary data, and reading it correctly is the claim.
cat > sp.txt <<'SPEOF'
the quick brown fox jumpd over teh lazy dog
SPEOF
check 'spell reads V8 own hlista and finds only the errors' 'jumpd teh' \
   "$("$V8ROOT/bin/sh" "$V8ROOT/usr/bin/spell" sp.txt 2>&1 | tr '\n' ' ' | sed 's/ $//')"

# --- grap: the program that needed struct-by-value ----------------------------
# grap stopped the build at `unimplemented operator 99 (STARG)` until placeargs()
# learned to copy an aggregate into consecutive argument slots.  Its Point is
# {struct obj *; double x, y} -- 24 bytes, three slots -- and plot.c's
# `line(type, p1, p2, desc)` passes two of them.  These cases are the only
# coverage of that code path in the whole tree, so they are worth their weight:
# revert the STARG case in compiler/ccom-arm64/gencode.c and grap does not build.
# See PLAN.md S4f and src/cmd/grap/PORTING.md.
cat > gr1.grap <<'GREOF'
.G1
frame invis ht 2 wid 3 left solid bot solid
coord x 0,10 y 0,100
draw solid
1 1
2 4
3 9
10 100
.G2
GREOF
if [ -x "$V8ROOT/usr/bin/grap" ]; then
	grapout=$("$V8ROOT/usr/bin/grap" gr1.grap 2>&1)
	# The coordinate-transform macro proves the frame and coord were read.
	case "$grapout" in
	*"define xy_gg"*) pass=$((pass+1)) ;;
	*) fail=$((fail+1)); echo "FAIL grap emitted no coordinate transform" ;;
	esac
	# One `line from' per segment between the four data points.  These are the
	# statements plot.c builds by passing two Points BY VALUE, so a broken
	# STARG shows up here as a wrong count or wrong numbers rather than a
	# crash -- the values travel through the argument slots.
	check 'grap plots one line per interval' '3' \
	    "$(printf '%s\n' "$grapout" | grep -c '^line  from')"
	check 'grap transforms the last point' '1' \
	    "$(printf '%s\n' "$grapout" | grep -c 'xy_gg(10.0000,100.000)')"
	# grap.defines is a RUNTIME file at /usr/lib/grap.defines, not an include;
	# without the install rule grap warns on every run.
	check 'grap finds its defines file' '0' \
	    "$(printf '%s\n' "$grapout" | grep -c 'grap warning')"

	# End to end.  This is what caught the pic bug: pic segfaulted on the `.lf`
	# lines grap emits, so grap alone passed while the pipeline produced
	# nothing.  A preprocessor that is never fed downstream is not tested.
	trout=$("$V8ROOT/usr/bin/grap" gr1.grap | "$V8ROOT/usr/bin/pic" \
	        | "$ROOT/build/stage0/troff/troff" 2>/dev/null)
	case "$trout" in
	"x T 202"*) pass=$((pass+1)) ;;
	*) fail=$((fail+1)); echo "FAIL grap|pic|troff produced no device stream" ;;
	esac
	# Drawing commands must survive all three stages.  Zero here was the
	# symptom of the pic crash: exit status was lost through the pipe.
	ndraw=$(printf '%s\n' "$trout" | grep -c '^D')
	if [ "$ndraw" -gt 0 ]; then pass=$((pass+1))
	else fail=$((fail+1)); echo "FAIL grap|pic|troff drew nothing"; fi

	# grap's own PIC token had the identical LP64 fault as pic's TROFF:
	# grapl.l stores a char * in yylval.p, grap.y declared the token <i>.
	# Two lexer rules reach it -- an explicit `pic' statement, and ANY troff
	# request inside .G1/.G2 that is not .G2 itself -- and both segfaulted.
	# Found by sweeping every grammar in the tree after fixing pic, not by
	# hitting it: the graphs above use neither construct.
	printf '.G1\npic box "raw"\n1 1\n2 2\n.G2\n' > gr2.grap
	"$V8ROOT/usr/bin/grap" gr2.grap > gr2.out 2>&1
	check 'grap survives an explicit pic statement' '0' "$?"
	check 'grap passes the pic line through' '1' "$(grep -c 'box "raw"' gr2.out)"
	printf '.G1\n.ft B\n1 1\n2 2\n.G2\n' > gr3.grap
	"$V8ROOT/usr/bin/grap" gr3.grap > gr3.out 2>&1
	check 'grap survives a troff request inside .G1' '0' "$?"
	check 'grap passes the troff request through' '1' "$(grep -c '^\.ft B' gr3.out)"
else
	fail=$((fail+10)); echo "FAIL grap not installed"
fi

# pic must not die on a troff directive inside .PS.  picy.y declared TROFF as
# <i> while the lexer stores a char * in yylval.p, so on LP64 the pointer lost
# its top 32 bits and _doprnt dereferenced 0x4a57c50 instead of 0x104a57c50.
# Nothing in the tree fed pic a `.' line inside a picture until grap did, on
# every graph it emits.  See src/cmd/pic/PORTING.md.
printf '.PS\n.lf 2\nbox\n.PE\n' > pl1.pic
"$V8ROOT/usr/bin/pic" pl1.pic >pl1.out 2>&1
check 'pic survives a bare .lf inside .PS' '0' "$?"
printf '.PS\n.lf 7 some.file\nbox\n.PE\n' > pl2.pic
"$V8ROOT/usr/bin/pic" pl2.pic >pl2.out 2>&1
check 'pic survives a named .lf inside .PS' '0' "$?"
# ...and passes it through rather than swallowing it.
check 'pic passes the troff line through' '1' "$(grep -c '^\.lf 7 some\.file' pl2.out)"

echo "wavec: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
