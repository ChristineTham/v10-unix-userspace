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
# KNOWN BROKEN: printf("%f") loses precision -- 3.14159 prints as 3.146509.
#
# %e and %g are correct and share ecvt with it, so it is the fcvt digit loop.
# The same source compiled by clang produces the right digits, so it is codegen.
# V8DBG=1 prints node types (octal, comparable with mfile2.h) and is the tool
# that found the malloc bug; the digit loop in src/libc/gen/ecvt.c is
#
#	while (p<=p1 && p<&buf[NDIG]) { arg *= 10; arg = modf(arg, &fj);
#					*p++ = (int)fj + '0'; }
#
# so the things to look at are the double-to-int conversion and whether `arg`
# stays a double across the loop.
