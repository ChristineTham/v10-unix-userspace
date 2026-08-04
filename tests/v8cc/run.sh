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
