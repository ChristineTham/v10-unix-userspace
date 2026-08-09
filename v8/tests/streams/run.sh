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

# V8'S setjmp, NOT THE HOST'S, and this one object is the whole reason the link
# line is not just $KERN.
#
# shim/kern/sys/slp.c does the setjmp half of streamio.c's three
# `longjmp(u.u_qsav)' calls -- the idiom sys/trap.c:176 uses to abort a system
# call when a signal arrives mid-sleep.  Taking that from <setjmp.h> would
# leave the kernel side of a 1985 stream saving its context with Apple's code,
# in an archive that gets linked into V8 programs; src/include/setjmp.h is this
# port's own, 24 longs for AAPCS64's callee-saved set, implemented in
# compiler/setjmp.s.  So the archive imports _setjmp and _longjmp exactly as it
# imports _memcpy, and all three are asserted below to come from libv8c.
SETJMP=$BUILD/libc/setjmp.o
[ -e "$SETJMP" ] || { echo "missing $SETJMP -- run make"; exit 1; }

# The probe is 1985 K&R code linked against 1985 K&R code, so it is compiled in
# that dialect, exactly as the Makefile compiles stream.c.
KFLAGS="-std=gnu89 -Wall -Wno-implicit-int -Wno-implicit-function-declaration
        -Wno-deprecated-non-prototype -Wno-parentheses -Wno-return-type
        -Wno-char-subscripts -Wno-extra-tokens
        -Wno-incompatible-function-pointer-types"
if ! clang $KFLAGS -o "$TMP/probe" "$ROOT/tests/streams/probe.c" "$KERN" "$SETJMP" \
     > "$TMP/build.log" 2>&1; then
	# ld warns about __common alignment because blkdata[] is 36 KB; not an error.
	grep -qv 'reducing alignment' "$TMP/build.log" &&
		{ echo "probe build failed:"; head -5 "$TMP/build.log"; exit 1; }
fi
"$TMP/probe" > "$TMP/out" 2>"$TMP/err" || bad "probe exited nonzero" "$(head -3 "$TMP/err")"
v() { awk -v k="$1" '$1==k {$1=""; sub(/^ /,""); print}' "$TMP/out"; }

# The syscall side gets its own probe, split the way the source is split -- see
# the header of sioprobe.c.  A DEADLINE, because every failure mode of tsleep
# is a hang rather than a wrong answer, and a hung suite prints nothing at all.
if ! clang $KFLAGS -o "$TMP/sioprobe" "$ROOT/tests/streams/sioprobe.c" \
     "$KERN" "$SETJMP" > "$TMP/siobuild.log" 2>&1; then
	grep -qv 'reducing alignment' "$TMP/siobuild.log" &&
		{ echo "sioprobe build failed:"; head -5 "$TMP/siobuild.log"; exit 1; }
fi
perl -e 'alarm 60; exec @ARGV' "$TMP/sioprobe" > "$TMP/sio" 2>"$TMP/sioerr" ||
	bad "sioprobe exited nonzero" "$(head -3 "$TMP/sioerr")"
s() { awk -v k="$1" '$1==k {$1=""; sub(/^ /,""); print}' "$TMP/sio"; }

# And the line discipline gets a third, for the reason the first two are two:
# probe.c is the stream engine, sioprobe.c the syscall side, ttyprobe.c a module
# riding on both.  No deadline -- nothing here sleeps.
if ! clang $KFLAGS -o "$TMP/ttyprobe" "$ROOT/tests/streams/ttyprobe.c" \
     "$KERN" "$SETJMP" > "$TMP/ttybuild.log" 2>&1; then
	grep -qv 'reducing alignment' "$TMP/ttybuild.log" &&
		{ echo "ttyprobe build failed:"; head -5 "$TMP/ttybuild.log"; exit 1; }
fi
"$TMP/ttyprobe" > "$TMP/tty" 2>"$TMP/ttyerr" ||
	bad "ttyprobe exited nonzero" "$(head -3 "$TMP/ttyerr")"
t() { awk -v k="$1" '$1==k {$1=""; sub(/^ /,""); print}' "$TMP/tty"; }

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

# ===========================================================================
# THE SYSCALL SIDE -- src/sys/sys/streamio.c, PLAN.md section 8a step 1.
# ===========================================================================

# --- stopen builds a stream head and hangs it off the inode -----------------
check "stopen returns NULL on success"      "1" "$(s openret)"
check "...and sets no error"                "0" "$(s openerr)"
check "...and the inode now has a stream"   "1" "$(s attached)"
# pgrp 0 means "no process group yet"; streamio.c:45 reads it that way, which
# is why shim/kern/sys/fio.c folds a host pid to 1..30000 and never to 0.
check "a fresh stream has no process group" "0" "$(s openpgrp)"

# --- a record goes down the stack and comes back up -------------------------
check "stwrite sets no error"                "0"     "$(s writeerr)"
check "stread gets the bytes back"           "hello" "$(s readbuf)"
check "...all five of them"                  "5"     "$(s readn)"
check "...with no error"                     "0"     "$(s readerr)"

# HAZARD 3, MEASURED RATHER THAN ARGUED.  stream.h:69 is `char count' -- "#
# processes in stream routines" -- signed here, incremented unbounded by
# stenter and tested for zero by stexit, so 128 nested entries would make a
# stream that never closes.  src/sys/PORTING.md settles it by counting:
# stenter has six callers, all system-call entry points, none reachable from
# another and none stored in a qinit.  This is the observable half -- the
# driver's put procedure runs while the process is still inside stwrite, so it
# sees exactly 1, and 0 once the call returns.
check "one process inside a stream counts 1" "1" "$(s insidecount)"
check "...and 0 once the call returns"       "0" "$(s outsidecount)"

# --- stioctl ----------------------------------------------------------------
check "FIONREAD reports the queued bytes" "4" "$(s fionread)"
check "...with no error"                  "0" "$(s fionreaderr)"
# streamio.c:562 is the one copyout in stioctl with NO null check on arg, and
# that is not an oversight: on a VAX a copyout to user address 0 landed in the
# read-only text segment, faulted, and came back -1, so the ioctl returned
# EFAULT.  shim/kern/sys/subr.c's copyout rejects NULL to reproduce the ANSWER
# rather than the absence of the fault -- without it this is a SIGSEGV inside
# the kernel archive.
check "FIONREAD with a null argument is EFAULT" "14" "$(s fionreadnull)"
check "a second record still round-trips"  "abcd" "$(s drained)"

check "TIOCSPGRP with arg 0 adopts this process" "1" "$(s spgrp)"
check "...and records the controlling device"    "1" "$(s ttydev)"
check "...and its inode number"                  "1" "$(s ttyino)"
check "TIOCGPGRP returns the group"              "1" "$(s gpgrplow)"
# INHERITED, DELIBERATELY.  TIOCGPGRP copies sizeof(stq->pgrp) -- two bytes --
# into a user int, leaving the top half stale.  The VAX copied two bytes there
# too, so unlike the :713 copyout below there is no coincidence to undo and the
# fidelity contract says reproduce it.  Asserted so a future "fix" is a
# decision rather than a slip.  src/sys/PORTING.md hazard 2.
check "...and leaves the user int's top half alone" "1" "$(s gpgrphigh)"
check "TIOCEXCL sets exclusive use"   "1" "$(s excl)"
check "TIOCNXCL clears it"            "1" "$(s nxcl)"

# --- the module stack -------------------------------------------------------
# src/sys/PORTING.md named qattach/qdetach as "exactly what tests/streams
# cannot reach today", because pushing a line discipline needs streamio.c.
# This is that gap closed.
check "v8k_stconf appends contiguously" "0 1" "$(s ld0) $(s ld1)"
check "FIOPUSHLD reports no error"      "0"   "$(s pusherr)"
check "...and the discipline is on the stack" "1" "$(s pushed)"
check "FIOLOOKLD finds it, by number"   "1"   "$(s lookval)"
check "...with no error"                "0"   "$(s lookerr)"

# THE RECORDED DEVIATION, AND THIS IS WHAT CHECKS IT.  Upstream's :713 is
# `copyout((caddr_t)&fmt, arg, sizeof(arg))' -- fmt is int (:549), arg is
# caddr_t (:543).  Four bytes on a VAX by coincidence; EIGHT here, so it reads
# past a 4-byte object and writes eight bytes into user memory.  The import
# changes it to sizeof(fmt).  A sentinel in the high half of the user object
# must survive, and does not under upstream's line.
check "FIOLOOKLD writes the number through the argument" "1" "$(s lookarg)"
check "...and exactly four bytes of it"                  "1" "$(s lookhigh)"

check "FIOPOPLD reports no error"        "0"  "$(s poperr)"
check "...and the discipline is gone"    "1"  "$(s popped)"
check "an unconfigured discipline is EINVAL" "22" "$(s pushbad)"

# --- file passing -----------------------------------------------------------
check "the file table hands out an entry" "1" "$(s falloc)"
# ENXIO IS UPSTREAM'S ANSWER, not a gap here.  sndfile (:926) walks the write
# chain looking for a queue whose qinfo is &strdata -- a stream HEAD -- and a
# stream with a device at the bottom has only one.  The topology that has two
# is pipe(2), which sys/pipe.c builds by cross-connecting two streams and which
# this port answers with the host's pipe instead.  src/sys/PORTING.md.
check "FIOSNDFD on a device-backed stream is ENXIO" "6" "$(s sndfd)"
check "FIORCVFD takes the passed file"     "0"   "$(s rcvfderr)"
check "...into the lowest free descriptor" "0"   "$(s rcvfd)"
check "...and copies out the credentials"  "101 202" "$(s rcvuid) $(s rcvgid)"
check "...and the descriptor really points at it" "1" "$(s rcvslot)"

# --- tsleep, over a real host descriptor ------------------------------------
# THE DESIGN THAT UNBLOCKED THIS WHOLE STEP.  A V8 stream's driver end is a
# host descriptor; tsleep is queuerun() then poll() on it, and wakeup records
# that a producer ran.  A pipe is the smallest device one process can drive.
check "a driver registers its descriptor"   "1"        "$(s ndrv)"
check "stread blocks in poll and gets the device's bytes" "device" "$(s drvread)"
check "...and the driver can withdraw"      "0"        "$(s ndrvafter)"
# tsleep's third argument is SECONDS -- upstream stores it in p_tsleep and
# sys/clock.c:315 decrements it once a second.  Reading it as ticks would turn
# stioctl's fifteen-second ack timeout into fifteen ticks and every ioctl
# through a module into an EIO.
check "a timed sleep with no device times out" "1" "$(s tsleeptime)"

# --- hangup reaches out of the stream and into the file table ---------------
check "M_HANGUP marks the stream"        "1" "$(s hungup)"
check "...and poisons the open file"     "1" "$(s fhungup)"
check "...clearing FWRITE"               "1" "$(s fnowrite)"
# and NOT FREAD: a reader must still be able to drain what the device queued
# before it vanished.  strput passes FWRITE alone; only stclose passes both.
check "...but leaving FREAD, so the last output can be read" "1" "$(s freadkept)"
check "a write to a hung-up stream is ENXIO" "6" "$(s writehung)"

check "stclose detaches the stream" "1" "$(s closed)"
check "no block leaked from the 4-byte class through all of that" "1" "$(s conserved4)"
check "...the 16-byte class"  "1" "$(s conserved16)"
check "...the 64-byte class"  "1" "$(s conserved64)"
check "...or the 1K class"    "1" "$(s conservedbig)"

# --- hazard 4's three widths, as sizes rather than as prose -----------------
# The /proc struct user freezes u_procp, u_qsav and u_ofile at VAX widths
# because a V8 program reads that layout; the kernel-side one needs LP64.  That
# is why there are two, and enumerating it is what found the u_ofile defect in
# shim/libkmemu/procfs.c.  A sizeof on the member is the guard an
# offset-plus-total-size pair cannot be.
check "u_ofile is NOFILE LP64 pointers"       "1024" "$(s ofilesize)"
check "u_qsav is a jump buffer this machine can use" "192" "$(s qsavsize)"
# ...and the two that stay at UPSTREAM's width, because they cross nothing.
# h/proc.h:28-29 declare both short, so on a VAX the field was exactly as wide
# as a process id.  The narrowing is this port's own widening for ps(1).
check "the kernel-side p_pid keeps upstream's short" "2" "$(s pidsize)"
check "and so does stream.h's pgrp"                  "2" "$(s pgrpsize)"
check "so the shim hands it an id in a VAX pid's range" "1" "$(s pidrange)"

# --- two signals, each in its own program because they are fatal ------------
# strput's M_HANGUP arm gsignals SIGHUP to the stream's process group, and
# stwrite psignals SIGPIPE to the writer.  Both are DELIVERY, which the survey
# listed as already working here -- but "signal numbering translates" is not
# "a handler runs", which is a distinction tests/v8sys learned the hard way.
sigcase() {	# name, ioctl-or-action snippet, expected message
	cat > "$ROOT/tests/streams/.sig.c" <<EOF
#include "../../shim/kern/h/param.h"
#undef printf
#undef bcopy
#undef psignal
#undef longjmp
#include <stdio.h>
#include <signal.h>
#include "../../src/sys/h/stream.h"
#include "../../shim/kern/h/proc.h"
#include "../../shim/kern/h/user.h"
#include "../../src/sys/h/inode.h"
#include "../../shim/kern/h/conf.h"
int qreply(), putq(), putctl();
static int lput(struct queue *q, struct block *bp) { qreply(q, bp); return 0; }
static long lopen(struct queue *q, int d) { return 1; }
static int lclose(struct queue *q) { return 0; }
static struct qinit rd = { putq, 0, lopen, lclose, 512, 256 };
static struct qinit wr = { lput, 0, lopen, lclose, 512, 256 };
static struct streamtab info = { &rd, &wr };
static struct inode ino;
static void caught(int s) { printf("CAUGHT %d\n", s); exit(0); }
main() {
	v8k_streaminit(); v8k_procinit();
	ino.i_count = 1; ino.i_number = 1; ino.i_sptr = 0;
	stopen(&info, 0, 0, &ino);
	signal(SIGHUP, caught); signal(SIGPIPE, caught);
	$2
	printf("NOSIGNAL\n");
	return 0;
}
EOF
	if clang $KFLAGS -w -o "$TMP/sig" "$ROOT/tests/streams/.sig.c" "$KERN" "$SETJMP" \
	     2>/dev/null; then
		check "$1" "$3" "$(perl -e 'alarm 20; exec @ARGV' "$TMP/sig" 2>&1)"
	else bad "$1 (build)"; fi
	rm -f "$ROOT/tests/streams/.sig.c"
}
sigcase "M_HANGUP signals the stream's process group" \
	'stioctl(&ino, (("t"[0]<<8)|118), (caddr_t)0); putctl(ino.i_sptr->wrq->next, 2);' \
	"CAUGHT 1"
sigcase "a write to a hung-up stream raises SIGPIPE" \
	'ino.i_sptr->flag |= HUNGUP; u.u_base="x"; u.u_count=1; stwrite(&ino);' \
	"CAUGHT 13"

# --- and the one configuration tsleep refuses to pretend about --------------
# Separate program, because a panic exits.  With no device registered and no
# timeout, nothing can wake the sleeper and shim/kern/sys/slp.c says so instead
# of spinning, inventing a timeout, or hanging in poll(NULL, 0, -1).
cat > "$ROOT/tests/streams/.deadlock.c" <<'EOF'
#include "../../shim/kern/h/param.h"
#undef printf
#include <stdio.h>
main() { tsleep((caddr_t)0, 0, 0); printf("SURVIVED\n"); return 0; }
EOF
if clang $KFLAGS -w -o "$TMP/dl" "$ROOT/tests/streams/.deadlock.c" "$KERN" "$SETJMP" \
     2>/dev/null; then
	out=$(perl -e 'alarm 20; exec @ARGV' "$TMP/dl" 2>&1); rc=$?
	check "an untimed sleep with no device panics" \
		"panic: tsleep: no device below, and no timeout" "$out"
	check "...and does not return to the caller" "2" "$rc"
else bad "deadlock probe build"; fi
rm -f "$ROOT/tests/streams/.deadlock.c"

# --- THE SEAM: stream.c is upstream's file, unmodified ----------------------
# The strongest claim this port can make about a source file, and it is checked
# rather than asserted.  If a machine dependency is ever handled by editing
# stream.c instead of by the header beside it, this is what says so.
prov=$(awk '$2 == "v8/usr/sys/dev/stream.c" {print $1}' "$ROOT/src/sys/dev/PROVENANCE")
here=$(git -C "$ROOT" hash-object src/sys/dev/stream.c)
check "src/sys/dev/stream.c still hashes to pristine V8" "$prov" "$here"

# ttyld.c and partab.c are in the same position and get the same guard, which
# is the point of putting NTTY in shim/kern/dev/tty.h: the discipline needed a
# number the shipped tree does not contain, and it still took NO edit to Bell
# Labs' source to supply it.  The day someone answers a machine dependency by
# touching ttyld.c instead of the header beside it, these go red.
prov=$(awk '$2 == "v8/usr/sys/dev/ttyld.c" {print $1}' "$ROOT/src/sys/dev/PROVENANCE")
here=$(git -C "$ROOT" hash-object src/sys/dev/ttyld.c)
check "src/sys/dev/ttyld.c still hashes to pristine V8" "$prov" "$here"

prov=$(awk '$2 == "v8/usr/sys/sys/partab.c" {print $1}' "$ROOT/src/sys/sys/PROVENANCE")
here=$(git -C "$ROOT" hash-object src/sys/sys/partab.c)
check "src/sys/sys/partab.c still hashes to pristine V8" "$prov" "$here"

# --- and streamio.c differs by EXACTLY the two recorded deviations ----------
# stream.c can be checked by hash because nothing in it changed.  streamio.c
# cannot: it carries two target-forced deviations, so the guard has to be the
# DIFF rather than the hash, or the whole file goes unwatched the moment one
# line is allowed to move.
#
# Both are LP64, both are argued from a sibling one function away, and both are
# written up in src/sys/PORTING.md.  What this asserts is that upstream lost
# exactly two lines and that they are those two -- an added PORT comment is
# fine, a third changed line is not.
UP=$ROOT/../third_party/Research-Unix-v8/v8/usr/sys/sys/streamio.c
uprov=$(awk '$2 == "v8/usr/sys/sys/streamio.c" {print $1}' "$ROOT/src/sys/sys/PROVENANCE")
uphash=$(git -C "$ROOT" hash-object "$UP" 2>/dev/null)
check "third_party's streamio.c is still pristine" "$uprov" "$uphash"

# THE TWO DEVIATIONS ARE NOT THE SAME SHAPE, which is why this counts removals
# and additions separately rather than counting "changed lines".  :713 REPLACES
# a line -- sizeof(arg) becomes sizeof(fmt).  urcvfile ADDS one -- upstream
# declares only stq, so the fix is a declaration that was never there.  A first
# draft of this check asserted two removals and failed, correctly.
#
# Removed lines only; the PORT comments we add are additions and are not the
# subject.  `diff' output, '<' side, minus the header.
diff "$UP" "$ROOT/src/sys/sys/streamio.c" | sed -n 's/^< //p' |
	sed 's/^[ 	]*//;s/[ 	]*$//' | grep -v '^$' > "$TMP/gone"
check "exactly one upstream line is gone" "1" "$(grep -c . < "$TMP/gone")"
check "...and it is the sizeof(arg) copyout" "1" \
	"$(grep -c 'copyout((caddr_t)&fmt, arg, sizeof(arg)))' < "$TMP/gone")"

# The replacements, so a deviation cannot be silently widened either.
check "the copyout now names its object" "1" \
	"$(grep -c 'copyout((caddr_t)&fmt, arg, sizeof(fmt)))' "$ROOT/src/sys/sys/streamio.c")"
# Scoped to urcvfile's own parameter list: `caddr_t arg;' appears three times
# in the file, and the other two are stioctl's and usndfile's, both upstream's.
# usndfile is the twin that made the omission legible in the first place.
check "urcvfile now declares arg a caddr_t" "1" \
	"$(sed -n '/^urcvfile(stq, arg)$/,/^{/p' "$ROOT/src/sys/sys/streamio.c" |
	   grep -c '^caddr_t arg;')"
check "...and the twin that always did still does" "1" \
	"$(sed -n '/^usndfile(stq, arg)$/,/^{/p' "$ROOT/src/sys/sys/streamio.c" |
	   grep -c '^caddr_t arg;')"

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
# SUBTRACTED RATHER THAN FILTERED, and the change is the point.  This used to
# grep away a hand-written list of names the archive defines -- fine while
# there were two objects, and a list that would have had to grow by every name
# streamio.c and shim/kern/sys/ export.  A name-by-name allow list is exactly
# how tests/kmemu's allowed leaks went stale.  So: everything the archive
# UNDEFINES, minus everything the archive DEFINES, is what actually crosses the
# seam, and nothing has to be maintained.
#
# Temp files rather than process substitution: this script's #! is /bin/sh, and
# on macOS that is bash 3.2 in POSIX mode, where `<(...)' is a syntax error --
# inside $(...) it fails at PARSE time, so the variable comes back empty and
# the case fails with an empty `got' rather than with an error anyone can read.
nm -u "$KERN" 2>/dev/null | grep -v '^$' | grep -v '\.o:$' |
	sed 's/^ *//' | sort -u > "$TMP/undef"
nm -g "$KERN" 2>/dev/null | awk '$2 ~ /^[TDBSC]$/ {print $3}' | sort -u > "$TMP/def"
undef=$(comm -23 "$TMP/undef" "$TMP/def" | tr '\n' ' ' | sed 's/ $//')
check "the archive's only externals are memcpy and V8's setjmp pair" \
	"_longjmp _memcpy _setjmp" "$undef"
# ...and all three are V8's OWN, which "it links" does not say.  memcpy is
# emitted by clang for the struct assignments in allocq(); setjmp and longjmp
# are called by shim/kern/sys/slp.c for streamio.c's u_qsav idiom.
for sym in _memcpy _setjmp _longjmp; do
	nm "$ROOT/build/stage0/libc/libv8c.a" 2>/dev/null | grep -q " T $sym\$" && ok ||
		bad "libv8c defines $sym, so the archive's call is V8's own"
done

# --- and stream.c does NOT reach into libv8sys or libv8c --------------------
# A negative control.  The kernel half is meant to be self-contained above the
# raw syscalls; if it ever starts calling the shim's v8s_* entry points, the
# layering has inverted and the archive stops being linkable on its own.
nm -u "$KERN" 2>/dev/null | grep -q '_v8s_' &&
	bad "the kernel archive calls into libv8sys" || ok

# ---------------------------------------------------------------------------
# THE TTY LINE DISCIPLINE -- src/sys/dev/ttyld.c, imported byte-identical.
#
# It is line discipline 0 (conf/devices:75), not a device, and on a real V8 it
# is pushed onto a terminal's stream by init.c:377.  Nothing is under it here,
# which bounds the traffic paths and NOT the open path -- ttyopen never
# dereferences q->next.  See ttyprobe.c's header.
# ---------------------------------------------------------------------------

# NTTY is the one number in this machinery that V8's shipped tree does not
# contain: config(8) generated it from a machine description that was not
# shipped, and there is no `#define NTTY' anywhere in third_party/.  So it is
# this port's decision, spelled in shim/kern/dev/tty.h and DERIVED rather than
# picked: a discipline needs a stream, so NSTREAM bounds it exactly.
check "NTTY is NSTREAM, so the discipline is never the scarcer resource" \
      "128" "$(t ntty)"
check "...and NSTREAM is still what sparam.h says" \
      "128" "$(awk '$2=="NSTREAM"{print $3}' "$ROOT/src/sys/research/sparam.h")"
check "struct ttyld is 14 bytes"        "14"  "$(t ttyldsize)"

# How big tty[] REALLY is, read out of the compiled archive rather than out of
# a header.  ttyprobe.c includes the same tty.h ttyld.c did, so a sizeof there
# would be one number read twice and would agree even if the object had been
# built against a different NTTY.  `tty' is a common symbol and nm prints its
# SIZE in the value column, so this is the object's own answer.
ttysz=$(nm -g "$KERN" 2>/dev/null | awk '$2=="C" && $3=="_tty" {print $1}')
check "tty[] in the object is NTTY * sizeof(struct ttyld)" \
      "1792" "$((16#${ttysz:-0}))"

# partab is the 51-line data file ttyld.c declares `extern char partab[]'.  A
# missing definition would link as a common symbol of zeros and read back as
# all-zero, so the CONTENT is asserted, at three points of the table, each
# transcribed from src/sys/sys/partab.c.
check "partab[0] is NUL's 0001"          "1"   "$(t partab0)"
check "partab['\\t'] is 0004, a class"   "4"   "$(t partabTab)"
check "partab['@'] is 0200, parity only" "128" "$(t partabAt)"

# The open path, driven through qinfo->qopen rather than by calling ttyopen by
# name -- so the `long (*)()' slot the Makefile suppresses a warning about is
# the thing under test, not a direct call that sidesteps it.
check "qopen returns 1"                      "1" "$(t open1)"
check "it hangs a ttyld off the read queue"  "1" "$(t ptrset)"
check "...and the write queue shares it"     "1" "$(t wrsame)"
check "QDELIM is set"                        "1" "$(t qdelim)"
check "QNOENB is set"                        "1" "$(t qnoenb)"

# The default terminal V8 gives you, ttyld.c:51-57.  These are the values
# init.c would have inherited before stty ever ran.
check "the slot is marked in use"       "1"   "$(t ttuse)"
check "ECHO is on by default"           "1"   "$(t echo)"
check "CRMOD is on by default"          "1"   "$(t crmod)"
check "erase is ^H"                     "8"   "$(t erase)"
check "kill is @"                       "64"  "$(t kill)"
check "intr is DEL, not ^C"             "127" "$(t intrc)"
check "quit is FS"                      "28"  "$(t quitc)"
check "no delimiters yet"               "0"   "$(t delct)"
check "column 0"                        "0"   "$(t col)"

# ttyld.c:47 returns early when qp->ptr is set, so a second push is a no-op
# rather than a second slot.  Getting this wrong would leak a slot per push.
check "pushing twice returns 1 again"   "1" "$(t open2)"
check "...and does not take a new slot" "1" "$(t samescnd)"
check "close releases the slot"         "1" "$(t closed)"

# EXHAUSTION, and the second of these is the load-bearing one.  ttyopen refuses
# past the end of tty[]; CLAUDE.md's rule is that a qopen must never return a
# NEGATIVE int, because `return -1' widens to 0x00000000ffffffff, which
# stopen:124 does not read as NULL and :131 does not read as 1 -- so the open
# would appear to SUCCEED and hand back an inode pointer of 0xffffffff.
# ufalloc() in this same tree does return -1, so the shape is not hypothetical.
check "exactly NTTY disciplines fit"    "128" "$(t nopen)"
check "the refusal is at slot NTTY"     "128" "$(t firstfail)"
check "and it is 0, never negative"     "0"   "$(t negative)"

echo "streams: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
