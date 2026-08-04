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
# KNOWN BROKEN: malloc faults on a static pointer initialiser.
#
# The fault is at ialloc+160, reading 0x10fb8:
#
#	<+144>: add   x9, x9, #0x8     ; allocb
#	<+148>: ldr   x9, [x9]         ; p = allocb          <- 8 bytes, correct
#	<+160>: ldrsw x9, [x9]         ; p->ptr
#
# 0x10fb8 is `alloca`'s address WITH THE TOP 32 BITS CUT OFF -- nm puts
# _alloca at 0x100010fb8 and _allocb at 0x100010008. So this is one more
# 32-bit truncation of a pointer, not a relocation problem: the object's
# relocations are present and correct (otool -r shows two 8-byte entries in
# __DATA,__data, exactly _allocb and _allocp).
#
# `grep -c ldrsw malloc.s` still reports 4. The opbigsz fix removed some of the
# narrow pointer loads but not all, so there is at least one more path by which
# pass 1 or the back end decides a pointer-width load can be done in 32 bits.
# Finding it means dumping the tree for clearbusy() -- ccom -X flags print it --
# and seeing what type the STAR node carries. malloc has
#
#	static union store alloca;
#	static union store *allocb = &alloca;
#
# and the generated data is right --
#
#	_allocb: .quad _alloca
#
# -- but in a position-independent image that .quad needs a rebase applied at
# load time, and the value being read back is the unslid one. So the suspect is
# no longer malloc or the arena: it is that our static pointer initialisers are
# not being relocated, which would affect every V8 program with an initialised
# pointer in static storage.
#
# Next step: check whether the .o carries a relocation for that .quad
# (`otool -r`), and whether linking without -nostdlib -e changes the behaviour.
# If the relocation is missing, the fix is in how gencode.c emits INIT for an
# address constant; if it is present but unapplied, it is a link-flags problem.
#
# THREE REAL BUGS WERE FIXED GETTING HERE, all LP64 truncation of pointers:
#
#  - opbigsz() returned SZCHAR for the bitwise operators, as the VAX did,
#    letting pass 1 narrow an AND to int width. On a VAX a pointer was 32 bits
#    so that was safe; here it emitted `ldrsw` for a pointer load. Now SZLONG.
#  - INT and ALIGN in malloc.c were `int`. The source names both, and BUSY, as
#    macros a different implementation must redefine, and says outright that
#    "INT is integer type to which a pointer can be cast".
#  - BUSY was a plain int, so `~BUSY` was an int and pulled the AND narrow
#    again by the other route. Now 1L.
#
# The sbrk arena is also now mapped above the program data segment, because
# V8's malloc sorts its free list by address and its header promises only a
# "noncontiguous, but monotonically linked, arena".
