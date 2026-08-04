#!/bin/sh
# Wave A: real V8 commands, compiled by v8cc, linked freestanding, run.
#
# These are authentic V8 sources from src/cmd, not test fixtures. Every compiler
# bug found so far has lived in combinations of features that real code uses and
# synthetic tests do not, so this suite leads with real programs by design.

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CC=$ROOT/rootfs/bin/cc
V8ROOT=$ROOT/rootfs
export V8ROOT
TMP=${TMPDIR:-/tmp}/wavea.$$
mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT

LIBC=$ROOT/build/stage0/libc/libv8c.a
CRT=$ROOT/build/stage0/crt0.o
STUBS=$ROOT/build/stage0/v8sys/stubs-freestanding.o
SHIM=$(ls "$ROOT"/build/stage0/v8sys/*.o | grep -v stubs-freestanding | tr '\n' ' ')

pass=0 fail=0

build() {	# build <name> <source>
	if ! "$CC" -c -o "$TMP/$1.o" "$2" > "$TMP/$1.log" 2>&1; then
		echo "FAIL $1 (compile)"; head -3 "$TMP/$1.log"; return 1
	fi
	if ! clang -nostdlib -e _v8start -o "$TMP/$1" "$CRT" "$TMP/$1.o" \
	    "$LIBC" "$STUBS" $SHIM -lSystem >> "$TMP/$1.log" 2>&1; then
		echo "FAIL $1 (link)"; head -3 "$TMP/$1.log"; return 1
	fi
	return 0
}

check() { # check <name> <expected> <actual>
	if [ "$2" = "$3" ]; then pass=$((pass+1))
	else fail=$((fail+1)); echo "FAIL $1"; echo "  want [$2]"; echo "  got  [$3]"; fi
}

cd "$TMP" || exit 1
printf 'alpha\nbeta\ngamma\n' > in.txt

# ---- cat ----------------------------------------------------------------
if build cat "$ROOT/src/cmd/cat.c"; then
	check 'cat one file'  'alpha beta gamma' "$(./cat in.txt | tr '\n' ' ' | sed 's/ $//')"
	check 'cat two files' '6' "$(./cat in.txt in.txt | wc -l | tr -d ' ')"
	# a missing file must at least not produce output or hang
	check 'cat missing is quiet on stdout' '' "$(./cat nosuchfile 2>/dev/null)"
else
	fail=$((fail+3))
fi

echo "wavea: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

# ---------------------------------------------------------------------------
# KNOWN BROKEN 1: a failed open is silent and exits 0.
#
# `cat nosuchfile` prints nothing on stderr and returns 0. cat.c does
#
#	fprintf(stderr, "cat: "); perror(*argv); retcode = 1;
#
# so both the diagnostic and the status are being lost. perror() lives in
# stdio/error.c, which IS linked, but it indexes sys_errlist -- and V8 builds
# that table in libc/gen with a `cc -S` plus an ed script (see libc/Makefile),
# which has not been ported. An empty table would explain the silence. The
# exit status is separate: check whether exit(n) carries its argument, since
# stubs.c currently supplies a placeholder exit() straight to the syscall.
#
# KNOWN BROKEN 2: cat with no file arguments, or with `-`, reads nothing.
#
# `cat file` works; `cat < file` and `printf x | cat` both produce no output,
# so it is the fi=0 path rather than anything about pipes -- the dev/ino
# collision that 16-bit inode folding could cause was the first guess and is
# ruled out, since redirecting from a plain file fails identically.
#
# cat.c forces argc=2 and sets fflg when given no arguments, then takes
# `fi = 0` and fstat()s it. Next: check what fstat(0) returns through the shim,
# and whether cat's "input file is output file" guard is rejecting on the
# folded dev/ino. That guard reads stdout's dev/ino at the top of main.
# ---------------------------------------------------------------------------
