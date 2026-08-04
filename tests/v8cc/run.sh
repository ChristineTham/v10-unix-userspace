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
# sign-extended tests as 4294967295 -- positive.
#
# That broke EVERY syscall error check in every V8 program, since the whole shim
# is clang-compiled and V8 code checks syscalls with `< 0`.  `cat nosuchfile`
# reported "input nosuchfile is output": open() returned -1, `fi < 0` was false,
# and then fstat() returned -1 and `>= 0` was true, so cat fell into the
# same-file guard with statb still holding stdout's stat from the top of main.
#
# It could not show up in a self-contained test: v8cc's own callees return a
# properly extended x0, so V8-to-V8 calls agree with each other and only the
# foreign seam disagrees.  Hence this test spans both compilers, and covers the
# three ways the value is consumed -- through memory, in a register variable,
# and straight out of the assignment.
cat > narrow.c <<'EOF'
int  cneg(void)  { return -1; }
char cnegc(void) { return -1; }
short cnegs(void){ return -1; }
unsigned cnegu(void) { return 0xffffffffu; }
EOF
cat > narrowuse.c <<'EOF'
extern int cneg(); extern char cnegc(); extern short cnegs();
/*
 * cnegu must be declared.  Left undeclared, K&R says it returns int, so the
 * compiler sign-extends and 0xffffffff correctly tests negative -- the right
 * answer to a different question.
 */
extern unsigned cnegu();
long entry()
{
	int mem; register int rvar;

	mem = cneg();
	rvar = cneg();
	if (!(mem < 0))            return 1;	/* via memory (ldrsw) */
	if (!(rvar < 0))           return 2;	/* in a register variable */
	if (!((mem = cneg()) < 0)) return 3;	/* value of the assignment */
	if (!(cneg() < 0))         return 4;	/* used directly */
	if (!(cnegc() < 0))        return 5;	/* char return */
	if (!(cnegs() < 0))        return 6;	/* short return */
	if (cnegu() < 0)           return 7;	/* unsigned must NOT go negative */
	return 0;
}
EOF
if "$CC" -c narrowuse.c 2>err.log && clang -c narrow.c 2>>err.log &&
   clang drv.c narrowuse.o narrow.o -o narrow 2>>err.log &&
   [ "$(./narrow)" = "0" ]; then ok
else bad "narrow return values are extended at the clang seam" \
    "got [$(./narrow 2>&1)]; $(head -2 err.log)"; fi

echo "v8cc: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

# ---------------------------------------------------------------------------
# NOT TESTED HERE, AND NOT A BUG: calling a host variadic function.
#
# v8cc passes arguments in one positional sequence in x0-x7 (see gencall in
# compiler/ccom-arm64/gencode.c).  Apple's ARM64 ABI passes VARIADIC arguments
# on the stack instead, so a v8cc-compiled call to the host's printf reads
# garbage -- the first attempt at a hello-world printed
#
#	sum 1..10 = 75007320
#
# while the same computation returned 55 correctly through a non-variadic call.
#
# This resolves itself in Phase 2b: printf will be V8's own, compiled by v8cc,
# using the same convention as its callers, and V8's varargs.h walks the
# contiguous argument block the prologue spills.  Until then, code compiled by
# v8cc must not call host variadic functions, and the drivers above do their
# printing from the modern-C side.
# ---------------------------------------------------------------------------
