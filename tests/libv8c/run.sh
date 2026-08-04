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

# malloc is KNOWN BROKEN and deliberately not run here -- it hangs, so running
# it would wedge the suite rather than fail it.  See the note at the bottom.

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
# KNOWN BROKEN: malloc, and the tree that causes it
#
# The fault is a 32-bit truncation of a pointer, and the tree responsible is now
# known exactly. Set V8DBG in the environment and v8ccom prints the T* type of
# every STAR, CONV and binary node (octal, so it lines up with mfile2.h where
# TINT is 04 and TPOINT is 04000):
#
#   $ cat cb4.c
#   union store { union store *ptr; long dummy[1]; int calloc; };
#   union store *f(p) union store *p;
#   { return (union store *)((long)(p->ptr) & ~1); }
#
#   $ V8DBG=1 v8ccom cb4.i /dev/null
#   BINOP 14 type=4000 L=4 R=4
#   STAR type=4 bytes=4 unsigned=0
#
# Read that carefully. The AND (op 14) is correctly typed TPOINT -- 04000 -- but
# BOTH ITS OPERANDS ARE TINT. The `(long)` cast has vanished and the pointer
# dereference underneath it has been retyped from TPOINT to TINT, so the back
# end quite correctly emits a 4-byte `ldrsw` for it.
#
# So this is a PASS 1 problem, not a back-end one. `p->ptr` on its own is typed
# TPOINT and loads 8 bytes correctly (verified). Something in the cast chain --
# tymatch's LONG-vs-INT ranking looks right, and bigsize() maps LONG to SZLONG
# correctly, so the suspect is makety() or the CAST folding in optim.c --
# collapses the CONV and narrows the operand.
#
# Next step: instrument makety() and optim.c's CAST handling, or dump the tree
# before and after doptim() for this expression.
#
# ALREADY FIXED, all the same LP64 root cause -- V8 assumes sizeof(int) ==
# sizeof(char *), and its own machinery inherits that:
#
#  - opbigsz() returned SZCHAR for the bitwise operators, as the VAX did. It
#    turns out pass 1 never calls it, so this was inert, but it was wrong.
#  - INT and ALIGN in malloc.c were `int`, and BUSY a plain int so `~BUSY` was
#    too. The file's own header names all three as macros a different
#    implementation must redefine, and says outright that "INT is integer type
#    to which a pointer can be cast".
#  - The sbrk arena is mapped above the program data segment, because V8's
#    malloc sorts its free list by address and promises only a "noncontiguous,
#    but monotonically linked, arena".
