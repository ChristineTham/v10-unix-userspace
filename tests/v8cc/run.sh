#!/bin/sh
# Tests for the v8cc driver: the whole pipeline, cpp -> ccom -> as -> ld.
#
#   tests/v8cc/run.sh

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CC=$ROOT/rootfs/bin/cc
V8ROOT=$ROOT/rootfs
export V8ROOT
TMP=${TMPDIR:-/tmp}/v8cctest.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT
cd "$TMP" || exit 1

pass=0 fail=0
ok()   { pass=$((pass+1)); }
bad()  { fail=$((fail+1)); echo "FAIL $1"; shift; [ $# -gt 0 ] && echo "    $*"; }

# A driver written in modern C, so it may use variadic printf; the code under
# test is compiled by v8cc and must not call variadic functions itself (see the
# note at the bottom of this file).
cat > drv.c <<'EOF'
#include <stdio.h>
long entry(void);
int main(void){ printf("%ld\n", entry()); return 0; }
EOF

# --- the preprocessor actually runs -------------------------------------
cat > pp.c <<'EOF'
#define DOUBLED(x) ((x)*2)
entry() { return DOUBLED(21); }
EOF
if "$CC" -c pp.c 2>err.log && [ -f pp.o ] &&
   clang drv.c pp.o -o pp 2>>err.log && [ "$(./pp)" = "42" ]; then ok
else bad "macro expansion through the full pipeline" "$(head -2 err.log)"; fi

# --- #include resolution against the V8 headers -------------------------
cat > inc.c <<'EOF'
#include <ctype.h>
entry() { return 7; }
EOF
if "$CC" -c inc.c 2>err.log && [ -f inc.o ]; then ok
else bad "#include <ctype.h> from the V8 headers" "$(head -2 err.log)"; fi

# --- -D on the command line reaches cpp ---------------------------------
cat > dflag.c <<'EOF'
entry() {
#ifdef WANTED
	return 1;
#else
	return 0;
#endif
}
EOF
if "$CC" -c -DWANTED dflag.c 2>err.log && clang drv.c dflag.o -o dflag 2>>err.log &&
   [ "$(./dflag)" = "1" ]; then ok
else bad "-DWANTED reaches the preprocessor" "$(head -2 err.log)"; fi

# --- -S stops after the compiler and leaves assembly --------------------
cat > sflag.c <<'EOF'
entry() { return 3; }
EOF
if "$CC" -S sflag.c 2>err.log && [ -f sflag.s ] && grep -q '_entry' sflag.s; then ok
else bad "-S leaves assembly" "$(head -2 err.log)"; fi

# --- -o names the output ------------------------------------------------
cat > oflag.c <<'EOF'
entry() { return 5; }
EOF
if "$CC" -c -o custom.o oflag.c 2>err.log && [ -f custom.o ]; then ok
else bad "-o names the object" "$(head -2 err.log)"; fi

# --- -O is accepted and ignored (c2 is not ported) ----------------------
if "$CC" -O -c -o o2.o oflag.c 2>err.log && [ -f o2.o ]; then ok
else bad "-O is accepted" "$(head -2 err.log)"; fi

# --- several translation units link together ----------------------------
cat > m1.c <<'EOF'
helper() { return 20; }
EOF
cat > m2.c <<'EOF'
entry() { return helper() + 2; }
EOF
if "$CC" -c m1.c 2>err.log && "$CC" -c m2.c 2>>err.log &&
   clang drv.c m1.o m2.o -o multi 2>>err.log && [ "$(./multi)" = "22" ]; then ok
else bad "separate compilation and linking" "$(head -2 err.log)"; fi

# --- a real computation, end to end -------------------------------------
cat > real.c <<'EOF'
gcd(a, b) int a, b; { while (b) { int t; t = a % b; a = b; b = t; } return a; }
entry() { return gcd(1071, 462); }
EOF
if "$CC" -c real.c 2>err.log && clang drv.c real.o -o real 2>>err.log &&
   [ "$(./real)" = "21" ]; then ok
else bad "gcd(1071,462) == 21" "$(head -2 err.log)"; fi

# --- narrow return values across the clang seam --------------------------
# AAPCS64 defines only w0 for a function returning int; the top half of x0 is
# unspecified and clang leaves whatever was there.  This back end computes at
# 64-bit width and compares with an x-form `cmp`, so a returned -1 that is not
# sign-extended tests as 4294967295 -- positive.  `cat nosuchfile` reported
# "input nosuchfile is output" for exactly that reason: open() returned -1 and
# `fi < 0` was false, then fstat() returned -1 and `>= 0` was true.
#
# The rule that came out of it, and what this test pins down:
#
#   char, short and unsigned returns ARE narrowed at the call site.  K&R's
#   implicit type for an undeclared function is signed int and nothing else, so
#   a call node with one of these types must have come from a declaration, and
#   narrowing it is simply correct.
#
#   Plain signed int returns are NOT narrowed, because `int` here may well mean
#   "undeclared".  V8 calls malloc with no declaration in scope and casts the
#   result to a pointer; sign-extending that from 32 bits truncates it, and
#   opendir segfaulted when this was tried.  The shim returns long instead --
#   see the note above the WRAP macros in shim/v8sys/stubs.c.
#
# Note what is NOT asserted: that a clang function DECLARED to return char or
# short is narrowed.  It usually is, but not always, and the difference is not
# under the back end's control -- pass 1's usual arithmetic conversions rewrite
# a node's type IN PLACE rather than inserting a conversion, so by the time the
# generator sees `cnegc() < 0` the call node says TINT and the declared char is
# gone.  Tracing it (V8DBG=1) shows both:
#
#	c = cnegc();		CALLEE calltype=1 (1 bytes)     narrowed
#	if (cnegc() < 0)	CALLEE calltype=4 (4 bytes)     not narrowed
#
# Rather than pretend to a guarantee that holds only in some contexts, the rule
# is that clang-compiled code V8 calls returns long-width values.  The shim does
# (stubs.c), and that is what the first case here checks.
cat > narrow.c <<'EOF'
long negl(void) { return -1; }               /* the shim's convention */
unsigned long bigu(void) { return 0xffffffffUL; }
char *giveptr(void) { static char buf[4] = "ok"; return buf; }
EOF
cat > narrowuse.c <<'EOF'
extern long negl();
extern unsigned long bigu();
long entry()
{
	long v;
	char *p;

	/* the syscall contract: a long-returning clang function tests negative */
	if (!(negl() < 0))          return 1;
	v = negl();
	if (!(v < 0))               return 2;	/* through a variable too */
	if (bigu() < 0)             return 3;	/* 0xffffffff is NOT negative */

	/*
	 * giveptr is deliberately NOT declared, exactly as V8 calls malloc --
	 * opendir.c has `(DIR *)malloc(sizeof(DIR))` with nothing in scope.
	 * K&R types the call int; sign-extending that from 32 bits truncates
	 * the pointer, which is how opendir came to segfault.
	 */
	p = (char *)giveptr();
	if (p[0] != 'o' || p[1] != 'k') return 4;
	return 0;
}
EOF
if "$CC" -c narrowuse.c 2>err.log && clang -c narrow.c 2>>err.log &&
   clang drv.c narrowuse.o narrow.o -o narrow 2>>err.log &&
   [ "$(./narrow)" = "0" ]; then ok
else bad "return values across the clang seam" \
    "got [$(./narrow 2>&1)]; $(head -2 err.log)"; fi


# --- the driver links the V8 world, so printf works -----------------------
#
# Until this landed, `cc -o prog prog.c` linked CLANG's startup and the HOST
# libc.  The program ran, and most of it behaved, and then printf printed
# rubbish: v8cc passes every argument in one positional sequence in x0-x7 (see
# gencall in compiler/ccom-arm64/gencode.c), while Apple's ARM64 ABI passes the
# VARIADIC arguments of a call on the stack.  A v8cc-compiled caller and the
# host printf disagreed about where the arguments were, and
#
#	printf("lit=%d\n", 42)		printed	lit=1839618368
#
# V8's own printf is not variadic in that sense -- it is printf(fmt, args)
# taking &args and walking forward -- so it wants exactly what v8cc emits.  The
# two agree only if the driver links V8's, which it now does.
#
# This is the whole point of the driver: a program compiled and linked with
# nothing but `cc` must work.  The other suites link the archives explicitly and
# would not have caught it.
cat > hello.c <<'EOF'
main()
{
	printf("lit=%d str=%s\n", 42, "ok");
	exit(0);
}
EOF
if "$CC" -o hello hello.c 2>cc.err; then
	got=$(./hello 2>&1)
	[ "$got" = "lit=42 str=ok" ] && ok || \
	    bad "cc -o links V8's printf" "want [lit=42 str=ok] got [$got]"
else
	bad "cc -o hello hello.c" "$(cat cc.err)"
fi

# and the same program compiled and linked in two steps
if "$CC" -c hello.c 2>cc2.err && "$CC" -o hello2 hello.o 2>>cc2.err; then
	got=$(./hello2 2>&1)
	[ "$got" = "lit=42 str=ok" ] && ok || \
	    bad "cc -c then cc -o" "want [lit=42 str=ok] got [$got]"
else
	bad "cc -c then cc -o" "$(cat cc2.err)"
fi

echo "v8cc: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
