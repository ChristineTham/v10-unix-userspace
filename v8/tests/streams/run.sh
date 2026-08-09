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
# riding on both.
#
# A DEADLINE, and this line used to say "no deadline -- nothing here sleeps".
# That was true of the open path and stopped being true the moment a driver went
# under the discipline: every stioctl below sends an M_IOCTL down and tsleeps on
# the acknowledgement for FIFTEEN SECONDS, and every stread with an empty queue
# sleeps until the driver sends something up.  So a driver that forgets to ack,
# or a discipline that swallows a block, fails as a HANG rather than as a wrong
# answer -- and an unbounded hang in suite 17 of 17 reads as a broken machine.
if ! clang $KFLAGS -o "$TMP/ttyprobe" "$ROOT/tests/streams/ttyprobe.c" \
     "$KERN" "$SETJMP" > "$TMP/ttybuild.log" 2>&1; then
	grep -qv 'reducing alignment' "$TMP/ttybuild.log" &&
		{ echo "ttyprobe build failed:"; head -5 "$TMP/ttybuild.log"; exit 1; }
fi
perl -e 'alarm 60; exec @ARGV' "$TMP/ttyprobe" > "$TMP/tty" 2>"$TMP/ttyerr" ||
	bad "ttyprobe exited nonzero" "$(head -3 "$TMP/ttyerr")"
t() { awk -v k="$1" '$1==k {$1=""; sub(/^ /,""); print}' "$TMP/tty"; }

# A DUPLICATED KEY IS SILENT, AND IT COST A ROUND WHILE THE TRAFFIC CASES WERE
# BEING WRITTEN.  These readers print EVERY matching line, so two probe lines
# with one key make $(t k) a two-line string -- which never equals the wanted
# value, and whose diagnostic is a `got' field containing both answers with no
# hint that the cause is the key rather than the code.  It happened here for
# real: the open-path section already printed `erase 8' and `kill 64', and the
# canonical-input cases printed `erase' and `kill' again.
#
# So assert the property instead of remembering it.  This is cheap, it covers
# all three probes, and it is the kind of guard that can only ever fire on a
# case someone is in the middle of adding.
for f in out sio tty; do
	dup=$(awk '{print $1}' "$TMP/$f" | sort | uniq -d | tr '\n' ' ')
	[ -z "$dup" ] || bad "$f: probe key printed twice, so \$(t k) is two lines" "$dup"
done

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
# is pushed onto a terminal's stream by init.c:377.  Two stacks are driven here
# and ttyprobe.c's header says why: a bare queue pair for the open path, which
# ttyopen can run on because it never dereferences q->next, and -- since step
# 1c -- a real stream head / ttyld / driver stack for everything below it.
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

# ---------------------------------------------------------------------------
# THE TRAFFIC PATHS -- step 1c, and the half step 1b could not reach.
#
# ttyldin, ttyinsrv, ttyosrv, outconv, ttysig and ttldioc compiled and linked
# from the day ttyld.c was imported, and NOTHING DROVE THEM.  They all reach
# past their own queue, and ttyldin reaches both ways in one function -- data
# up through q->next, flow control down through WR(q)->next -- so exercising
# them needed a bottom end, which is what a driver is and a module is not.
#
# Everything below runs against a real three-layer stack, built the way
# init.c:368-382 builds one: stopen the driver, then FIOPUSHLD the discipline
# between it and the stream head.  Only the driver is this port's; the stream
# head is streamio.c and the discipline is ttyld.c, both authentic.
# ---------------------------------------------------------------------------

check "the driver's stream opens"       "0" "$(t drvopenerr)"
# NULL is SUCCESS here -- stopen returns an inode only for a cloning driver
# (streamio.c:131, `if ((long)nip != 1)').  sioprobe.c asserts the same thing
# for the loopback driver, and it is the shape a reader gets backwards.
check "...and stopen returns NULL to say so" "1" "$(t drvopenret)"
check "...and the inode is streaming"   "1" "$(t drvattached)"
check "the discipline registers"        "1" "$(t ttyldnum)"
check "FIOPUSHLD pushes it"             "0" "$(t pusherr)"
# Pushed BETWEEN, not beside: ttyopen ran (so the stream head's write queue now
# leads to a ttyld with a slot), and there is still something under it.
check "...and ttyopen ran on the pushed queue" "1" "$(t pushedptr)"
check "...with a tty[] slot taken"      "1" "$(t pushedttuse)"
check "...and the driver is still below it" "1" "$(t pushedbelow)"

# --- the read path: ttyldin queues, ttyinsrv canonicalises ----------------
# One read needs BOTH.  ttyldin puts each byte on the queue and, on the
# newline, enqueues an M_DELIM and qenables; ttyinsrv then runs at splx(0) and
# gathers the line.  Values are printed with non-graphic bytes as \NNN, so the
# delivered newline is asserted rather than swallowed by the shell.
check "a typed line arrives"            "hi\\012" "$(t canon)"
check "...with the newline counted"     "3"       "$(t canonn)"
check "...and no error"                 "0"       "$(t canonerr)"
# ECHO is on by default, so the same bytes went back out the write side as they
# arrived -- ttyldin putd's them onto WR(q), ttyosrv drains them, the driver
# sees them.  A terminal shows you what you typed because the KERNEL sends it
# back, and this is that loop closing through 1985 code.
check "...and were echoed, CRMOD'd, to the device" "hi\\015\\012" "$(t echo1)"

# Erase and kill are ttyinsrv's work, not ttyldin's: ttyldin queues \010
# verbatim and only the canonicaliser backs up over it.
check "erase backs over a character"    "hi\\012" "$(t canonerase)"
check "kill discards the line so far"   "hi\\012" "$(t canonkill)"
check "CRMOD makes a typed CR a NL"     "cr\\012" "$(t crmodin)"
check "...and the program reads 3 bytes" "3"      "$(t crmodinn)"

# --- the write path: ttyosrv and outconv ----------------------------------
# THE TAB DOES NOT EXPAND, and this case exists to say so.  outconv's
# expansion loop is guarded by (t_flags&TBDELAY)==XTABS (ttyld.c:385) and
# ttyopen sets ECHO|CRMOD only -- XTABS means "this terminal cannot do tabs
# itself", a property of the hardware.  A first draft of this case expected
# `a       b' from reading the loop and not its guard.
check "outconv passes a tab through"    "a\\011b\\015\\012" "$(t outconv)"
# 0 is the CR/LF dance, not a reset: `a' takes t_col to 1, the tab to 8, `b' to
# 9, the injected \r zeroes it, and the \n leaves it alone because CRMOD is set.
check "...and t_col ends where the CR left it" "0" "$(t outcol)"

# And now the loop, with the flag that unlocks it.  XTABS alone: no ECHO adding
# bytes from the other direction, no CRMOD adding a \r.  `a' leaves t_col at 1
# and the loop writes spaces until (t_col & 07) == 0, which is seven.
check "XTABS expands a tab to the next multiple of 8" \
      "a\\040\\040\\040\\040\\040\\040\\040b" "$(t xtabs)"
check "...leaving t_col at 9"           "9" "$(t xtabscol)"

# --- ttysig: a byte becomes a real signal ---------------------------------
# The end-to-end one, and the only case in this suite whose assertion is that a
# HANDLER RAN.  DEL is t_intrc, ttyldin recognises it, ttysig flushes both
# queues and sends M_SIGNAL up, streamio.c:379 turns that into gsignal, and the
# shim's gsignal is a real kill(2) to this process.  Six layers, one keystroke.
check "pgrp is set, or gsignal goes nowhere" "1" "$(t pgrpset)"
check "DEL from the terminal delivers SIGINT" "1" "$(t intsig)"
check "...and FS delivers SIGQUIT"      "1" "$(t quitsig)"
check "...and the device was told to flush" "1" "$(t intflush)"
check "...and the delimiter count was reset" "0" "$(t intdelct)"
# Not cosmetic: the three characters typed before the DEL are gone.
check "...and the typed-ahead line was discarded" "0" "$(t intdropped)"

# --- ttldioc from the process side (fromdev 0) ----------------------------
# stioctl packages an M_IOCTL, sends it down, and sleeps on the ack.  ttyosrv
# picks it off and calls ttldioc(q, bp, RD(q), 0) -- and for TIOCSETP the zero
# matters: the block goes FURTHER DOWN, so the ack that wakes stioctl is the
# driver's.  That is why the driver has to acknowledge and not free.
check "TIOCSETP completes"              "0"  "$(t setperr)"
check "...and sets RAW"                 "1"  "$(t setpraw)"
check "...and the erase character"      "35" "$(t setperase)"
check "...and the kill character"       "37" "$(t setpkill)"
check "...and the DEVICE saw the ioctl" "1"  "$(t setpioc)"
# ttldioc's last act: RAW clears QDELIM|QNOENB on the reader, because a raw
# stream has no delimiters to promise.
check "...and RAW clears QDELIM on the reader" "0" "$(t setpqdelim)"

# RAW traffic really is raw: ttyldin's RAW branch and ttyinsrv's (CBREAK|RAW)
# branch, neither of which the canonical cases reach.
check "RAW passes 4 bytes through"      "4"  "$(t rawn)"
check "...with the CR unconverted"      "13" "$(t rawbyte3)"
check "...and no echo"                  "0"  "$(t rawecho)"

check "TIOCGETP completes"              "0"  "$(t getperr)"
check "...and reads back the erase"     "35" "$(t getperase)"
check "...and the kill"                 "37" "$(t getpkill)"
check "...and the flags"                "1"  "$(t getpraw)"

# THE BEHAVIOURAL DIFFERENCE WORTH A CASE.  ttldioc's TIOCSETP arm passes the
# block down to the device; its TIOCSETC arm is `qreply(q, bp)' with fromdev 0,
# which turns it round AT THE DISCIPLINE.  So one command reaches the hardware
# and the other does not -- invisible from the syscall's return value, which is
# 0 either way, and visible only by asking the driver.
check "TIOCSETC completes"              "0" "$(t setcerr)"
check "...and sets the interrupt character" "3" "$(t setcintrc)"
check "...WITHOUT the device ever seeing it" "0" "$(t setcioc)"
check "TIOCGETC reads it back"          "3" "$(t getcintrc)"
check "...with no error"                "0" "$(t getcerr)"

# --- CBREAK, the third mode -----------------------------------------------
# ttyldin has three branches and RAW plus canonical is two.  CBREAK is the
# middle: no line gathering, so a character is readable the instant it arrives
# -- but special characters are still interpreted and ECHO still happens, and
# neither is true in RAW.  Those three together are what separate it.
check "CBREAK is settable"              "0"  "$(t cbreakerr)"
check "...and reads without a newline typed" "2" "$(t cbreakn)"
check "...returning what was typed"     "ab" "$(t cbreak)"
check "...still echoing, unlike RAW"    "2"  "$(t cbreakecho)"
check "...and still signalling, unlike RAW" "1" "$(t cbreaksig)"

check "TIOCSETP restores canonical mode" "1" "$(t recanon)"
check "...and QDELIM comes back"        "1" "$(t recanonqdelim)"

# --- flow control: the direction only a driver can see --------------------
# t_stopc sets TTSTOP and sends M_STOP DOWN to the device -- WR(q)->next, not
# q->next -- and t_startc clears it and sends M_START.  A second module stacked
# above would never see either, which is the argument for a driver in one line.
check "^S stops the line"               "1" "$(t stopstate)"
check "...and the device is told"       "1" "$(t stopsent)"
check "^Q starts it again"              "1" "$(t startstate)"
check "...and the device is told that too" "1" "$(t startsent)"
# And the characters are consumed, not delivered: ttyldin `continue's past them.
check "...and neither reaches the program" "xyz\\012" "$(t flowline)"

# --- LCASE and maptab[]: a Model 33 with no lower case and no braces ------
# The most 1970s thing in the file, and the only reader of maptab[]'s 128
# bytes.  Two functions share the work: ttyldin marks an escaped character by
# setting bit 7 and never consults the map; ttyinsrv sees the marked byte and
# does the lookup.  So `A\a\(' is a plain A folded DOWN to a, an escaped a
# mapped UP to A, and an escaped ( mapped to a brace the keyboard cannot type.
check "LCASE folds and maptab unfolds"  "0"        "$(t lcaseerr)"
check "...so A\\a\\( reads back aA{"      "aA{\\012" "$(t lcase)"

# With LCASE off, the same else-arm has two outcomes and only one is obvious:
# an ordinary character KEEPS its backslash, but one that IS the erase, kill or
# eof character is emitted alone -- dropping the backslash is how you type a
# literal one.  `@' is the default kill, so `\@' is a bare @ and `\z' is not.
check "an escaped ordinary character keeps its backslash, an escaped kill does not" \
      "\\134z@\\012" "$(t escape)"
# MEASURED, NOT PREDICTED.  ttyldin:171-175 strips bit 7 off an escaped
# backslash and SETS TTESC AGAIN, so the literal \ is queued unmarked and the
# NEXT character is treated as escaped though no second backslash was typed.
# That is upstream's behaviour; the case records it so a future reader meets
# the behaviour rather than the intention.
check "a doubled backslash leaves the escape latched" \
      "\\134\\134z\\012" "$(t dblesc)"

# --- TANDEM: the flow control that runs the other way ---------------------
# ^S from the terminal stops OUTPUT.  TANDEM is the discipline noticing its own
# INPUT queue filling and sending a stop character BACK so the sender pauses.
# The threshold is upstream's own arithmetic on ttrinit's numbers --
# (limit + lolimit) / 2 with ttrinit {..., 600, 60} (ttyld.c:34), so 330 --
# which is why this needs hundreds of bytes with no newline rather than a line.
check "400 bytes with no newline reach the queue" "400" "$(t tandemcount)"
check "...so TANDEM blocks"              "1"  "$(t tandemblocked)"
check "...and one character goes to the device" "1" "$(t tandemstopped)"
check "...and it is t_stopc, ^S"         "19" "$(t tandemstopc)"
# The release is ttyinsrv's tail, not ttyldin's -- it only happens because
# something READ.  256 is this probe's buffer, not a property of the stream.
check "a read drains it"                 "256" "$(t tandemread)"
check "...and TANDEM releases"           "1"   "$(t tandemunblocked)"
check "...with t_startc, ^Q"             "17"  "$(t tandemstartc)"

# --- outconv's delays, for four terminals that existed ---------------------
# V8 still carried padding for the tty 37, vt05, tn 300 and ti 700 in 1985: a
# carriage return on a tn 300 took longer than the next character took to
# arrive, so the discipline emits an M_DELAY the driver turns into silence.
# The algorithm lives in bits 12-13 of the flag word, which is why that word is
# worth more than a boolean.
check "CR1 selects the tn 300 algorithm" "0" "$(t crdelayerr)"
check "...and a CR emits one M_DELAY"    "1" "$(t crdelayn)"
check "...whose count is 5"              "5" "$(t crdelayval)"
check "...and the carriage is back at column 0" "0" "$(t crdelaycol)"
# The negative control: the same CR with the delay bits clear emits nothing, so
# the case above measures the ALGORITHM and not the presence of a return.
check "...but with the bits clear, no delay at all" "0" "$(t nodelayn)"

# --- the ack a driver must SHORTEN, and the one it must refuse ------------
# TIOCHPCL is in neither switch, so ttldioc's default arm passes it down and
# the DRIVER is what answers.  cons.c:64-67 answers with an M_IOCNAK carrying
# no payload byte, and streamio.c:803-809 turns exactly that into ENOTTY -- so
# a driver that acked everything would report success for an unimplemented
# command.
check "an unimplemented ioctl reaches the driver" "1" "$(t naked)"
check "...and comes back ENOTTY"        "25" "$(t nakerr)"

# AND THE LENGTH OF AN ACK IS PART OF THE ACK.  stioctl builds every M_IOCTL 20
# bytes long; ttldioc's TIOCSETP arm does not touch wptr; streamio.c:793-798
# copies `wptr - rptr' back to the caller.  So an unshortened ack writes 16
# bytes into a 6-byte struct sgttyb -- ten past the object, on every set.
# Bell Labs' drivers each spend one line on it (cons.c:56-58, dz.c:229), and
# the first draft of this port's driver did not, which the lp64-auditor found.
#
# A VALUE SENTINEL CANNOT SEE THIS: the ten bytes written are the ten copyin
# read from that address moments earlier, so the write round-trips and memory
# ends up correct.  The only observable is the fault, so the probe arranges one
# -- sg at the last six bytes of a writable page with the next page READABLE
# but not writable, so the authentic 20-byte over-READ still succeeds and only
# the write can fail.  Measured: 0 with the line, SIGBUS (10) without it.
check "a shortened ack writes nothing past the caller's sgttyb" "0" "$(t guardsig)"
check "...and the ioctl itself still succeeds" "0" "$(t guardexit)"

# --- ttldioc from the DEVICE side (fromdev 1) -----------------------------
# The other arm, and the only one a driver can reach: an M_IOCTL sent UP
# arrives at ttyldin, which calls ttldioc(WR(q), bp, q, 1).  With fromdev set
# every arm ends in qreply(rdq, bp) -- back DOWN as an M_IOCACK -- so a modem
# that asks the discipline for the line settings is answered without the
# process being involved.  Nothing in this port had taken that arm before.
check "an ioctl from the device is acked back to it" "1" "$(t devioc)"

# M_BREAK in canonical mode is an interrupt: a line break and a DEL key reach
# ttysig by different routes, which is a claim about the switch at the top of
# ttyldin rather than about signals.
check "a line break raises SIGINT"      "1" "$(t breaksig)"
# The last arm of that switch goes THROUGH rather than being consumed.
check "M_HANGUP is passed up to the stream head" "1" "$(t hungup)"
# And the slot comes back, which is what lets the exhaustion case above still
# measure NTTY rather than NTTY-1.
check "stclose releases the tty[] slot" "1" "$(t stclosed)"

echo "streams: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
