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
STUBS=$ROOT/build/stage0/v8sys/libv8stubs.a
# Link the ARCHIVES, never a glob of loose .o files: a stale object left
# behind by a rename once shadowed libv8stubs.a entirely, and the suites
# passed while testing the old code.
SHIM=$ROOT/build/stage0/v8sys/libv8sys.a

pass=0 fail=0

build() {	# build <name> <source>
	if ! "$CC" -c -o "$TMP/$1.o" "$2" > "$TMP/$1.log" 2>&1; then
		echo "FAIL $1 (compile)"; head -3 "$TMP/$1.log"; return 1
	fi
	if ! clang -nostdlib -e _v8start -o "$TMP/$1" "$CRT" "$TMP/$1.o" \
	    "$LIBC" "$STUBS" "$SHIM" -lSystem >> "$TMP/$1.log" 2>&1; then
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
	check 'cat stdin pipe' 'piped' "$(printf 'piped\n' | ./cat)"
	check 'cat stdin redirect' 'alpha beta gamma' \
	    "$(./cat < in.txt | tr '\n' ' ' | sed 's/ $//')"
else
	fail=$((fail+4))
fi

# ---- echo ---------------------------------------------------------------
if build echo "$ROOT/src/cmd/echo.c"; then
	check 'echo words' 'hello world' "$(./echo hello world)"
	check 'echo -n'    'nonl'        "$(./echo -n nonl)"
else
	fail=$((fail+2))
fi

# ---- wc -----------------------------------------------------------------
if build wc "$ROOT/src/cmd/wc.c"; then
	check 'wc counts' '3 3 17' "$(./wc < in.txt | tr -s ' ' | sed 's/^ //')"
else
	fail=$((fail+1))
fi

# ---- basename -----------------------------------------------------------
if build basename "$ROOT/src/cmd/basename.c"; then
	check 'basename strips dir'    'cat.c' "$(./basename /usr/src/cmd/cat.c)"
	check 'basename strips suffix' 'cat'   "$(./basename /usr/src/cmd/cat.c .c)"
else
	fail=$((fail+2))
fi

# ---- tee ----------------------------------------------------------------
if build tee "$ROOT/src/cmd/tee.c"; then
	check 'tee to file and stdout' 'hi hi' \
	    "$(printf 'hi\n' | ./tee t.out | cat - t.out | tr '\n' ' ' | sed 's/ $//')"
else
	fail=$((fail+1))
fi

# ---- yes ----------------------------------------------------------------
if build yes "$ROOT/src/cmd/yes.c"; then
	check 'yes repeats' 'y y y' "$(./yes 2>/dev/null | head -3 | tr '\n' ' ' | sed 's/ $//')"
else
	fail=$((fail+1))
fi

# ---- rev ----------------------------------------------------------------
if build rev "$ROOT/src/cmd/rev.c"; then
	check 'rev reverses' 'cba' "$(printf 'abc\n' | ./rev)"
else
	fail=$((fail+1))
fi

# ---- tr -----------------------------------------------------------------
if build tr "$ROOT/src/cmd/tr.c"; then
	check 'tr range'    'ABC' "$(printf 'abc\n' | ./tr a-z A-Z)"
	check 'tr explicit' 'xyz' "$(printf 'abc\n' | ./tr abc xyz)"
	# -d took a SIGBUS until string literals moved to writable data: with one
	# argument the other string is the literal "", and nextc() ends with
	# `if(c==0) *--s->p = 0;` -- a write straight into it.
	check 'tr -d'       'acac' "$(printf 'abcabc\n' | ./tr -d b)"
	check 'tr -cd'      'abcabc' "$(printf 'abc-abc\n' | ./tr -cd abc)"
	# -s squeezes characters that are in STRING2, per V8's tr(1) -- so with
	# no string2 nothing is squeezed, which is not the BSD reading.
	check 'tr -s two strings' 'abc' "$(printf 'aabbcc\n' | ./tr -s a-c a-c)"
	check 'tr -s one string'  'aabbcc' "$(printf 'aabbcc\n' | ./tr -s abc)"
else
	fail=$((fail+6))
fi

# ---- cmp ----------------------------------------------------------------
# cmp needs the ctype table, and it was the first program to want one.
printf 'alpha\nbeta\n' > c1.txt; cp c1.txt c2.txt; printf 'alpha\nbetX\n' > c3.txt
if build cmp "$ROOT/src/cmd/cmp.c"; then
	./cmp c1.txt c2.txt > cmp.out 2>&1
	check 'cmp same (silent)'  ''   "$(cat cmp.out)"
	check 'cmp same (status)'  '0'  "$?"
	check 'cmp differ' 'c1.txt c3.txt differ: char 10, line 2' \
	    "$(./cmp c1.txt c3.txt 2>&1)"
	./cmp -s c1.txt c3.txt 2>/dev/null
	check 'cmp -s status' '1' "$?"
	check 'cmp -l' '10 141 130' "$(./cmp -l c1.txt c3.txt | tr -s ' ' | sed 's/^ //')"
	./cmp c1.txt nosuchfile > /dev/null 2>&1
	check 'cmp missing file status' '2' "$?"
else
	fail=$((fail+6))
fi

# ---- sum ----------------------------------------------------------------
if build sum "$ROOT/src/cmd/sum.c"; then
	check 'sum is stable' "$(./sum < in.txt)" "$(./sum < in.txt)"
else
	fail=$((fail+1))
fi

# ---- getchar/putchar round trip through stdio ---------------------------
cat > rt.c <<'EOF'
#include <stdio.h>
main() { int c; while ((c = getchar()) != EOF) putchar(c); return 0; }
EOF
if build rt rt.c; then
	check 'stdio round trip' 'alpha beta gamma' \
	    "$(./rt < in.txt | tr '\n' ' ' | sed 's/ $//')"
else
	fail=$((fail+1))
fi

# ---- head ---------------------------------------------------------------
if build head "$ROOT/src/cmd/head.c"; then
	check 'head -2'      'alpha beta' "$(./head -2 in.txt | tr '\n' ' ' | sed 's/ $//')"
	check 'head default' 'alpha beta gamma' \
	    "$(./head in.txt | tr '\n' ' ' | sed 's/ $//')"
	check 'head stdin'   'alpha' "$(./head -1 < in.txt)"
else
	fail=$((fail+3))
fi

echo "wavea: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

# ---------------------------------------------------------------------------
# SOLVED, kept because the search was long and the shape of it is the point:
#
#   `cat nosuchfile` printed "cat: input nosuchfile is output" -- its dev/ino
#   guard -- instead of a perror message.
#
#   Six hypotheses were eliminated by direct test, each correct in isolation:
#   argc, fflg, `**++argv`, the `&&`/`||` lowering on cat's exact expression,
#   open()'s return value and errno, and assignment-in-condition against -1 in
#   both `int` and `register int`.
#
#   What finally settled it was instrumenting cat's own decision points instead
#   of writing a seventh test case:
#
#       DBG fi=-1
#       DBG fstat=-1 dev=15 ino=23002 sd=15 si=23002
#       cat: input nosuchfile is output
#
#   open() returned -1 and `fi < 0` was FALSE; fstat() returned -1 and `>= 0`
#   was TRUE. Not one bug in cat's argument loop -- every signed comparison
#   against a syscall result was inverted, and statb still held stdout's stat
#   from the top of main, so the dev/ino guard matched.
#
#   Cause: AAPCS64 defines only w0 for a function returning int; the top half of
#   x0 is unspecified. This back end computes at 64-bit width and compares with
#   an x-form `cmp`, so -1 arriving as 0x00000000ffffffff tested as positive.
#   The whole shim is clang-compiled, so EVERY syscall error check in EVERY V8
#   program was broken -- and could not show up in a self-contained test,
#   because v8cc's own callees return a properly extended x0 and only disagree
#   with foreign ones.
#
#   The first fix -- sign-extend at the call site -- was WRONG, and instructive
#   about why. V8 calls malloc undeclared (opendir.c: `(DIR *)malloc(...)` with
#   nothing in scope), so K&R types the call int, and sign-extending it from 32
#   bits truncated the pointer: opendir went from working to segfaulting. The
#   compiler cannot tell a declared int from an undeclared one, because they are
#   the same node. So the seam adapts instead -- every wrapper in
#   shim/v8sys/stubs.c returns long -- and the compiler narrows only types that
#   must have been declared. The two halves are documented together, in
#   stubs.c and in gencall().
#
#   Worth remembering: the recorded next step was to read the generated assembly
#   and diff it against clang's. Instrumenting the program was faster and
#   pointed straight at the seam.

# ---------------------------------------------------------------------------
# tr and cmp: both were listed here as broken, and neither turned out to be a
# bug in the program.
#
#   `tr -d b` took a SIGBUS.  V8's nextc() ends with `if(c==0) *--s->p = 0;`,
#   pushing the NUL back after reading past the end, and with one argument the
#   other string is the literal "".  String literals were being emitted into
#   __TEXT,__cstring; V8's own VAX back end puts them in `.data 1`/`.data 2`
#   (src/cmd/ccom/vax/lcatch2.c), because 1985 C had no `const` and programs
#   wrote to literals freely.  They now go to writable data here too.
#
#   `cmp` did not compile, then wanted _ctype.  The table is authentic V8 --
#   libc/gen/ctype.c -- and was simply not imported yet.
#
# The general shape again: neither was a defect in the ported program.  One was
# a target-model decision made wrongly, one was a missing library file.
# ---------------------------------------------------------------------------
