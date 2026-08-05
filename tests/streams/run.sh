#!/bin/sh
# V8's stream machinery -- Dennis Ritchie's, running on ARM64.
#
# src/sys/dev/stream.c is the FIRST BELL LABS KERNEL SOURCE in this port, and
# it is byte-identical to upstream; shim/kern/ is the machine-dependent half it
# compiles against.  So there are two different things to assert here and they
# fail in different ways:
#
#   * THE ENGINE BEHAVES.  Queues, blocks, priority ordering, coalescing,
#     the high-water mark.  A message-passing engine that is subtly wrong does
#     not crash -- it drops or reorders one message in a stream of thousands,
#     which is the hardest kind of bug to see from outside.  tests/streams/probe.c
#     asks it directly rather than through a program.
#
#   * THE SEAM IS WHERE IT SAYS IT IS.  stream.c must still hash to upstream's
#     blob, the archive must import nothing from the host, and spl6/splx must
#     actually defer -- a pair that did nothing would pass every behavioural
#     case in the file.

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=$ROOT/build/stage0
KERN=$BUILD/kern/libv8kern.a
TMP=${TMPDIR:-/tmp}/streams.$$
mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT

pass=0 fail=0
check() {
	if [ "$2" = "$3" ]; then pass=$((pass+1))
	else fail=$((fail+1)); echo "FAIL $1"; echo "  want [$2]"; echo "  got  [$3]"; fi
}
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL $1"; shift; [ $# -gt 0 ] && echo "    $*"; }

[ -e "$KERN" ] || { echo "missing $KERN -- run make"; exit 1; }

# The probe is 1985 K&R code linked against 1985 K&R code, so it is compiled in
# that dialect, exactly as the Makefile compiles stream.c.
KFLAGS="-std=gnu89 -Wall -Wno-implicit-int -Wno-implicit-function-declaration
        -Wno-deprecated-non-prototype -Wno-parentheses -Wno-return-type
        -Wno-char-subscripts"
if ! clang $KFLAGS -o "$TMP/probe" "$ROOT/tests/streams/probe.c" "$KERN" \
     > "$TMP/build.log" 2>&1; then
	# ld warns about __common alignment because blkdata[] is 36 KB; not an error.
	grep -qv 'reducing alignment' "$TMP/build.log" &&
		{ echo "probe build failed:"; head -5 "$TMP/build.log"; exit 1; }
fi
"$TMP/probe" > "$TMP/out" 2>"$TMP/err" || bad "probe exited nonzero" "$(head -3 "$TMP/err")"
v() { awk -v k="$1" '$1==k {$1=""; sub(/^ /,""); print}' "$TMP/out"; }

# --- qinit built one freelist per size class ------------------------------
# From src/sys/research/sparam.h, which is Bell Labs' own configuration for the
# machine V8 was developed on -- not a number this port chose.
check "the 4-byte freelist is NBLK4 long"    "64"  "$(v free4)"
check "the 16-byte freelist is NBLK16 long"  "200" "$(v free16)"
check "the 64-byte freelist is NBLK64 long"  "200" "$(v free64)"
check "the 1K freelist is NBLKBIG long"      "20"  "$(v freebig)"

# --- allocb picks a class by size, and lim is the class's REAL size --------
# rbsize[] and bsize[] differ for the big class: 1024 bytes of storage, but
# accounted as 250 against a queue's limit. Confusing the two would make a queue
# fill four times too early, silently.
check "3 bytes takes a class-0 block, 4 wide"      "0 4"    "$(v cls3) $(v lim3)"
check "16 bytes takes a class-1 block, 16 wide"    "1 16"   "$(v cls16) $(v lim16)"
check "40 bytes takes a class-2 block, 64 wide"    "2 64"   "$(v cls40) $(v lim40)"
check "900 bytes takes a class-3 block, 1024 wide" "3 1024" "$(v cls900) $(v lim900)"
check "a fresh block is M_DATA" "0" "$(v newtype)"
check "freeb returns a block to its own class" "1" "$(v freed4delta)"

# --- ordering ---------------------------------------------------------------
check "getq is FIFO" "ABC" "$(v fifo)"
# The whole reason M_FLUSH and M_HANGUP work on a backed-up queue: putq inserts
# type >= QPCTL after any other priority messages but ahead of all data.
check "priority messages overtake queued data, in their own order" \
	"STab" "$(v pri)"
check "putbq goes to the front, but still behind priority" "P12" "$(v putbq)"

# --- coalescing, which is also the only call to bcopy in stream.c -----------
# Two partial M_DATA blocks in a row become one.  This is why a queue of
# single-character terminal input does not cost one block per character -- and
# if bcopy were wrong, or wrong-way-round, this is where it would show.
check "adjacent partial data blocks coalesce" "helloworld" "$(v coalesced)"
check "...into exactly one block" "1" "$(v coalescedrest)"

# --- flushing and the high-water mark ---------------------------------------
check "a queue with blocks on it has a count" "1" "$(v beforeflush)"
check "flushq empties it" "0" "$(v afterflush)"
check "and gives every block back" "1" "$(v flushreturned)"
check "QFULL is clear below the limit" "0" "$(v fullbefore)"
check "QFULL is set at the limit" "1" "$(v fullafter)"
check "and the count is in bsize units, not bytes" "40" "$(v fullcount)"

# --- the plumbing between queues --------------------------------------------
check "backq crosses the pair, follows next, and crosses back" "1" "$(v backq)"
check "allocq hands out a reader/writer pair" "1 1" "$(v qpair)"
check "OTHERQ inverts in both directions" "1 1" "$(v otherq)"

# --- the producers ----------------------------------------------------------
check "putctl queues a control message through the queue's own putp" "2" "$(v putctl)"
check "qpctl does it via putq instead" "66" "$(v qpctl)"
check "putd appends one byte" "z1" "$(v putd)"
check "putcpy copies a literal record" "abcdef" "$(v putcpy)"

# --- the service-procedure scheduler ----------------------------------------
check "qenable at level 0 runs the service procedure" "1" "$(v servedimmediate)"

# THE MACHINE-DEPENDENT HALF'S WHOLE CLAIM.  On the VAX, setqsched() was
# mtpr(SIRR, 1) -- request a software interrupt at IPL 1, which the processor
# held off until the priority level dropped.  Here spl6/splx are a nesting
# counter and setqsched consults it, so the deferral survives the move off the
# hardware.  These two cases are the only ones in this file that an spl6/splx
# pair implemented as no-ops would fail; everything else would still pass.
check "qenable inside spl6 does NOT run it yet"  "0" "$(v deferredinside)"
check "the matching splx releases it"            "1" "$(v deferredafter)"
check "an inner splx does not release an outer section" "0" "$(v nestedinner)"
check "the outer one does"                              "1" "$(v nestedouter)"

# --- allocation is conserved ------------------------------------------------
# Every block taken above has been given back, so the freelists are the lengths
# qinit made them.  A leak here is invisible until a long-running stream user
# panics with "allocb out of blocks", hours later and nowhere near the cause.
check "no block leaked from the 4-byte class"  "64"  "$(v conserved4)"
check "...the 16-byte class"                   "200" "$(v conserved16)"
check "...the 64-byte class"                   "200" "$(v conserved64)"
check "...or the 1K class"                     "20"  "$(v conservedbig)"

# --- the one privileged register this machine has, and the rest -------------
# setqsched() is mtpr(SIRR, 1) and that is the whole of the VAX in stream.h.
# Any OTHER register describes hardware that is not here, and answering quietly
# is how a machine-dependent gap becomes a program that runs and is wrong -- so
# it panics.  Separate program, because a panic exits.
#
# It also checks that panic FORMATS.  Every panic in stream.c is a plain string,
# so the first version of panic() printed its format verbatim and looked right
# against every authentic caller; this one, written afterwards, printed a
# literal `%x' where the register number should have been.  The message with the
# number is the whole reason to have the message.
cat > "$TMP/mtpr.c" <<'EOF'
#include "../../shim/kern/h/param.h"
#include "../../src/sys/h/stream.h"
#undef printf
main() { v8k_mtpr(0x11, 1); printf("SURVIVED\n"); return 0; }
EOF
cp "$TMP/mtpr.c" "$ROOT/tests/streams/.mtpr.c"
if clang $KFLAGS -w -o "$TMP/mtpr" "$ROOT/tests/streams/.mtpr.c" "$KERN" 2>/dev/null; then
	out=$("$TMP/mtpr" 2>&1); rc=$?
	check "an unknown privileged register panics" \
		"panic: mtpr to register 11, which this machine does not have" "$out"
	check "...and does not return to the caller" "2" "$rc"
else bad "mtpr probe build"; fi
rm -f "$ROOT/tests/streams/.mtpr.c"

# --- THE SEAM: stream.c is upstream's file, unmodified ----------------------
# The strongest claim this port can make about a source file, and it is checked
# rather than asserted.  If a machine dependency is ever handled by editing
# stream.c instead of by the header beside it, this is what says so.
prov=$(awk '$2 == "v8/usr/sys/dev/stream.c" {print $1}' "$ROOT/src/sys/dev/PROVENANCE")
here=$(git -C "$ROOT" hash-object src/sys/dev/stream.c)
check "src/sys/dev/stream.c still hashes to pristine V8" "$prov" "$here"

# --- ...and the archive takes nothing from the host -------------------------
# stream.c calls bcopy, and NEITHER libv8c NOR libv8sys defines one -- so an
# omission here would not fail the link, it would resolve out of libSystem and
# leave Bell Labs' stream engine copying messages with Apple's code.  That is
# the class tests/kmemu sweeps the rootfs for; this is the same check one level
# down, on the archive itself.
#
# memcpy is the single permitted external and it is not written by anyone here:
# clang emits it for the struct assignments in allocq().  It resolves to V8's
# OWN memcpy, which libv8c defines -- asserted below, because "it links" does
# not say whose.
undef=$(nm -u "$KERN" 2>/dev/null | grep -v '^$' | grep -v '\.o:$' |
	grep -vE '^_(panic|panicstr|qinit|queuerun|queueflag|spl6|splx|v8k_)' |
	sort -u | tr '\n' ' ' | sed 's/ $//')
check "the archive's only external is memcpy" "_memcpy" "$undef"
nm "$ROOT/build/stage0/libc/libv8c.a" 2>/dev/null | grep -q ' T _memcpy' && ok ||
	bad "libv8c defines memcpy, so the compiler-emitted call is V8's own"

# --- and stream.c does NOT reach into libv8sys or libv8c --------------------
# A negative control.  The kernel half is meant to be self-contained above the
# raw syscalls; if it ever starts calling the shim's v8s_* entry points, the
# layering has inverted and the archive stops being linkable on its own.
nm -u "$KERN" 2>/dev/null | grep -q '_v8s_' &&
	bad "the kernel archive calls into libv8sys" || ok

echo "streams: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
