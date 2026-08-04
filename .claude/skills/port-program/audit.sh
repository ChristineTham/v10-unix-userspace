#!/bin/sh
# Mechanical pre-build audit for a freshly imported V8 program.
#
#   .claude/skills/port-program/audit.sh src/cmd/NAME
#
# Finds only what greps can find honestly.  It is a checklist that cannot be
# skipped, not a substitute for reading the code -- use the lp64-auditor
# subagent for judgement.

dir=$1
[ -n "$dir" ] || { echo "usage: audit.sh src/cmd/NAME" >&2; exit 2; }
[ -d "$dir" ] || { echo "no such directory: $dir" >&2; exit 2; }

echo "=== $dir ==="
echo

# ---------------------------------------------------------------------------
echo "--- 1. #included files that are NOT headers -----------------------------"
echo "    Invisible to every dependency scanner AND to a *.c glob.  Each one"
echo "    must be named explicitly in the Makefile and added to tests/deps."
echo "    Note the [ \\t]* -- V8 writes '# include' with a space, so a pattern"
echo "    anchored on '#include' silently finds nothing."
echo
grep -rhnE '#[ 	]*include[ 	]*"[^"]*"' "$dir" 2>/dev/null |
	grep -v '\.h"' | sed 's/.*"\(.*\)".*/      \1/' | sort -u
echo

# ---------------------------------------------------------------------------
echo "--- 2. pointer-returning functions: declared, or truncated to int? ------"
echo "    A declaration that IS present is correct and needs no action."
echo "    What matters is a call with no declaration in scope."
echo
grep -rn 'char \*[a-z_]*();\|struct [a-z_]* \*[a-z_]*();' "$dir"/*.c "$dir"/*.y 2>/dev/null |
	sed 's/^/      /' | head -20
echo "      (declarations found above; now check for CALLS lacking one)"
echo

# ---------------------------------------------------------------------------
echo "--- 3. the (int)signal idiom -------------------------------------------"
echo "    Usually BENIGN: truncating a function pointer keeps the low bit, and"
echo "    that is all '& 01' wants (SIG_IGN is 1).  Flag only if the result is"
echo "    used as an address."
echo
grep -rn '(int) *signal\|(int)signal' "$dir"/*.c 2>/dev/null | sed 's/^/      /'
echo

# ---------------------------------------------------------------------------
echo "--- 4. file-scope names that collide with libc --------------------------"
echo "    Mach-O resolves COMMON symbols from archives; a.out ld did not.  A"
echo "    tentative array named like a libc function is silently replaced by"
echo "    it -- spell's index[2050] became libc's 156-byte index()."
echo "    A program defining its own FUNCTION of that name is fine."
echo
for n in index rindex abs exp log div time link read write open close; do
	grep -rn "^[a-z_ 	]*\b$n\[" "$dir"/*.c 2>/dev/null |
		sed "s/^/      COLLIDES ($n): /"
done
echo

# ---------------------------------------------------------------------------
echo "--- 5. variadic calls that libc may not supply -------------------------"
echo "    A gap in src/libc does NOT fail the link: it resolves from -lSystem,"
echo "    and for a variadic function that is an ABI mismatch, because v8cc"
echo "    passes arguments in x0-x7 and Apple passes variadic ones on the"
echo "    stack.  This has bitten three times."
echo
for f in printf fprintf sprintf scanf fscanf sscanf execl execle execlp; do
	if grep -rqw "$f" "$dir"/*.c 2>/dev/null; then
		if ls src/libc/*/"$f".c >/dev/null 2>&1; then
			printf '      %-8s used, and src/libc has it\n' "$f"
		else
			printf '      %-8s USED BUT NOT IN src/libc -- check libv8c.a\n' "$f"
		fi
	fi
done
echo

# ---------------------------------------------------------------------------
echo "--- 6. pre-C89 initialisers (will not compile) --------------------------"
grep -rnE '^[a-z_]+ +[a-z_]+ +[0-9]+ *;' "$dir"/*.c 2>/dev/null | sed 's/^/      /'
echo
echo "=== end.  Greps find shapes, not bugs.  Run the lp64-auditor subagent"
echo "=== for the judgement calls, especially cross-file array width mismatches."
