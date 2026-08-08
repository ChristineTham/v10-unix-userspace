# trunc-sweep.awk -- find every place a CALL's return value survives into
# arithmetic and is then truncated to 32 bits by arm64_trunc().
#
# Reads `otool -tV' output on stdin and prints one line per site:
#
#	<callee> <address>
#
# WHY THE BINARY AND NOT THE SOURCE.  The property is "was this function
# declared where it was called", and that is not a textual property of the call
# -- the declaration may be in an #include'd non-header, or absent, and either
# way the call site reads the same.  What the compiler DID is visible only in
# what it emitted.  ps -T was found this way and a source grep would not have
# bounded it (PLAN.md S4j).
#
# The shape:
#	bl      _f
#	mov     xN, x0          <- return value into xN
#	...
#	add     xN, xN, #k      <- arithmetic, still xN
#	sxtw    xN, wN          <- arm64_trunc: the top half is gone
#
# CORRECT whenever f really returns int, which is 63 of the 64 sites here.
# FATAL when f returns a pointer: under Mach-O the image loads at 0x100000000,
# so bit 32 is set in every static address and the truncated value is inside
# __PAGEZERO every time.  The caller triages by callee name; see run.sh.

/[ \t]bl[ \t]/ {
	# a call clobbers the caller-saved set, so every tag dies here
	pending = $NF
	delete tag; delete arith
	next
}

/[ \t]mov[ \t]+x[0-9]+, x0[ \t]*$/ {
	if (pending != "") {
		r = $(NF-1); sub(/,$/, "", r)
		tag[r] = pending; arith[r] = 0
		pending = ""
	}
	next
}

/[ \t]sxtw[ \t]+x[0-9]+, w[0-9]+[ \t]*$/ {
	r = $(NF-1); sub(/,$/, "", r)
	w = $NF; sub(/^w/, "x", w)
	if (r == w && (r in tag) && arith[r])
		print tag[r], $1
	delete tag[r]
	next
}

/[ \t](add|sub|mul|lsl|neg|mvn)[ \t]+x[0-9]+,/ {
	for (i = 1; i <= NF; i++)
		if ($i ~ /^(add|sub|mul|lsl|neg|mvn)$/) { r = $(i+1); break }
	sub(/,$/, "", r)
	if (r in tag) arith[r] = 1
	next
}

# anything else writing an x register kills the tag
/[ \t][a-z][a-z0-9.]*[ \t]+x[0-9]+,/ {
	for (i = 2; i <= NF; i++)
		if ($i ~ /^x[0-9]+,$/) { r = $i; sub(/,$/, "", r); delete tag[r]; break }
}

/[ \t](b|b\.[a-z]+|cbz|cbnz|tbz|tbnz|ret)[ \t]?/ { pending = "" }
