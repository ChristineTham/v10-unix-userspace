#!/bin/sh
# End-to-end tests for v8ccom: compile K&R C with V8's front end and our ARM64
# back end, assemble and link with the host toolchain, run, compare output.
#
# Each case is a self-contained K&R C file plus its expected stdout. The point is
# to find what the back end gets WRONG, so a case that fails to compile, fails
# to assemble, or prints the wrong answer all count the same.
#
#   tests/v8ccom/run.sh [path-to-v8ccom]

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
V8CCOM=${1:-$ROOT/build/stage0/ccom-arm64/v8ccom}
CPP=$ROOT/build/stage0/cpp/cpp
TMP=${TMPDIR:-/tmp}/v8ccomtest.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

pass=0 fail=0
FAILED=""

# t <name> <expected-stdout> ; program text on stdin, must define main-callable
# entry points used by the driver in $TMP/drv.c
t() {
	name=$1 want=$2
	cat > "$TMP/t.c"

	if ! "$V8CCOM" "$TMP/t.c" "$TMP/t.s" > "$TMP/cc.log" 2>&1; then
		fail=$((fail+1)); FAILED="$FAILED $name(compile)"
		echo "FAIL $name: v8ccom failed"
		sed 's/^/    /' "$TMP/cc.log" | head -3
		return
	fi
	if [ -s "$TMP/cc.log" ]; then
		fail=$((fail+1)); FAILED="$FAILED $name(diag)"
		echo "FAIL $name: v8ccom diagnostics"
		sed 's/^/    /' "$TMP/cc.log" | head -3
		return
	fi
	if ! clang -c "$TMP/t.s" -o "$TMP/t.o" > "$TMP/as.log" 2>&1; then
		fail=$((fail+1)); FAILED="$FAILED $name(assemble)"
		echo "FAIL $name: assembler rejected the output"
		grep 'error' "$TMP/as.log" | head -3 | sed 's/^/    /'
		return
	fi
	if ! clang "$TMP/drv.c" "$TMP/t.o" -o "$TMP/prog" > "$TMP/ld.log" 2>&1; then
		fail=$((fail+1)); FAILED="$FAILED $name(link)"
		echo "FAIL $name: link failed"
		head -3 "$TMP/ld.log" | sed 's/^/    /'
		return
	fi
	got=$("$TMP/prog" 2>&1)
	if [ "$got" = "$want" ]; then
		pass=$((pass+1))
	else
		fail=$((fail+1)); FAILED="$FAILED $name(output)"
		echo "FAIL $name: want [$want] got [$got]"
	fi
}

# ---------------------------------------------------------------- arithmetic
cat > "$TMP/drv.c" <<'EOF'
#include <stdio.h>
long f();
int main(void){ printf("%ld\n", f()); return 0; }
EOF

t 'return constant' '42' <<'EOF'
f() { return 42; }
EOF

# --------------------------------------------------------- stack frame size
# TWO immediate limits, not one, and only the first was honoured.  add/sub take
# a 12-bit immediate, so a frame of 4096 or more goes through x16 -- that much
# spadjust() always did.  But `mov' assembles to MOVZ, whose immediate is 16
# bits shifted by 0/16/32/48, so it can name 65535 and cannot name 65536.
#
# It did not miscompile; the ASSEMBLER refused the output, which is the good
# direction to fail in and only happens because clang is strict:
#
#	mov x16, #65984
#	    ^ error: expected compatible register or logical immediate
#
# Nothing in the tree had wanted a 64 KB local until a test program did, which
# is why 156 Wave A programs and all of Wave B and C never found it -- the same
# shape as STARG.  Each case writes BOTH ends of the array and adds them, so a
# frame that assembles but is not actually there fails too.
#
# 65600 RATHER THAN 65536, and that is the difference between a boundary case
# and one that looks like a boundary case.  65536 is 0x10000, which MOVZ names
# perfectly well with lsl #16 -- so the obvious "one past the limit" number is
# encodable and the case passed even with the fix reverted.  Measured, then
# corrected: 65600 is 0x10040, which needs both halves.
t 'frame under the add/sub limit'   '16' <<'EOF'
f() { char b[1000]; b[0] = 7; b[999] = 9; return b[0] + b[999]; }
EOF

t 'frame at the add/sub limit'      '16' <<'EOF'
f() { char b[4096]; b[0] = 7; b[4095] = 9; return b[0] + b[4095]; }
EOF

t 'frame just under the MOVZ limit' '16' <<'EOF'
f() { char b[65000]; b[0] = 7; b[64999] = 9; return b[0] + b[64999]; }
EOF

t 'frame past the MOVZ limit'       '16' <<'EOF'
f() { char b[65600]; b[0] = 7; b[65599] = 9; return b[0] + b[65599]; }
EOF

t 'frame far past it'               '16' <<'EOF'
f() { char b[1000000]; b[0] = 7; b[999999] = 9; return b[0] + b[999999]; }
EOF

t 'add' '7' <<'EOF'
add(a,b) int a,b; { return a+b; }
f() { return add(3,4); }
EOF

t 'sub mul' '54' <<'EOF'
f() { int a,b; a = 10; b = 4; return (a-b)*9; }
EOF

t 'divide' '7' <<'EOF'
f() { int a; a = 56; return a/8; }
EOF

t 'modulo' '3' <<'EOF'
f() { int a; a = 23; return a%5; }
EOF

t 'bitwise' '6' <<'EOF'
f() { int a,b; a = 14; b = 7; return (a&b)^8&~8; }
EOF

t 'shifts' '40' <<'EOF'
f() { int a; a = 5; return (a<<4)>>1; }
EOF

t 'unary minus and complement' '-6' <<'EOF'
f() { int a; a = 5; return -a-1; }
EOF

t 'local variables' '30' <<'EOF'
f() { int a,b,c; a=5; b=6; c=a*b; return c; }
EOF

t 'assignment operators' '25' <<'EOF'
f() { int a; a = 5; a += 10; a -= 2; a *= 2; a /= 2; a += 12; return a; }
EOF

t 'increment decrement' '11' <<'EOF'
f() { int a,b; a = 5; a++; b = a++; ++a; return a+b-3; }
EOF

# ------------------------------------------------------------- control flow
t 'if taken' '1' <<'EOF'
f() { int a; a = 5; if (a > 3) return 1; return 0; }
EOF

t 'if not taken' '0' <<'EOF'
f() { int a; a = 2; if (a > 3) return 1; return 0; }
EOF

t 'if else' '20' <<'EOF'
f() { int a; a = 2; if (a > 3) return 10; else return 20; }
EOF

t 'while loop sum' '55' <<'EOF'
f() { int i,s; i=1; s=0; while (i <= 10) { s += i; i++; } return s; }
EOF

t 'for loop' '3628800' <<'EOF'
f() { int i; long p; p=1; for (i=1; i<=10; i++) p *= i; return p; }
EOF

t 'do while' '10' <<'EOF'
f() { int i; i=0; do { i++; } while (i < 10); return i; }
EOF

t 'nested loops' '100' <<'EOF'
f() { int i,j,n; n=0; for(i=0;i<10;i++) for(j=0;j<10;j++) n++; return n; }
EOF

t 'break and continue' '25' <<'EOF'
f() { int i,s; s=0; for(i=0;i<100;i++){ if(i>=10) break; if(i&1) continue; s+=i; } return s+5; }
EOF

t 'logical and or' '1' <<'EOF'
f() { int a,b; a=1; b=0; if (a && !b || b) return 1; return 0; }
EOF

t 'ternary' '7' <<'EOF'
f() { int a; a=3; return a>2 ? 7 : 9; }
EOF

t 'switch' '20' <<'EOF'
f() { int a,r; a=2; switch(a){ case 1: r=10; break; case 2: r=20; break; default: r=30; } return r; }
EOF

t 'switch default' '30' <<'EOF'
f() { int a,r; a=9; switch(a){ case 1: r=10; break; case 2: r=20; break; default: r=30; } return r; }
EOF

t 'goto' '5' <<'EOF'
f() { int i; i=0; again: i++; if (i<5) goto again; return i; }
EOF

# ------------------------------------------------------------------ pointers
t 'pointer deref' '99' <<'EOF'
f() { int a; int *p; a = 99; p = &a; return *p; }
EOF

t 'pointer store' '17' <<'EOF'
f() { int a; int *p; p = &a; *p = 17; return a; }
EOF

t 'array indexing' '30' <<'EOF'
f() { int v[5]; int i; for(i=0;i<5;i++) v[i]=i*i; return v[1]+v[2]+v[3]+v[4]; }
EOF

t 'pointer arithmetic' '3' <<'EOF'
f() { int v[5]; int *p; v[3]=3; p = v; p += 3; return *p; }
EOF

t 'char array' '3' <<'EOF'
f() { char b[8]; b[0]='a'; b[1]='b'; b[2]='c'; b[3]=0; return strlen(b); }
EOF

# ------------------------------------------------------------------ globals
t 'global variable' '77' <<'EOF'
int g;
f() { g = 77; return g; }
EOF

t 'global initialised' '123' <<'EOF'
int g = 123;
f() { return g; }
EOF

t 'global array' '6' <<'EOF'
int ga[4];
f() { ga[0]=1; ga[1]=2; ga[2]=3; return ga[0]+ga[1]+ga[2]; }
EOF

t 'static local' '3' <<'EOF'
bump() { static int n; n++; return n; }
f() { bump(); bump(); return bump(); }
EOF

# ------------------------------------------------------------------- calls
t 'recursion' '55' <<'EOF'
fib(n) int n; { if (n < 2) return n; return fib(n-1)+fib(n-2); }
f() { return fib(10); }
EOF

t 'many arguments' '28' <<'EOF'
g(a,b,c,d,e,ff,gg) int a,b,c,d,e,ff,gg; { return a+b+c+d+e+ff+gg; }
f() { return g(1,2,3,4,5,6,7); }
EOF

t 'call through pointer' '9' <<'EOF'
sq(n) int n; { return n*n; }
f() { int (*p)(); p = sq; return (*p)(3); }
EOF

t 'char and short types' '3' <<'EOF'
f() { char c; short s; c = 1; s = 2; return c+s; }
EOF

t 'unsigned comparison' '1' <<'EOF'
f() { unsigned u; u = 1; if (u > 0) return 1; return 0; }
EOF

# An unsigned `/`, `%` or `>>` whose result is USED IN A SIGNED CONTEXT.
#
# optim.c's sconvert() drops a conversion that changes nothing but signedness
# and paints its type onto the operand, which is right for every operator whose
# bits are the same either way -- and wrong for these three, where gencode.c
# reads that same type to pick udiv/sdiv and lsr/asr.  The operand needs its top
# bit set for the two to differ, so every case here uses one that has it.
#
# Each of these returned a NEGATIVE number before the fix.  Found through
# printf: doprnt's convert() writes digits[val % base], where the subscript
# supplies the conversion, so %lx of a negative long lost its last digit.
t 'unsigned mod through a signed conversion' '13' <<'EOF'
f() { unsigned long v; v = 0xbf1a36e2eb1c432dL; return (long)(v % 16); }
EOF

# Divides by 2^60 rather than by 16 so the quotient fits in the int this
# harness's f() returns -- the answer is then 11 unsigned against -4 signed,
# rather than two 64-bit numbers whose difference needs squinting at.
t 'unsigned divide through a signed conversion' '11' <<'EOF'
f() { unsigned long v; v = 0xbf1a36e2eb1c432dL;
      return (long)(v / 0x1000000000000000L); }
EOF

t 'unsigned shift through a signed conversion' '11' <<'EOF'
f() { unsigned long v; v = 0xbf1a36e2eb1c432dL; return (long)(v >> 60); }
EOF

# The shape that actually failed: no cast in sight, the subscript is the
# conversion.  tbl[13] is 'd'.
t 'unsigned mod as a subscript' '100' <<'EOF'
char tbl[] = "0123456789abcdef";
f() { unsigned long v; v = 0xbf1a36e2eb1c432dL; return tbl[v % 16]; }
EOF

# Narrowing repaints through the other exit from sconvert(), at the bottom.
t 'unsigned mod narrowed to int' '13' <<'EOF'
f() { unsigned long v; v = 0xbf1a36e2eb1c432dL; return (int)(v % 16); }
EOF

# ... and the assign-op form, which the ASG block above sconvert() already
# refuses to rewrite for exactly this reason.
t 'unsigned mod-assign through a signed conversion' '13' <<'EOF'
f() { unsigned long v; v = 0xbf1a36e2eb1c432dL; return (long)(v %= 16); }
EOF

# `long f()', not `f()', AND THAT IS A CORRECTION RATHER THAN A TIDY-UP.  The
# driver declares `long f();' but this definition had an implicit int return,
# so `return a*1000000' converts a long to an int and the answer IS
# 1215752192 -- clang -std=gnu89 agrees.  It read as 100000000000 only because
# v8cc never truncated an int, which is the bug arm64_trunc() fixes below: THE
# CASE WAS CALIBRATED AGAINST THE BROKEN COMPILER, so fixing the compiler broke
# the test.  Same shape as tests/wavec counting drawing commands that only
# matched while every coordinate was zero.  Declaring the return type is what
# makes it test long arithmetic, which is what its name says.
t 'long arithmetic' '100000000000' <<'EOF'
long f() { long a; a = 100000; return a*1000000; }
EOF

# ...and the truncation it used to hide, asserted on purpose.
t 'an int return truncates a long expression' '1215752192' <<'EOF'
f() { long a; a = 100000; return a*1000000; }
EOF

t 'sizeof' '4' <<'EOF'
f() { return sizeof(int); }
EOF

t 'string literal' '5' <<'EOF'
f() { char *s; s = "hello"; return strlen(s); }
EOF

t 'struct member' '30' <<'EOF'
struct pt { int x; int y; };
f() { struct pt p; p.x = 10; p.y = 20; return p.x + p.y; }
EOF

t 'struct pointer' '12' <<'EOF'
struct pt { int x; int y; };
f() { struct pt p; struct pt *q; q = &p; q->x = 5; q->y = 7; return q->x + q->y; }
EOF

# -------------------------------------------------------------- floating point
cat > "$TMP/drv.c" <<'EOF'
#include <stdio.h>
double fd();
int main(void){ printf("%.4f\n", fd()); return 0; }
EOF

t 'double arithmetic' '7.5000' <<'EOF'
double fd() { double a,b; a = 5.0; b = 2.5; return a+b; }
EOF

t 'double multiply divide' '6.2500' <<'EOF'
double fd() { double a; a = 12.5; return a*2.0/4.0; }
EOF

t 'double subtract negate' '-3.2500' <<'EOF'
double fd() { double a,b; a = 1.25; b = 4.5; return a-b; }
EOF

t 'int to double' '3.5000' <<'EOF'
double fd() { int i; double d; i = 7; d = i; return d/2.0; }
EOF

t 'double compare' '1.0000' <<'EOF'
double fd() { double a,b; a = 2.5; b = 1.5; if (a > b) return 1.0; return 0.0; }
EOF

t 'double loop accumulate' '5.5000' <<'EOF'
double fd() { int i; double s; s = 0.0; for(i=1;i<=10;i++) s += 0.55; return s; }
EOF

t 'float type' '2.5000' <<'EOF'
double fd() { float f; f = 2.5; return f; }
EOF

t 'double function argument' '9.0000' <<'EOF'
double twice(x) double x; { return x*2.0; }
double fd() { double twice(); return twice(4.5); }
EOF

t 'double array' '6.0000' <<'EOF'
double fd() { double v[4]; v[0]=1.0; v[1]=2.0; v[2]=3.0; return v[0]+v[1]+v[2]; }
EOF

cat > "$TMP/drv.c" <<'EOF'
#include <stdio.h>
long f();
int main(void){ printf("%ld\n", f()); return 0; }
EOF

t 'double to int' '7' <<'EOF'
f() { double d; d = 7.9; return (int)d; }
EOF

# ------------------------------------------------------ structs and bitfields
t 'struct assignment' '77' <<'EOF'
struct s { int a; int b; int c; };
f() { struct s x, y; x.a=70; x.b=7; x.c=0; y = x; return y.a+y.b; }
EOF

t 'large struct assignment' '1225' <<'EOF'
struct big { int v[50]; };
f() { struct big x, y; int i, t; for(i=0;i<50;i++) x.v[i]=i; y = x; t=0;
      for(i=0;i<50;i++) t += y.v[i]; return t; }
EOF

t 'nested struct copy' '9' <<'EOF'
struct in { int p; int q; };
struct out { struct in i; int r; };
f() { struct out a, b; a.i.p=4; a.i.q=5; a.r=0; b = a; return b.i.p+b.i.q; }
EOF

t 'bitfield read' '5' <<'EOF'
struct bf { unsigned lo:4; unsigned hi:4; };
f() { struct bf b; b.lo = 5; b.hi = 3; return b.lo; }
EOF

t 'bitfield high' '3' <<'EOF'
struct bf { unsigned lo:4; unsigned hi:4; };
f() { struct bf b; b.lo = 5; b.hi = 3; return b.hi; }
EOF

t 'bitfield sum' '8' <<'EOF'
struct bf { unsigned lo:4; unsigned hi:4; };
f() { struct bf b; b.lo = 5; b.hi = 3; return b.lo + b.hi; }
EOF

# ------------------------------------------------------------- many arguments
t 'twelve arguments' '78' <<'EOF'
g(a,b,c,d,e,ff,gg,hh,ii,jj,kk,ll) int a,b,c,d,e,ff,gg,hh,ii,jj,kk,ll;
{ return a+b+c+d+e+ff+gg+hh+ii+jj+kk+ll; }
f() { return g(1,2,3,4,5,6,7,8,9,10,11,12); }
EOF

t 'nine arguments' '45' <<'EOF'
g(a,b,c,d,e,ff,gg,hh,ii) int a,b,c,d,e,ff,gg,hh,ii;
{ return a+b+c+d+e+ff+gg+hh+ii; }
f() { return g(1,2,3,4,5,6,7,8,9); }
EOF

t 'many args with call inside' '66' <<'EOF'
id(x) int x; { return x; }
g(a,b,c,d,e,ff,gg,hh,ii,jj,kk) int a,b,c,d,e,ff,gg,hh,ii,jj,kk;
{ return a+b+c+d+e+ff+gg+hh+ii+jj+kk; }
f() { return g(1,2,3,id(4),5,6,7,8,9,10,11); }
EOF

# ---------------------------------------------- and the CALLER has to survive
# The two cases above ask whether the callee RECEIVED the arguments.  They both
# passed while a nine-argument call was destroying its caller's saved registers,
# because a test that only reads the callee cannot see it.
#
# AAPCS64 puts arguments nine and up at [sp, #0], and the prologue used to save
# the callee-saved registers at the bottom of the frame -- so the last one saved
# WAS [sp, #0].  A function that both used register variables and called
# something with more than eight arguments therefore overwrote the register it
# had saved on behalf of its caller, and handed the corrupted value back on
# return.  emit.c's arm64_endfunction now puts the call area beneath the saves.
#
# Three frames are needed to see it: the damage lands on the caller of the
# function that makes the wide call, so `mid' must both save registers and make
# the call, and `f' must still be using its own afterwards.  Nothing in 156 Wave
# A programs plus Wave B and C provoked it; printp() in ps(1) did, with a
# seven-value sprintf.
t 'a wide call does not eat the caller-of-the-callers registers' '210' <<'EOF'
nine(a,b,c,d,e,ff,gg,hh,ii) int a,b,c,d,e,ff,gg,hh,ii; { return 0; }
mid()
{
	register int q1,q2,q3,q4,q5,q6;
	q1=1; q2=2; q3=3; q4=4; q5=5; q6=6;
	nine(1,2,3,4,5,6,7,8,524287);
	return q1+q2+q3+q4+q5+q6;
}
f()
{
	register int p1,p2,p3,p4,p5,p6;
	p1=10; p2=20; p3=30; p4=40; p5=50; p6=60;
	mid();
	return p1+p2+p3+p4+p5+p6;
}
EOF

# The same shape with POINTERS, which is how it actually presented: ps(1) got a
# char * back where main() kept a struct direct *, and walked off its array.
t 'a wide call does not eat a caller-held pointer' '111' <<'EOF'
static int tab[4];
nine(a,b,c,d,e,ff,gg,hh,ii) int a,b,c,d,e,ff,gg,hh,ii; { return 0; }
mid(junk) char *junk;
{
	register char *r1, *r2, *r3, *r4, *r5, *r6;
	r1=junk; r2=junk; r3=junk; r4=junk; r5=junk; r6=junk;
	nine(1,2,3,4,5,6,7,8,junk);
	return r1 == junk;
}
f()
{
	register int *p; register int n;
	tab[0]=1; tab[1]=10; tab[2]=100;
	p = tab; n = 0;
	mid("x");
	n = p[0] + p[1] + p[2];
	return n;
}
EOF

# ------------------------------------------------------- struct passed by value
# STARG.  The back end copies the aggregate into CONSECUTIVE 8-byte argument
# slots -- the V8/VAX convention, not AAPCS64's by-reference rule for composites
# over 16 bytes.  PLAN.md S4f has why.
#
# These are written after grap showed which combinations matter, not before: a
# synthetic suite passed 62 cases while the first program to pass a struct by
# value would not compile at all.  Each shape below is one the manual
# differential against clang covered.
#
# Sizes matter here in a way they do not elsewhere.  Pass 1's simpstr() rewrites
# a struct argument to a plain scalar when its size is exactly 1, 2, 4 or 8
# bytes, so ONLY other sizes reach STARG.  A 24-byte struct is three slots; a
# 12-byte one is two, with the second half-used.

t 'struct by value, 24 bytes' '6' <<'EOF'
struct p3 { long a, b, c; };
take(v) struct p3 v; { return v.a + v.b + v.c; }
f() { struct p3 v; v.a = 1; v.b = 2; v.c = 3; return take(v); }
EOF

t 'two structs by value' '21' <<'EOF'
struct p3 { long a, b, c; };
take(v, w) struct p3 v, w; { return v.a+v.b+v.c + w.a+w.b+w.c; }
f() { struct p3 v, w;
	v.a = 1; v.b = 2; v.c = 3;
	w.a = 4; w.b = 5; w.c = 6;
	return take(v, w); }
EOF

# The one that exercises the discontinuity in argslot(): slots 0-7 are scratch
# loaded into x0-x7 just before the branch, slots 8+ are the outgoing area at
# sp+0.  Six scalars then a three-slot struct puts it across the boundary.
t 'struct straddling the register/stack boundary' '621' <<'EOF'
struct p3 { long a, b, c; };
take(q,r,s,t,u,w, v, z) struct p3 v; { return q+r+s+t+u+w + v.a+v.b+v.c + z; }
f() { struct p3 v; v.a = 100; v.b = 200; v.c = 300;
	return take(1,2,3,4,5,6, v, 0); }
EOF

# Not a multiple of the slot size: stn.stsize arrives already rounded up to 128
# bits by argsize(), so the copy moves 16 bytes for a 12-byte object.  The
# members must still land where the callee reads them.
t 'struct by value, 12 bytes' '66' <<'EOF'
struct i3 { int a, b, c; };
take(k, v) struct i3 v; { return k + v.a + v.b + v.c; }
f() { struct i3 v; v.a = 11; v.b = 22; v.c = 33; return take(0, v); }
EOF

# A call inside the argument: the struct's ADDRESS is what the subtree yields,
# and evaluating it must not disturb an argument already placed.
t 'struct by value with a call in the address' '15' <<'EOF'
struct p3 { long a, b, c; };
struct p3 *idp(q) struct p3 *q; { return q; }
take(k, v) struct p3 v; { return k + v.a + v.b + v.c; }
f() { struct p3 v; v.a = 2; v.b = 4; v.c = 8; return take(1, *idp(&v)); }
EOF

# Doubles inside the aggregate, which is grap's actual Point shape.  The members
# travel through integer slots and must come back out as doubles.
t 'struct of doubles by value' '450' <<'EOF'
struct pt { char *tag; double x, y; };
take(v) struct pt v; { return (long)((v.x + v.y) * 100.0); }
f() { struct pt v; v.tag = "t"; v.x = 1.5; v.y = 3.0; return take(v); }
EOF

# --------------------------------------- int members of an aggregate PARAMETER
# acctype() widens an int VPARAM to the full 8-byte slot, because K&R makes an
# undeclared parameter int and the tree routinely puts a pointer in one.  That
# widening must NOT reach an int MEMBER of an aggregate parameter -- pass 1
# presents both as the same node.  arm64_aggparam() in local.c is what tells
# them apart, from the declared types bfcode() is handed.
#
# This was a documented limitation until STARG made it reachable.  Its symptom
# is not a crash: the low 32 bits are right and everything above them is the
# next member, so the value only looks wrong once something reads it as a long.

t 'int members of a struct parameter do not swallow each other' '66' <<'EOF'
struct i3 { int a, b, c; };
take(k, v) struct i3 v; { return k + v.a + v.b + v.c; }
f() { struct i3 v; v.a = 11; v.b = 22; v.c = 33; return take(0, v); }
EOF

# The exact shape from the note above acctype(): a union whose int member is
# narrower than the union, assigned and then passed by value.  This is pic's
# makeattr(), which had to be fixed in the source before the compiler could.
t 'narrow member of a union parameter' '1' <<'EOF'
union u { int i; char *p; };
take(v) union u v; { return (v.i == 0); }
f() { union u v; v.p = "rubbish in the top half"; v.i = 0; return take(v); }
EOF

# A scalar int parameter next to an aggregate one must STILL be widened -- the
# fix must not throw away the thing acctype() exists for.  Here `s' is an
# undeclared parameter holding a char *, the 271-parameters case.
t 'undeclared pointer parameter is still widened' '5' <<'EOF'
struct i3 { int a, b, c; };
take(s, v) struct i3 v; { return strlen(s) + v.a; }
f() { struct i3 v; v.a = 2; v.b = 0; v.c = 0; return take("abc", v); }
EOF

# ---------------------------------------------------------------------------
# AN int MUST WRAP AT 32 BITS, and for four years it did not.
#
# Every integer here lives in an x register, properly extended -- an `int' is
# loaded with `ldrsw' and is correct as a 64-bit quantity.  Arithmetic was then
# emitted 64-bit: `add x9, x9, x10'.  Right for every result that FITS, and
# wrong the moment one does not, because the register has no 32-bit edge to
# wrap at.  The value then disagrees with itself -- printf("%d") reads the low
# half and is RIGHT, while a comparison reads all 64 and is WRONG.
#
# It needs an int accumulator that actually overflows AND lives in a register:
# an automatic is stored back through `str w' and re-narrowed by the store, so
# `register' is what exposes it.  1187 cases had not reached that pair.
#
# Found in dumpdir's checksum(), which sums 256 arbitrary ints off a tape
# record: it computed exactly CHECKSUM, printed exactly CHECKSUM, and took the
# not-equal branch -- so every dump tape this port wrote was unreadable by the
# two programs written to read it.  gencode.c's arm64_trunc() is the fix.
#
# The cases below are the four operators that can set bits above bit 31.  Each
# is written so the true 64-bit answer and the true 32-bit answer differ, and
# each asks a COMPARISON rather than printing -- printing was never wrong.
# ---------------------------------------------------------------------------
echo
echo "  -- int overflow must wrap in a register, not accumulate in x"

# 2000000000*3 - 1704948258 = 4295051742; that is 84446 modulo 2^32.
t 'register int addition wraps at 32 bits' '1' <<'EOF'
f() {
	register i, j;
	int big[4];
	big[0] = 2000000000; big[1] = 2000000000;
	big[2] = 2000000000; big[3] = -1704948258;
	i = 0;
	for (j = 0; j < 4; j++)
		i += big[j];
	return (i == 84446);
}
EOF

# ...and the plain binary form, not just the compound assignment.
t 'and so does i = i + x' '1' <<'EOF'
f() {
	register i;
	int a, b;
	a = 2000000000; b = 2000000000;
	i = a + b;
	return (i < 0);		/* 4e9 wrapped is negative */
}
EOF

t 'subtraction wraps too' '1' <<'EOF'
f() {
	register i;
	int a, b;
	a = -2000000000; b = 2000000000;
	i = a - b;
	return (i > 0);		/* -4e9 wrapped is positive */
}
EOF

t 'multiplication wraps' '1' <<'EOF'
f() {
	register i;
	int a, b;
	a = 65536; b = 65536;
	i = a * b;
	return (i == 0);	/* 2^32 wrapped is 0 */
}
EOF

t 'a left shift off the top wraps' '1' <<'EOF'
f() {
	register i;
	int a, n;
	a = 1; n = 32;
	i = a << n;
	return (i == 0);
}
EOF

# The other half of the claim: an UNSIGNED int must wrap by zero-extension, so
# the same sum has to compare equal against the unsigned value rather than the
# sign-extended one.  arm64_widen emits `mov w,w' here and `sxtw' above, and
# getting that backwards would pass every signed case.
t 'unsigned int wraps by zero extension' '1' <<'EOF'
f() {
	register unsigned u;
	unsigned a, b;
	a = 4000000000; b = 1000000000;
	u = a + b;
	return (u == 705032704);	/* 5e9 - 2^32 */
}
EOF

# AND, OR, ER and RS of correctly-extended operands are ALREADY correct, so
# arm64_trunc deliberately leaves them alone.  This is the negative control for
# that decision: it must still be right, and if someone "simplifies" the guard
# to every operator these keep passing while the code gets slower -- the point
# is that they pass WITHOUT the extra instruction.
t 'a negative int survives and or xor and >>' '1' <<'EOF'
f() {
	register i;
	int a;
	a = -1000000000;
	i = a & -1;
	if (i != -1000000000) return (0);
	i = a | 0;
	if (i != -1000000000) return (0);
	i = a ^ 0;
	if (i != -1000000000) return (0);
	i = a >> 1;
	if (i != -500000000) return (0);
	return (1);
}
EOF

# ---------------------------------------------------------------------------
# ...AND THE SAME FAULT IN THE TWO UNARY OPERATORS.  Found by sweeping the list
# above rather than by a program failing: the first arm64_trunc() considered
# only the BINARY operators, because the checksum bug happened to be a `+'.
#
# `neg' is a subtract and `mvn' is a NOT, and both are emitted x-form:
#
#	-INT_MIN is 2^31, which does not fit -- so it came out POSITIVE.
#	-u on an UNSIGNED is wrong for EVERY nonzero value, not just an edge
#	   case: the operand is zero-extended and `neg' sets all 64 top bits.
#	~u on an UNSIGNED is likewise wrong for every value.
#	~i on a SIGNED int is already exactly right, and is the negative
#	   control below -- bits 63..32 all equal bit 31, and flipping every
#	   bit preserves that.
#
# Each case computes its expected value with an operator that WAS covered, so
# the answer is derived by the port rather than transcribed.
# ---------------------------------------------------------------------------
echo
echo "  -- and the unary operators, which the first version of the list missed"

t 'unary minus on unsigned equals 0 minus it' '1' <<'EOF'
f() {
	register unsigned u, v;
	u = 1;
	u = -u;
	v = 0;
	v = v - 1;		/* MINUS is covered; UNARY MINUS was not */
	return (u == v);
}
EOF

t 'negating INT_MIN gives INT_MIN back' '1' <<'EOF'
f() {
	register i, k;
	int a;
	a = 2147483647;
	i = 0 - a - 1;		/* INT_MIN, by a covered operator */
	k = i;
	i = -i;
	return (i == k);
}
EOF

t 'complement on unsigned equals -1 minus it' '1' <<'EOF'
f() {
	register unsigned u, v;
	u = 15;
	v = 0;
	v = v - 1 - 15;		/* ~x == -x-1 */
	u = ~u;
	return (u == v);
}
EOF

# ---------------------------------------------------------------------------
# A FOURTH ONE, AND IT IS NOT IN THE BACK END AT ALL.  optim.c's sconvert()
# deleted a conversion that changes only signedness and painted the new type
# onto the operand -- free on a VAX, where a register was exactly as wide as an
# int.  Here the back end holds an int sign-extended and an unsigned int
# zero-extended, so the paint left the register carrying the SOURCE type's
# extension under the destination type's name, and an x-form `cmp' saw two
# different 64-bit values.  An explicit cast did not help: the cast is what was
# being deleted.  PATCHES.md has it; this is the third fault in those same
# seven lines, which that file predicted.
# ---------------------------------------------------------------------------
echo
echo "  -- a signedness change is a conversion, not a repaint"

t 'unsigned and int holding the same bits compare equal' '1' <<'EOF'
f() {
	register unsigned u;
	register int i;
	u = 0; u = u - 1;	/* 0xffffffff, zero-extended  */
	i = 0; i = i - 1;	/* -1,         sign-extended  */
	return (u == i);	/* C converts i to unsigned   */
}
EOF

t 'an explicit cast between int and unsigned is not a no-op' '1' <<'EOF'
f() {
	register unsigned u;
	register int i;
	u = 0; u = u - 1;
	i = 0; i = i - 1;
	if (u != (unsigned)i) return (0);
	if ((int)u != i) return (0);
	return (1);
}
EOF

# ---------------------------------------------------------------------------
# The negative controls for both fixes, and they have to be read off the
# ASSEMBLY rather than the answer: a redundant extension gives the right value,
# so a behavioural test cannot tell "correct" from "correct and slower".  That
# distinction is the whole reason COMPL is guarded on tyunsigned() instead of
# being added to the operator list, and the reason `& | ^ >>' are left alone.
#
# Each function is small enough that the only extension it can contain is the
# one under test.  A signed int extends with `sxtw' and an unsigned one with
# `mov w,w', so the pattern has to match both -- matching only `mov w' scores a
# signed `neg' as zero and passes while the bug is present.
# ---------------------------------------------------------------------------
echo
echo "  -- and no extension where none is needed (read off the assembly)"

# tasm <name> <extended-regexp> <want-count> ; program text on stdin
tasm() {
	name=$1 pat=$2 want=$3
	cat > "$TMP/a.c"
	if ! "$V8CCOM" "$TMP/a.c" "$TMP/a.s" > "$TMP/cc.log" 2>&1; then
		fail=$((fail+1)); FAILED="$FAILED $name(compile)"
		echo "FAIL $name: v8ccom failed"
		sed 's/^/    /' "$TMP/cc.log" | head -3
		return
	fi
	got=$(grep -cE "$pat" "$TMP/a.s")
	if [ "$got" = "$want" ]; then
		pass=$((pass+1))
	else
		fail=$((fail+1)); FAILED="$FAILED $name(count)"
		echo "FAIL $name: want $want line(s) matching [$pat], got $got"
		sed 's/^/    /' "$TMP/a.s"
	fi
}

EXT='sxtw|mov[[:space:]]+w'

tasm 'signed ~ needs no extension'        "$EXT" 0 <<'EOF'
sc(a) { return (~a); }
EOF

tasm 'unsigned ~ gets exactly one'        "$EXT" 1 <<'EOF'
unsigned uc(a) unsigned a; { return (~a); }
EOF

tasm 'signed neg gets one'                "$EXT" 1 <<'EOF'
sn(a) { return (-a); }
EOF

tasm 'unsigned neg gets one'              "$EXT" 1 <<'EOF'
unsigned un(a) unsigned a; { return (-a); }
EOF

tasm 'and needs none'                     "$EXT" 0 <<'EOF'
sa(a,b) { return (a & b); }
EOF

tasm 'right shift needs none'             "$EXT" 0 <<'EOF'
sr(a,b) { return (a >> b); }
EOF

tasm 'int -> unsigned gets one'           "$EXT" 1 <<'EOF'
unsigned ui(a) int a; { return ((unsigned)a); }
EOF

tasm 'unsigned -> int gets one'           "$EXT" 1 <<'EOF'
iu(a) unsigned a; { return ((int)a); }
EOF

# The claim sconvert()'s comment makes about the cost of keeping the CONV: at
# register width the two types have identical representation, arm64_widen()
# emits nothing for 8 bytes, and so long <-> unsigned long must still be free.
tasm 'long <-> unsigned long stays free'  "$EXT" 0 <<'EOF'
unsigned long ul(a) long a; { return ((unsigned long)a); }
EOF

# ---------------------------------------------------------------------------
# WHERE arm64_trunc() CONTRADICTS THE CALL CASE, pinned so that changing either
# side goes red.  gencode.c refuses to narrow a signed-int CALL return, because
# in this tree `int' usually means "undeclared" and the value is really a
# pointer -- opendir's undeclared malloc is why.  arm64_trunc() then truncates
# at the next arithmetic node, which is correct C and destroys such a pointer.
#
# Both behaviours are wanted; what must not happen is one of them changing
# silently.  Found by `ps -T': ctime() is undeclared in ps.h alone, and
# printp.c:24 does ctime(&up->u_start)+4.  Under Mach-O the image loads at
# 0x100000000, so a truncated pointer is in __PAGEZERO every time.
# ---------------------------------------------------------------------------
echo
echo "  -- the CALL/arithmetic seam that ps -T fell through"

# Half one: the call itself keeps all 64 bits.  This is what makes undeclared
# malloc work at all, and until now only opendir(3) exercised it.
tasm 'a signed int CALL return is not narrowed' "$EXT" 0 <<'EOF'
cg() { return (gg()); }
EOF

# Half two: arithmetic on it truncates.  Right for an int, fatal for a smuggled
# pointer -- and the reason the fix is a DECLARATION in the caller, not a
# change here.  If this ever reads 0, the dumpdir checksum bug is back.
tasm 'but arithmetic on it truncates'           "$EXT" 1 <<'EOF'
cp1() { return (gg() + 4); }
EOF

# ...and with the declaration the port actually applies, nothing is emitted:
# the PLUS is pointer-typed, and arm64_widen() is a no-op above four bytes.
# This is ps.h's fix in one line, and it is the case that would fail if
# someone widened arm64_trunc() to fire on pointer types too.
tasm 'a declared char * survives the same +4'   "$EXT" 0 <<'EOF'
char *ct();
char *cp2() { return (ct() + 4); }
EOF

echo
echo "v8ccom: $pass passed, $fail failed"
[ -n "$FAILED" ] && echo "failing:$FAILED"
[ "$fail" -eq 0 ]
