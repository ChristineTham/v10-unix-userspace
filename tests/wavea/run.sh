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

echo "wavea: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

# ---------------------------------------------------------------------------
# STILL BROKEN, each recorded rather than left to be rediscovered:
#
#   head, rev  -- build and link, produce no output. Narrowed: stdio INPUT is
#                 the problem, and specifically getc's fast path. A round trip
#
#                     while ((c = getchar()) != EOF) putchar(c);
#
#                 on "abc\n" reads the right NUMBER of characters (4, so
#                 _filbuf and EOF detection work) but writes 'a' then 0x80
#                 three times. The 'a' comes from _flsbuf, taken when _cnt is
#                 still 0; the rest go through getc's fast path
#
#                     (--(p)->_cnt>=0 ? (int)*(p)->_ptr++ : _filbuf(p))
#
#                 which is a post-increment used as a VALUE rather than as an
#                 lvalue. putc's mirror image of this was the double-evaluation
#                 bug just fixed, so look there first: gen(STAR) over an INCR.
#
#   tr         -- `echo abc | tr a-z A-Z` gives "A000" instead of "ABC", so the
#                 a-z range expansion is filling its table wrongly. tr builds
#                 its translation table with nested loops over char values;
#                 given that signed char is deliberate here (CHSIGN), check
#                 whether the table index is going negative.
#
#   cmp        -- does not compile.
#
#   sum        -- FIXED, works.
#
#   A failed open is silent and exits 0 (see cat nosuchfile). cat does
#   `fprintf(stderr, "cat: "); perror(*argv); retcode = 1;` and loses both.
#   perror() indexes sys_errlist, which V8 builds in libc/gen with a `cc -S`
#   plus an ed script (libc/Makefile) that has not been ported -- an empty
#   table would explain the silence.
