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

t 'long arithmetic' '100000000000' <<'EOF'
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

echo
echo "v8ccom: $pass passed, $fail failed"
[ -n "$FAILED" ] && echo "failing:$FAILED"
[ "$fail" -eq 0 ]
