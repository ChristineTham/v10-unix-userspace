#!/bin/sh
# The self-host boundary, as a test.
#
# THE CLAIM.  Every translation unit of the C compiler -- cpp's two and ccom's
# eighteen, machine-independent and ARM64 back end alike -- compiles with v8cc
# and links freestanding against V8's own libc.  Nothing here needs clang except
# as the assembler and link editor, which are the host's by design (PLAN.md S1).
#
# WHY IT IS ITS OWN SUITE.  Compiling the compiler is a far harsher test of the
# back end than anything in tests/v8ccom, because the input is 20k lines of 1985
# C rather than a case someone thought to write.  It is also a dialect audit of
# compiler/ccom-arm64/, which is new code and drifts towards modern C unless
# something pulls it back: __VA_ARGS__, a U suffix, an automatic aggregate
# initialiser, a declaration after a statement and <stdlib.h> each broke this
# suite before it passed, and each is invisible to a clang build.
#
# WHAT IS NOT CLAIMED, and it matters.  The ccom this produces links but does
# NOT yet run correctly -- see PLAN.md S4c.  So the ccom cases below stop at the
# link, deliberately, and say so.  cpp goes all the way: its self-hosted build
# runs and produces byte-identical output to the stage-0 one, which is a real
# fixpoint for one of the two passes.

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
V8ROOT=$ROOT/rootfs; export V8ROOT
B=$ROOT/build/stage0
CC=$V8ROOT/bin/cc

TMP=${TMPDIR:-/tmp}/selfhost.$$
mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
cd "$TMP" || exit 1

pass=0 fail=0
ck() {
	if [ "$2" = "$3" ]; then pass=$((pass+1))
	else fail=$((fail+1)); echo "FAIL $1"; echo "  want [$2]"; echo "  got  [$3]"; fi
}

[ -x "$CC" ] || { echo "missing $CC -- run make"; exit 1; }

CCOM_MI="xdefs scan pftn trees optim reader common1 pjw lookup catch2 t2print cgram"
CCOM_MD="local local2 emit printx gencode dbstubs"
A64=$ROOT/compiler/ccom-arm64
MIDIR=$ROOT/src/cmd/ccom/common
INC="-I$A64 -I$MIDIR -I."

# cgram.c reads y.debug, which is a checked-in table, not a generated one.
cp "$ROOT/src/cmd/ccom/vax/y.debug.sv" y.debug

# --- every ccom translation unit compiles under v8cc ------------------------
# Warnings are expected and are not failures: manifest.h's `pad[NCHNAM-sizeof
# (char *)]` is a zero-length array on LP64, which V8 mentions every time.  What
# is asserted is that an object came out.
for f in $CCOM_MI; do
	$CC $INC -DYYDEBUG -c -o "$f.o" "$MIDIR/$f.c" 2>/dev/null
	ck "ccom/$f.c compiles under v8cc" yes "$([ -s $f.o ] && echo yes || echo no)"
done
for f in $CCOM_MD; do
	$CC $INC -c -o "$f.o" "$A64/$f.c" 2>/dev/null
	ck "ccom-arm64/$f.c compiles under v8cc" yes "$([ -s $f.o ] && echo yes || echo no)"
done

# --- and links freestanding, against V8's libc and nothing else -------------
# clang here is the LINK EDITOR, not a compiler: -nostdlib, V8's crt0, V8's
# archives, and -lSystem only because the shim is written against the host
# syscall layer.  That is the documented exception list, not a fallback.
clang -nostdlib -e _v8start -o v8ccom "$B/crt0.o" ./*.o \
	"$B/libc/libv8c.a" "$B/v8sys/libv8stubs.a" "$B/v8sys/libv8sys.a" -lSystem \
	2>/dev/null
ck 'the self-hosted ccom links' yes "$([ -x v8ccom ] && echo yes || echo no)"
ck 'the self-hosted ccom is a V8 binary' yes \
   "$(nm v8ccom 2>/dev/null | grep -q '_v8start' && echo yes || echo no)"

# --- cpp, all the way through -----------------------------------------------
CPPSRC=$ROOT/src/cmd/cpp
CPPFLAGS_V8="-Dunix=1 -Darm64=1 -DFLEXNAMES -DMTIME"

# B4's precondition: V8's own yacc reads the PRISTINE grammar.  The stage-0
# build pipes cpy.y through YACCFIX first, because 1978 yacc spelled actions
# '={ ... }' and a modern yacc will not take that.  V8's yacc needs no such fix,
# and this asserts it rather than assuming it.
"$B/yacc/yacc" "$CPPSRC/cpy.y" >/dev/null 2>&1
ck 'V8 yacc reads cpp grammar unfixed' yes "$([ -s y.tab.c ] && echo yes || echo no)"
mv -f y.tab.c cpy.c 2>/dev/null
cp "$CPPSRC/yylex.c" .			# #included, and not a header

$CC $CPPFLAGS_V8 -c -o cpp.o "$CPPSRC/cpp.c" 2>/dev/null
ck 'cpp/cpp.c compiles under v8cc' yes "$([ -s cpp.o ] && echo yes || echo no)"
$CC $CPPFLAGS_V8 -I. -c -o cpy.o cpy.c 2>/dev/null
ck 'cpp/cpy.c compiles under v8cc' yes "$([ -s cpy.o ] && echo yes || echo no)"

clang -nostdlib -e _v8start -o cpp "$B/crt0.o" cpp.o cpy.o \
	"$B/libc/libv8c.a" "$B/v8sys/libv8stubs.a" "$B/v8sys/libv8sys.a" -lSystem \
	2>/dev/null
ck 'the self-hosted cpp links' yes "$([ -x cpp ] && echo yes || echo no)"
ck 'the self-hosted cpp is a V8 binary' yes \
   "$(nm cpp 2>/dev/null | grep -q '_v8start' && echo yes || echo no)"

# It runs, and it is right.
printf '#define SQ(x) ((x)*(x))\nint a = SQ(3);\n' > t.c
# The doubled space is V8's, not a typo: cpp substitutes the expansion and
# leaves the space that preceded the macro name in place.
ck 'the self-hosted cpp expands macros' 'int a =  ((3)*(3));' \
   "$(./cpp t.c 2>/dev/null | grep 'int a')"

# THE FIXPOINT, for this pass.  The stage-0 cpp was built by clang from host
# headers; this one was built by V8's own compiler from V8's headers.  If the
# back end were miscompiling anything cpp depends on, the two would disagree on
# some input -- so the comparison is run over a real source file with real
# macros and real includes, not the toy above.
"$V8ROOT/lib/cpp" "$ROOT/src/cmd/cat.c" "-I$V8ROOT/usr/include" > a.i 2>/dev/null
./cpp "$ROOT/src/cmd/cat.c" "-I$V8ROOT/usr/include" > b.i 2>/dev/null
# Both non-trivial first.  `cmp -s` of two empty files is a pass, and a cpp
# that failed to run would produce exactly that -- the comparison has to be
# stopped from succeeding vacuously before it can mean anything.
ck 'stage-0 cpp produced real output' yes \
   "$([ "$(wc -l < a.i)" -gt 100 ] && echo yes || echo no)"
ck 'self-hosted cpp produced real output' yes \
   "$([ "$(wc -l < b.i)" -gt 100 ] && echo yes || echo no)"
ck 'self-hosted cpp output is byte-identical' yes \
   "$(cmp -s a.i b.i && echo yes || echo no)"

# --- THE FIXPOINT ------------------------------------------------------------
# ccom1 above was built by the stage-0 compiler, which clang built from the host
# headers.  So ccom1 inherits one generation of difference and ccom1 == ccom2 is
# NOT the property to test -- it is false, by two instructions, and was false
# for a good reason: stage-0 disagreed with the ported compiler about one
# unsignedness question.
#
# The property that means "this compiler reproduces itself" is ccom2 == ccom3:
# build ccom2 with ccom1, ccom3 with ccom2, and compare what those two GENERATE.
# Generated assembly, not the binaries -- Mach-O embeds a UUID and timestamps,
# so two identical compilers produce non-identical files.
#
# This is the classic three-stage bootstrap, and the convergence at stage 2 is
# the point: whatever stage 0 believed has been washed out.
#
# HONEST LIMIT.  These cases were NOT mutation-verified against the bug that
# made them possible.  Removing the alignment argument from commdec() again --
# the defect that kept the self-hosted ccom from running at all -- does not fail
# this suite, because that fault was layout-dependent: it needed a particular
# arrangement of objects, and the arrangement here happens not to trigger it.
# So read a pass as "the compiler converges", not "the compiler is correct".
# A regression that changes generated code in a self-consistent way would
# converge too, and these cases would not notice.
stage() {				# stage COMPILER OUTDIR -> builds OUTDIR/ccom
	_cc=$1; _out=$2
	mkdir -p "$_out" "$_out/root/lib" "$_out/root/usr"
	cp "$ROOT/src/cmd/ccom/vax/y.debug.sv" "$_out/y.debug"
	cp "$V8ROOT/lib/cpp" "$_out/root/lib/cpp"
	cp "$_cc" "$_out/root/lib/ccom"
	cp "$V8ROOT"/lib/crt0.o "$V8ROOT"/lib/libv8*.a "$_out/root/lib/"
	cp -R "$V8ROOT/usr/include" "$_out/root/usr/"
	( cd "$_out" || exit 1
	  for f in $CCOM_MI; do
		V8ROOT=$_out/root $CC $INC -DYYDEBUG -c -o "$f.o" "$MIDIR/$f.c" 2>/dev/null
	  done
	  for f in $CCOM_MD; do
		V8ROOT=$_out/root $CC $INC -c -o "$f.o" "$A64/$f.c" 2>/dev/null
	  done
	  clang -nostdlib -e _v8start -o ccom "$B/crt0.o" ./*.o \
		"$B/libc/libv8c.a" "$B/v8sys/libv8stubs.a" "$B/v8sys/libv8sys.a" \
		-lSystem 2>/dev/null )
}

stage "$TMP/v8ccom" "$TMP/s2"
ck 'ccom2 builds, using ccom1' yes "$([ -x $TMP/s2/ccom ] && echo yes || echo no)"
stage "$TMP/s2/ccom" "$TMP/s3"
ck 'ccom3 builds, using ccom2' yes "$([ -x $TMP/s3/ccom ] && echo yes || echo no)"

# Compare on a real translation unit, preprocessed the way the driver does it.
"$V8ROOT/lib/cpp" "$ROOT/src/cmd/cat.c" "-I$V8ROOT/usr/include" > fx.i 2>/dev/null
"$TMP/s2/ccom" fx.i fx2.s >/dev/null 2>&1
"$TMP/s3/ccom" fx.i fx3.s >/dev/null 2>&1
ck 'ccom2 compiles a real file' yes \
   "$([ "$(wc -c < fx2.s 2>/dev/null || echo 0)" -gt 1000 ] && echo yes || echo no)"
ck 'ccom3 compiles a real file' yes \
   "$([ "$(wc -c < fx3.s 2>/dev/null || echo 0)" -gt 1000 ] && echo yes || echo no)"
ck 'FIXPOINT: ccom2 and ccom3 generate identical code' yes \
   "$(cmp -s fx2.s fx3.s && echo yes || echo no)"

echo "selfhost: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
