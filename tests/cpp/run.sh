#!/bin/sh
# Regression tests for stage-0 cpp.
#
# These are behaviour tests, not build tests: every bug found while bootstrapping
# cpp compiled cleanly and failed only at runtime.
#
#   tests/cpp/run.sh [path-to-cpp]

CPP=${1:-build/stage0/cpp/cpp}
V8INC=third_party/Research-Unix-v8/v8/usr/include
TMP=${TMPDIR:-/tmp}/cpptest.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

pass=0 fail=0

# check <name> <expected-substring> -- reads program text on stdin
check() {
	name=$1 want=$2
	cat > "$TMP/in.c"
	got=$("$CPP" -P "$TMP/in.c" 2>&1)
	if echo "$got" | grep -q -- "$want"; then
		pass=$((pass+1))
	else
		fail=$((fail+1))
		echo "FAIL: $name"
		echo "  want substring: $want"
		echo "  got: $(echo "$got" | tr '\n' '|')"
	fi
}

# checknot <name> <forbidden-substring>
checknot() {
	name=$1 bad=$2
	cat > "$TMP/in.c"
	got=$("$CPP" -P "$TMP/in.c" 2>&1)
	if echo "$got" | grep -q -- "$bad"; then
		fail=$((fail+1))
		echo "FAIL: $name -- found forbidden '$bad'"
	else
		pass=$((pass+1))
	fi
}

check 'object macro' 'bar' <<'EOF'
#define FOO bar
FOO
EOF

check 'function macro' '((1)+(2))' <<'EOF'
#define ADD(a,b) ((a)+(b))
ADD(1,2)
EOF

# The #if expression grammar (cpy.y) -- precedence, division, && and !
check 'if arithmetic precedence' 'SEVEN_OK' <<'EOF'
#if 1+2*3 == 7
SEVEN_OK
#endif
EOF

check 'if logical ops' 'ARITH_OK' <<'EOF'
#if (4/2) == 2 && !0
ARITH_OK
EOF

# An undefined identifier in #if must evaluate to 0, not raise
# "Illegal character".  This is the yylex.c COFF-bias regression: when yylex.c
# and cpp.c disagree about the signed-char table offset, identifiers are still
# recognised in normal text but silently stop being recognised inside #if.
check 'undefined identifier in if is 0' 'UNDEF_IS_ZERO' <<'EOF'
#if UNDEFINED_THING
BAD
#else
UNDEF_IS_ZERO
#endif
EOF

check 'defined() operator' 'IS_DEFINED' <<'EOF'
#define HAVE_IT 1
#if defined(HAVE_IT)
IS_DEFINED
#endif
EOF

# We target ARM64, not a VAX.  If cpp announces 'vax', every program it
# preprocesses takes VAX code paths -- including inline VAX assembly.
check 'predefines unix'  'have_unix'  <<'EOF'
#ifdef unix
have_unix
#endif
EOF

check 'predefines arm64' 'have_arm64' <<'EOF'
#ifdef arm64
have_arm64
#endif
EOF

checknot 'does NOT predefine vax' 'HAVE_VAX' <<'EOF'
#ifdef vax
HAVE_VAX
#endif
EOF

# LP64: pperror's varargs were implicitly int and every caller passes char *.
# Truncation shows up only here, in the text of an error message.
echo '#include "totally-missing-header.h"' > "$TMP/missing.c"
if "$CPP" -P "$TMP/missing.c" 2>&1 | grep -q 'totally-missing-header.h'; then
	pass=$((pass+1))
else
	fail=$((fail+1))
	echo "FAIL: LP64 pperror -- filename mangled in error message"
	"$CPP" -P "$TMP/missing.c" 2>&1 | head -2
fi

if "$CPP" -Zbogus "$TMP/missing.c" 2>&1 >/dev/null | grep -q 'Zbogus'; then
	pass=$((pass+1))
else
	fail=$((fail+1)); echo "FAIL: LP64 pperror -- flag name mangled"
fi

# Short pperror calls (fewer args than the definition declares) must not crash.
printf '#endif\n' > "$TMP/ifless.c"
if "$CPP" -P "$TMP/ifless.c" 2>&1 | grep -q 'If-less endif'; then
	pass=$((pass+1))
else
	fail=$((fail+1)); echo "FAIL: short pperror call path"
fi

# The real job: authentic V8 source through authentic V8 headers, no diagnostics.
if [ -d "$V8INC" ]; then
	err=$("$CPP" -I"$V8INC" src/cmd/cat.c "$TMP/cat.i" 2>&1)
	if [ -n "$err" ]; then
		fail=$((fail+1))
		echo "FAIL: V8 cat.c through V8 headers emitted diagnostics:"
		echo "$err" | head -5
	elif grep -q '_iobuf' "$TMP/cat.i"; then
		pass=$((pass+1))
	else
		fail=$((fail+1)); echo "FAIL: V8 stdio.h did not expand"
	fi
fi

echo "cpp: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
