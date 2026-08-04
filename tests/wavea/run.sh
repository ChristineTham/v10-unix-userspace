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
#                 lvalue.
#
#                 UPDATE: getc's generated code has been read and is CORRECT --
#                 decrement _cnt, store, branch on sign, then load _ptr, save
#                 the old value, increment, store, ldrb through the old. putc's
#                 is correct too. So both macros are right and the fault is
#                 elsewhere in the round trip. Note 0x80 is _IOLBF; _flsbuf sets
#                 that flag when isatty() says the stream is a terminal.
#
#                 isatty is now implemented in the shim (ioctl.c) and is NOT the
#                 cause. But the bug is now pinned exactly. Printing what
#                 getchar() returns:
#
#                     printf 'abc\n' | ./gv   ->  [97][82985088][82985088][...]
#
#                 97 is 'a', returned correctly through _filbuf on the first
#                 call. Every later call takes getc's fast path and yields
#                 82985088 -- a POINTER value, i.e. _ptr rather than *_ptr.
#
#                 getc's code in isolation is correct (verified instruction by
#                 instruction), and putchar with literal characters is correct.
#                 What differs is the context: getc is a CONDITIONAL expression,
#                 and condit() lowers `a ? b : c` into GENBR/GENLAB with both
#                 arms delivering their value through the QNODE pseudo-register,
#                 which is x0. So the fast-path arm is not moving its loaded byte
#                 into x0 -- the value left there is the address instead.
#
#                 ROOT CAUSE FOUND -- it is structural, in gencall(), and it is
#                 not about stdio or QNODE at all.
#
#                 gencall() allocates its outgoing-argument area with a
#                 `sub sp, sp, #N` before the call and releases it with a
#                 matching `add` after. That is only correct when control flows
#                 straight through. condit() lowers `a ? b : c` into branches,
#                 so when a call sits in one arm the sub and the add end up on
#                 DIFFERENT PATHS. Visible directly in the generated main() for
#                 `while ((c = getchar()) != EOF)`:
#
#                     sub sp, sp, #16
#                     b   L21           <- branches away, never restores sp
#
#                 The stack drifts by 16 bytes per iteration, so locals and
#                 spill slots read back as garbage -- which is exactly the
#                 pointer-shaped value getchar() appeared to return.
#
#                 THE FIX: stop adjusting sp per call. Compute the maximum
#                 outgoing-argument area over all calls in the function while
#                 generating its body, reserve that once in the prologue
#                 (emit.c already emits the prologue after the body is captured,
#                 so the maximum is known by then), and address arguments at
#                 fixed offsets from sp. That is what every ABI-conformant
#                 compiler does and it is branch-safe by construction.
#
#                 This also explains why calls worked in every earlier test: the
#                 62 back-end tests and the libc tests all call functions in
#                 straight-line code or in loops without a conditional AROUND
#                 the call. It needs a call inside a ternary arm, which is
#                 exactly what getc and putc are.
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
