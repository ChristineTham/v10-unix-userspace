#!/bin/sh
# V8's own libc, compiled by v8cc, linked freestanding.

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CC=$ROOT/rootfs/bin/cc
V8ROOT=$ROOT/rootfs
export V8ROOT
TMP=${TMPDIR:-/tmp}/libv8c.$$
mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT; cd "$TMP" || exit 1

pass=0 fail=0
LIBC=$ROOT/build/stage0/libc/libv8c.a
CRT=$ROOT/build/stage0/crt0.o
STUBS=$ROOT/build/stage0/v8sys/stubs-freestanding.o
SHIM=$(ls "$ROOT"/build/stage0/v8sys/*.o | grep -v stubs-freestanding | tr '\n' ' ')

run() {	# run <name> <expected>; program source on stdin
	name=$1; want=$2; cat > t.c
	if ! "$CC" -c t.c 2>e.log; then
		fail=$((fail+1)); echo "FAIL $name (compile)"; head -2 e.log; return
	fi
	if ! clang -nostdlib -e _v8start -o t "$CRT" t.o "$LIBC" "$STUBS" $SHIM -lSystem 2>>e.log; then
		fail=$((fail+1)); echo "FAIL $name (link)"; head -2 e.log; return
	fi
	got=$(./t 2>&1)
	if [ "$got" = "$want" ]; then pass=$((pass+1))
	else fail=$((fail+1)); echo "FAIL $name"; echo "  want [$want]"; echo "  got  [$got]"; fi
}

run 'fputs' 'fputs works' <<'EOF'
#include <stdio.h>
main() { fputs("fputs works\n", stdout); fflush(stdout); return 0; }
EOF

run 'printf literal' 'hello' <<'EOF'
#include <stdio.h>
main() { printf("hello\n"); fflush(stdout); return 0; }
EOF

run 'printf integers' 'd=42 n=-17 x=ff o=100 u=7' <<'EOF'
#include <stdio.h>
main() { printf("d=%d n=%d x=%x o=%o u=%u\n", 42, -17, 255, 64, 7);
         fflush(stdout); return 0; }
EOF

run 'printf strings' 's=[v8] c=X' <<'EOF'
#include <stdio.h>
main() { printf("s=[%s] c=%c\n", "v8", 'X'); fflush(stdout); return 0; }
EOF

run 'printf width and padding' '[   42][42   ][00042]' <<'EOF'
#include <stdio.h>
main() { printf("[%5d][%-5d][%05d]\n", 42, 42, 42); fflush(stdout); return 0; }
EOF

run 'malloc' 'malloc ok' <<'EOF'
#include <stdio.h>
char *malloc();
main() {
	char *p; int i;
	p = malloc(100);
	if (p == 0) { printf("malloc failed\n"); fflush(stdout); return 1; }
	for (i = 0; i < 100; i++) p[i] = 'x';
	if (p[99] != 'x') { printf("bad\n"); fflush(stdout); return 1; }
	printf("malloc ok\n"); fflush(stdout); return 0;
}
EOF

run 'malloc many' 'many ok' <<'EOF'
#include <stdio.h>
char *malloc();
main() {
	char *v[64]; int i;
	for (i = 0; i < 64; i++) {
		v[i] = malloc(64 + i * 7);
		if (v[i] == 0) { printf("fail %d\n", i); fflush(stdout); return 1; }
		v[i][0] = i;
	}
	for (i = 0; i < 64; i++) if (v[i][0] != i) { printf("clobber\n"); fflush(stdout); return 1; }
	for (i = 0; i < 64; i += 2) free(v[i]);
	printf("many ok\n"); fflush(stdout); return 0;
}
EOF

run 'printf %f' 'f=3.141590' <<'EOF'
#include <stdio.h>
main() { printf("f=%f\n", 3.14159); fflush(stdout); return 0; }
EOF

run 'string routines' 'len=5 cmp=0 cat=abcdef' <<'EOF'
#include <stdio.h>
main() {
	char b[32];
	strcpy(b, "abc");
	strcat(b, "def");
	printf("len=%d cmp=%d cat=%s\n", strlen("hello"), strcmp("a","a"), b);
	fflush(stdout); return 0;
}
EOF

run 'printf %e' 'e=3.141590e+04' <<'EOF'
#include <stdio.h>
main() { printf("e=%e\n", 31415.9); fflush(stdout); return 0; }
EOF

echo "libv8c: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# FIXED: printf("%f") -- it was lvalues being evaluated twice.
#
# Read-modify-write operators (`x op= y`, `x++`, `--x`) must read the lvalue and
# store back to it. The back end generated the lvalue once for each half, which
# re-ran any side effect inside it. V8's ecvt() rounding loop contains
#
#	if (p1>buf) ++*--p1;
#
# and that decremented p1 twice and incremented through it twice:
#
#	"123" with p1 at [2]  ->  expected "133" p1 at [1]
#	                          got      "323" p1 at [0]
#
# lvaddr()/lvload()/lvstore() in gencode.c now materialise the address once and
# both halves work through it.
#
# How it was found, which is the reusable part: instrumenting cvt() showed BOTH
# compilers entering the rounding tail with identical state --
#
#	TAIL p=8 p1=7 r2=1 dec=1 buf=[31415899]
#
# -- and leaving with different answers, which localised the bug to ten lines.
# Everything else had been eliminated by testing it standalone: the digit loop
# and modf (both give the same 1415899 under clang, so the trailing 899 is just
# binary floating point), 64-bit shifts and masks, `*p1 += 5`, `*p++ = c`, and
# `p1 = &buf[n]; p1 += r2`. What was left was the one construct never tested
# alone -- and it failed the moment it was.
# ---------------------------------------------------------------------------
