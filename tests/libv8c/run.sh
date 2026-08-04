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
# KNOWN BROKEN: printf("%f") -- 3.14159 prints as 3.146509.
#
# The bug is isolated to a ten-line loop, and the input to that loop is proven
# identical under both compilers. Instrumenting cvt() in src/libc/gen/ecvt.c
# just before its rounding tail:
#
#	TAIL p=8 p1=7 r2=1 dec=1 buf=[31415899]     <- v8cc
#	TAIL p=8 p1=7 r2=1 dec=1 buf=[31415899]     <- clang
#	v8cc  digits=[3146509]
#	clang digits=[3141590]
#
# Same p, same p1, same r2, same decpt, same buffer -- different answers. So
# everything upstream is correct and the fault is entirely in:
#
#	p = p1;
#	*p1 += 5;
#	while (*p1 > '9') {
#		*p1 = '0';
#		if (p1>buf) ++*--p1;
#		else { *p1 = '1'; (*decpt)++; ... }
#	}
#
# Executed by hand on buf=[31415899] with p1=7 this gives 31415900, then
# *p='\0' at p=7 yields 3141590 -- clang's answer. v8cc puts a '6' at buf[3],
# which is '1'+5, so its `*p1 += 5` hit buf[3] rather than buf[7] even though
# p1 printed as 7 immediately before.
#
# RULED OUT already, each tested in isolation: the digit loop and modf (both
# give the identical 1415899 under clang, so that trailing 899 is just binary
# floating point); 64-bit shifts and masks; `*p1 += 5` and `*p++ = c` as
# standalone patterns; and `p1 = &buf[n]; p1 += r2` pointer arithmetic.
#
# What has NOT been tested: the combination in that loop -- `++*--p1`, a
# pre-decrement of a register pointer and a pre-increment through it in one
# expression, and `p = p1` between two register variables. Both p and p1 are
# `register char *` in cvt(). Note that the one back-end bug of this shape
# already found -- gen() handing out a register variable's own register, which
# operators then clobbered -- was exactly this kind of thing, and only showed up
# in real libc code. Start with `++*--p1`.
