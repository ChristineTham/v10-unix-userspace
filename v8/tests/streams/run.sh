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
#include "../../shim/kern/h/hostok.h"
#include <stdio.h>
#include <signal.h>
#include "../../src/sys/h/stream.h"
#include "../../shim/kern/h/proc.h"
#include "../../src/sys/h/dir.h"
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
# something READ.  401 is what was SENT (400 characters and the newline), which
# is a property of the case; it used to read 256 and that was the probe's
# buffer, a number that silently left 145 bytes queued for the next case.
check "a read drains the whole line"     "401" "$(t tandemread)"
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

# --- the other three delay algorithms, and max()'s ONLY caller ------------
# max() was written for this import -- the one name ttyld.c needed that the
# shim did not have -- and writing it is what found min() misdeclared in two
# files.  It has EXACTLY ONE call site in the whole tree, ttyld.c:439, inside
# the tty 37 newline delay, and until these two cases nothing had executed it.
# Two lines because max() has two branches and the column decides which:
# count is max(t_col>>4 + 3, 6), so `abc' takes the constant and 64 characters
# take the computed value.
check "the tty 37 newline delay takes max()'s constant arm" "6" "$(t nl1short)"
check "...and a long line takes its computed arm"           "7" "$(t nl1long)"

# The tab delay -- the arm outconv reaches when TBDELAY names an algorithm
# rather than XTABS.  1 - (t_col | ~07) at column 0 is 1 - (-8) = 9.
check "the tty 37 tab delay is 9 at column 0" "9" "$(t tab1delay)"
check "...and the tab still moves the column to 8" "8" "$(t tab1col)"

# AND THE VERTICAL DELAY IS NOT REACHED BY A VERTICAL TAB.  partab.c:12 gives
# 013 (VT) class 1, `non-printing'; it is 014, FORM FEED, that is class 5.  So
# the flag spelled VTDELAY is a form-feed delay -- 127 ticks, the longest in
# the file, because ejecting a page is the slowest thing a printer does.
check "a vertical tab produces no delay at all" "0"   "$(t vtnone)"
check "...but a form feed produces 127"         "127" "$(t ffdelay)"

# --- ttyhog, and the buffer the reader cannot see -------------------------
# ttyhog (ttyld.c:176) is the older limit and the ruder: once the read queue
# holds 512, a character that is not a newline is REPLACED by \007 and never
# queued, so the terminal beeps instead of accepting more.  600 sent.
check "the read queue caps at 512"      "512" "$(t hogcount)"
check "...and the terminal is made to beep" "1" "$(t hogbells)"
check "...and fewer than 600 characters got in" "1" "$(t hogcapped)"

# canonb is 256 bytes and ttyinsrv flushes at 255, so the ~498 characters that
# got through cross it at least twice.  WHAT IS ASSERTED IS THAT THE READER
# CANNOT TELL: one read returns the whole line, because stread loops on the
# DELIMITER rather than on a message boundary.
#
# Two drafts of this case were wrong before it was measured.  The first
# expected ">255 bytes in one read"; the second saw 145 and concluded the line
# arrives in PIECES -- and 145 was not a piece, it was the TANDEM case's unread
# remainder leaking in through a 256-byte buffer.  ttyprobe.c grew a readline()
# to close that, which is why every canonical read now takes whole lines.
check "a line longer than canonb comes back whole" "498" "$(t canonbtotal)"
check "...in a single read, so the buffer is invisible" "1" "$(t canonbonepiece)"
check "...ending in the newline"        "10"  "$(t canonbend)"

# --- the two arms only the device can originate ---------------------------
# M_DELIM sent UP is dropped on the floor (ttyldin:86-88, `freeb; return'),
# because a device has no business telling a canonical discipline where a line
# ends -- that is the discipline's own judgement.  So the line typed after it
# arrives alone, with no empty record in front.
check "an M_DELIM from the device is discarded" "ok\\012" "$(t delimdropped)"
# And an unsolicited M_IOCACK goes straight through to the stream head, which
# takes streamio.c's `(stp->flag&IOCWAIT)==0' branch and frees it.  The case
# that matters is the one after: the next real ioctl must be unaffected.
check "a stray ack does not corrupt the next real ioctl" "0" "$(t afterstrayack)"

# ttyosrv's M_FLUSH arm, AND THE NOTE THAT STOOD HERE WAS WRONG ABOUT HOW TO
# REACH IT.  It said the arm "needs TIOCFLUSH, which stioctl handles itself",
# as though handling it were the obstacle.  It is the mechanism:
# streamio.c:594 is putctl(stq->wrq->next, M_FLUSH), and stq->wrq->next IS
# ttyld's write queue -- so the block lands on it, ttyosrv runs, and passes it
# down.  Accurate citation, opposite conclusion; the same shape as the recorded
# constraint that blocked the inode fix for months.
check "TIOCFLUSH reaches ttyosrv"       "0" "$(t flusherr)"
check "...which passes the flush to the device" "1" "$(t flushtodev)"

# --- the ack a driver must SHORTEN, and the one it must refuse ------------
# TIOCHPCL is in neither switch, so ttldioc's default arm passes it down and
# the DRIVER is what answers.  cons.c:64-67 answers with an M_IOCNAK carrying
# no payload byte, and streamio.c:803-809 turns exactly that into ENOTTY -- so
# a driver that acked everything would report success for an unimplemented
# command.
check "an unimplemented ioctl reaches the driver" "1" "$(t naked)"
# 25 is V8's number, not the host's, and that is already proved one layer down:
# shim/kern/sys/subr.c:74-77 _Static_asserts eleven codes against V8's own
# rootfs/usr/include/errno.h, ENOTTY among them.  So this is a value the BUILD
# guarantees rather than a property of whatever machine the suite runs on --
# which is the distinction tests/kmemu's nice and pid cases exist to make.
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

# --- §8a step 5: the six imported files, and the seam they brought with them
# alloc.c, iget.c, nami.c, rdwri.c, subr.c and bio.c are imported AND NOW BUILT
# -- they are $(V8FS_OBJ) in the Makefile and members of libv8kern.a.
#
# FOUR ARE PRISTINE AND TWO CARRY A DEVIATION, and the second one is why this
# comment changed.  It said "five are pristine"; alloc.c stopped being pristine
# when the build produced two -Wincompatible-pointer-types warnings on it, and
# both were the SAME NOLONG cause as nami.c's -- a `long *' walking an array
# that §8a step 4a narrowed to v8_i32 because it is on disk.
#
# So the two deviations in this import are one bug class found twice, and the
# difference between them is only how loudly it announced itself: nami.c's
# stopped every path lookup dead, alloc.c's was a warning in a build that
# succeeded.  Each gets the DIFF guard rather than the hash, for the reason
# recorded at streamio.c's -- a file with a deviation cannot be hashed, and
# "it has a PORTING.md" is not a guard.
for f in sys/iget.c sys/rdwri.c sys/subr.c dev/bio.c; do
	d=$(dirname "$f")
	prov=$(awk -v p="v8/usr/sys/$f" '$2 == p {print $1}' "$ROOT/src/sys/$d/PROVENANCE")
	here=$(git -C "$ROOT" hash-object "src/sys/$f")
	check "src/sys/$f still hashes to pristine V8" "$prov" "$here"
done

# nami.c's deviation is the NOLONG name compare, and the guard asserts its
# SHAPE rather than its size: exactly three lines left, and each of them is a
# `*(long *)' that became a `*(int *)'.  Counting removals and additions
# separately is the lesson streamio.c's guard already paid for -- a first
# draft that assumes "N changed lines" fails on a deviation that adds a
# comment.
UPNAMI=$ROOT/../third_party/Research-Unix-v8/v8/usr/sys/sys/nami.c
if [ -f "$UPNAMI" ]; then
	gone=$(diff "$UPNAMI" "$ROOT/src/sys/sys/nami.c" | grep -c '^<')
	check "nami.c: upstream lost exactly three lines" "3" "$gone"
	longs=$(diff "$UPNAMI" "$ROOT/src/sys/sys/nami.c" |
		grep '^<' | grep -c '\*(long \*)')
	check "...and all three are the long-cast name compare" "3" "$longs"
	ints=$(diff "$UPNAMI" "$ROOT/src/sys/sys/nami.c" |
		grep '^>' | grep -c '\*(int \*)')
	check "...replaced by exactly three int casts" "3" "$ints"
	# The wrong fix would have been -DDIRSIZ=254 to reach the strncmp arm.
	# That arm must stay unreached, so the compare stays a compare.
	check "...and the strncmp arm is still the #else" "1" \
		"$(grep -c 'strncmp(nm, dp->d_name, DIRSIZ)' "$ROOT/src/sys/sys/nami.c")"
else bad "upstream nami.c not found for the diff guard"; fi

# alloc.c's deviation is the SAME CLASS as nami.c's and a DIFFERENT SHAPE, so
# the guard is shaped to it rather than copied.  nami.c lost three lines and
# gained three casts; alloc.c loses exactly ONE declaration -- `register long
# *p' -- and gains one `register v8_i32 *p' plus a comment block.  Counting
# removals and additions separately is what makes that difference expressible,
# and it is the lesson streamio.c's guard already paid for.
UPALLOC=$ROOT/../third_party/Research-Unix-v8/v8/usr/sys/sys/alloc.c
if [ -f "$UPALLOC" ]; then
	gone=$(diff "$UPALLOC" "$ROOT/src/sys/sys/alloc.c" | grep -c '^<')
	check "alloc.c: upstream lost exactly one line" "1" "$gone"
	check "...and it is the long-pointer declaration" "1" \
		"$(diff "$UPALLOC" "$ROOT/src/sys/sys/alloc.c" |
		   grep '^<' | grep -c 'register long \*p;')"
	check "...replaced by exactly one v8_i32 pointer" "1" \
		"$(diff "$UPALLOC" "$ROOT/src/sys/sys/alloc.c" |
		   grep '^>' | grep -c 'register v8_i32 \*p;')"
	# THE DEVIATION IS ONLY CORRECT WHILE s_bfree IS FOUR BYTES WIDE, and
	# that is declared in a different file by a different layer.  If
	# src/include/sys/filsys.h ever widened S_bfree back to `long', this
	# deviation would become the bug instead of the fix -- so the guard
	# asserts the thing it DEPENDS on, not just its own text.
	check "...and s_bfree is still the narrowed on-disk type" "1" \
		"$(grep -c 'v8_i32.*S_bfree\[BITMAP\]' "$ROOT/src/include/sys/filsys.h")"
	# Upstream states the assumption in a comment five lines below the
	# declaration, and that comment is what made the class recognisable.
	# THIS GUARD MATCHED ITS OWN DOCUMENTATION ON ITS FIRST RUN -- the PORT
	# comment added directly above quotes the phrase "BITS PER LONG", so a
	# bare grep -c returned 2 where upstream has 1.  Third instance of the
	# prose-matching instrument fault in this session alone, and it fired
	# inside a guard written by someone who had just written the other two
	# up.  Anchor on the CODE -- upstream's line is a for-loop with the
	# comment trailing it -- not on the phrase.
	check "...and upstream's BITS PER LONG line survives" "1" \
		"$(grep -c 'j < 32; j++).*BITS PER LONG' "$ROOT/src/sys/sys/alloc.c")"

	# A LINE-NUMBER CITATION INSIDE THE FILE IT CITES IS SELF-INVALIDATING,
	# and this port has now paid for that twice in one afternoon.
	#
	# The PORT comment above the declaration names the four uses of `p' by
	# line.  Writing that comment pushed every one of them down by 43, so
	# the first draft's `:70' pointed into the middle of the comment doing
	# the citing.  Correcting it moved them again -- twice, because the
	# correction itself added lines -- and only the third measurement
	# converged.  A subagent audit of the whole tree found sixteen stale
	# citations of exactly this shape, eight of them caused by this one
	# comment.
	#
	# So the citations are a TEST rather than prose, which is the same move
	# the deviation list above already makes.  Each cited line must contain
	# the code the comment says is there.  It costs four greps and it goes
	# red the moment anyone adds a line to this file without re-measuring.
	A=$ROOT/src/sys/sys/alloc.c
	cite() {	# label, cited line, pattern the line must contain
		check "alloc.c PORT comment: $1" "1" \
			"$(sed -n "$2p" "$A" | grep -c -- "$3")"
	}
	# The four numbers the comment gives as `ours', read back out of it so
	# the test cannot drift from the comment either -- it reads the comment
	# and then checks the comment.
	cn=$(sed -n 's/^[[:blank:]]*\*[[:blank:]]*:\([0-9]*\), :\([0-9]*\)[[:blank:]].*/\1 \2/p' "$A" | head -1)
	set -- $cn
	cite "the first read-modify-write is where it says"  "$1" '\*p &= ~(1 << (j&31))'
	cite "and so is the second"                          "$2" '\*p &= ~(1 << j)'
	cite "the scan loop is where it says"  \
		"$(sed -n 's/^[[:blank:]]*\*[[:blank:]]*:\([0-9]*\) (:83).*/\1/p' "$A")" \
		'for(i = 0; i < BITMAP'
	cite "the bit test is where it says"   \
		"$(sed -n 's/^[[:blank:]]*\*[[:blank:]]*:\([0-9]*\) (:89).*/\1/p' "$A")" \
		'if(\*p & (1 << j))'
	cite "and BITS PER LONG is where it says" \
		"$(sed -n 's/^[[:blank:]]*\* :\([0-9]*\) (upstream :88).*/\1/p' "$A")" \
		'BITS PER LONG'
else bad "upstream alloc.c not found for the diff guard"; fi

# --- param.h's redirects and hostok.h's undefs are DERIVED FROM EACH OTHER ---
# shim/kern/h/hostok.h exists because the redirect list reached thirteen and a
# thirteen-line list copied into every consumer decays independently in each
# copy.  Its own header comment claims this suite keeps the two in step -- and
# when that sentence was first written the check did not exist, which is the
# failure mode this whole file is built against: A CLAIM ABOUT A GUARD IS NOT
# A GUARD.  So it is derived rather than counted.
#
# Every plain `#define NAME v8k_NAME' in param.h must have an `#undef NAME' in
# hostok.h.  uballoc is deliberately excluded and the pattern excludes it for
# free: it is a function-like macro, not a host name, so it has no v8k_ target
# and nothing would #undef it.
PH=$ROOT/shim/kern/h/param.h
HO=$ROOT/shim/kern/h/hostok.h
if [ -f "$PH" ] && [ -f "$HO" ]; then
	grep -hE '^#define[[:blank:]]+[a-z_]+[[:blank:]]+v8k_' "$PH" |
		awk '{print $2}' | sort -u > "$TMP/redir"
	grep -hE '^#undef[[:blank:]]+[a-z_]+' "$HO" |
		awk '{print $2}' | sort -u > "$TMP/undone"
	# the sweep must not be vacuous -- an empty redirect list would make
	# both comm results empty and this block pass while measuring nothing
	n=$(wc -l < "$TMP/redir" | tr -d ' ')
	[ "$n" -ge 10 ] && ok ||
		bad "only $n redirects found in param.h -- the pattern has drifted"
	check "every param.h redirect is undone by hostok.h" "" \
		"$(comm -23 "$TMP/redir" "$TMP/undone" | tr '\n' ' ' | sed 's/ $//')"
	check "and hostok.h undoes nothing param.h does not redirect" "" \
		"$(comm -13 "$TMP/redir" "$TMP/undone" | tr '\n' ' ' | sed 's/ $//')"
else bad "param.h or hostok.h missing -- cannot check the redirect list"; fi

# --- THE TWO ERRNO TABLES ARE THE SAME SEAM, AND NOTHING COMPARED THEM ------
# v8fsd's errnames[] turns a host errno into a symbolic name for the wire and
# p9cl.c's enames[] turns it back; each file's comment cites the other and calls
# the mapping "exactly reversible by the client".  That is a claim about two
# lists in two directories, and it is the shape the block above exists for.
#
# THE DIRECTION IS NOT SYMMETRIC, which is why this is two cases and not one
# `diff'.  A name the SERVER can send that the CLIENT does not know falls into
# enumber()'s EIO fallback -- documented for a foreign server's own prose, so it
# is silent, and an errno the port controls would be quietly flattened.  That is
# the direction that must be empty.  The reverse is harmless and is asserted
# separately: a client entry for a name this server never sends is either dead
# or deliberate, and there is nothing dead here today, so a new one should have
# to be noticed.
#
# The client's V8_ values are NOT compared, deliberately.  Two of them collapse
# on purpose -- ENAMETOOLONG is V8_ENOENT and ENOTEMPTY is V8_EEXIST, because
# V7 has neither errno -- so a value comparison would fail on the very entries
# whose collapse is the considered answer.  What has to agree is the NAME SET.
FSD=$ROOT/shim/v8fsd/v8fsd.c
CLI=$ROOT/shim/v8sys/p9cl.c
if [ -f "$FSD" ] && [ -f "$CLI" ]; then
	# The tables are the only place in either file where a bare "E..." name
	# appears in braces beside a value, which is what makes a grep enough.
	sed -n '/^static const struct { int e; const char \*name; } errnames\[\]/,/^};/p' \
		"$FSD" | grep -oE '"E[A-Z]+"' | tr -d '"' | sort -u > "$TMP/esrv"
	sed -n '/^static const struct { const char \*n; int e; } enames\[\]/,/^};/p' \
		"$CLI" | grep -oE '"E[A-Z]+"' | tr -d '"' | sort -u > "$TMP/ecli"
	# Neither extraction may be vacuous: a pattern that has drifted matches
	# nothing, both comm results come out empty, and the block passes while
	# measuring nothing at all.
	ns=$(wc -l < "$TMP/esrv" | tr -d ' ')
	nc=$(wc -l < "$TMP/ecli" | tr -d ' ')
	[ "$ns" -ge 15 ] && ok || bad "only $ns names found in v8fsd's errnames[]"
	[ "$nc" -ge 15 ] && ok || bad "only $nc names found in p9cl's enames[]"
	check "every errno the server can send, the client knows" "" \
		"$(comm -23 "$TMP/esrv" "$TMP/ecli" | tr '\n' ' ' | sed 's/ $//')"
	check "and the client knows no name the server cannot send" "" \
		"$(comm -13 "$TMP/esrv" "$TMP/ecli" | tr '\n' ' ' | sed 's/ $//')"
else bad "v8fsd.c or p9cl.c missing -- cannot compare the errno tables"; fi

# --- §8a step 5: the six are BUILT, and what the archive imports ------------
# A hash guard says a file is upstream's.  It says nothing about whether the
# build ever reads it, and for a whole release the answer here was "no".
KERNA=$ROOT/build/stage0/kern/libv8kern.a
if [ -f "$KERNA" ]; then
	for o in alloc.o iget.o nami.o rdwri.o bio.o v8fs.o; do
		check "libv8kern.a contains $o" "1" \
			"$(ar t "$KERNA" 2>/dev/null | grep -c "^$o\$")"
	done
	# TWO subr.o, and that is the assertion rather than an accident.
	# src/sys/sys/subr.c and shim/kern/sys/subr.c share a basename; the
	# Makefile puts their objects in different directories for exactly this
	# reason.  If that ever collapsed, ONE WOULD SILENTLY REPLACE THE OTHER
	# -- ar would take the second and say nothing -- and this count drops
	# to one.  Same shape as $(IMGBIN) and $(V8BIN) having to be disjoint.
	check "both subr.o objects are in the archive" "2" \
		"$(ar t "$KERNA" 2>/dev/null | grep -c '^subr\.o$')"

	# THE EXTERNAL IMPORTS, BY SUBTRACTION -- what the archive undefines
	# minus what it defines.  Not an allow list: CLAUDE.md records why, and
	# tests/kmemu's ALLOWED going stale is the precedent.
	#
	# THIS IS THE ONLY INSTRUMENT THAT COULD SEE STEP 5'S WORST BUG.
	# KERNFLAGS carries -Wno-implicit-function-declaration, argued for the
	# K&R dialect -- so a MISSING MACRO (BSIZE(dev), itod(), NINDIR()) was
	# compiled as a call to an undefined FUNCTION, in silence.  Fourteen of
	# them.  The build was clean and only the symbol table knew.
	nm -u "$KERNA" 2>/dev/null | grep -v '^$' | grep -v ':' | sort -u > "$TMP/k.u"
	nm -g "$KERNA" 2>/dev/null |
	    awk '$2=="T"||$2=="D"||$2=="S"||$2=="C"{print $3}' | sort -u > "$TMP/k.d"
	check "libv8kern imports only V8's own three" "_longjmp _memcpy _setjmp" \
		"$(comm -23 "$TMP/k.u" "$TMP/k.d" | tr '\n' ' ' | sed 's/ $//')"
else bad "libv8kern.a not built -- cannot check the step 5 objects"; fi

# --- the width names are declared TWICE, so the two are compared ------------
# shim/kern/h/param.h and src/include/sys/types.h both declare v8_i16, v8_u16,
# v8_i32 and v8_u32.  The kernel side cannot include the userland types.h --
# it re-typedefs daddr_t, ino_t, dev_t and off_t, three at widths the kernel
# side deliberately narrows -- so the duplication is forced, and a forced
# duplication is a thing to measure rather than to promise in a comment.
#
# Two programs, one per header, printing sizeof and signedness.  Comparing the
# OUTPUT rather than the text is what makes this a check on the types and not
# on how they happen to be spelled.
WP=$TMP/width
cat > "$WP-k.c" <<'KEOF'
#include "../h/param.h"
/*
 * param.h:152-157 says this out loud and names the file it applies to:
 * `#define printf v8k_printf' rewrites stdio's own declaration into a
 * conflicting prototype, so the #undefs have to come BEFORE <stdio.h>.
 * probe.c:23-24 does the same two.  Written from the comment after the
 * probe hit the error the comment predicts.
 */
#undef printf
#undef bcopy
#include <stdio.h>
int main(){ printf("%zu %zu %zu %zu %d %d\n",
	sizeof(v8_i16), sizeof(v8_u16), sizeof(v8_i32), sizeof(v8_u32),
	(int)((v8_i16)-1 < 0), (int)((v8_i32)-1 < 0)); return 0; }
KEOF
> "$WP-u.c" cat <<'UEOF'
#include <sys/types.h>
main(){ printf("%d %d %d %d %d %d\n",
	(int)sizeof(v8_i16), (int)sizeof(v8_u16),
	(int)sizeof(v8_i32), (int)sizeof(v8_u32),
	(int)((v8_i16)-1 < 0), (int)((v8_i32)-1 < 0)); }
UEOF
# THE USERLAND HALF IS COMPILED BY v8cc, NOT BY THE HOST'S clang, and that is
# forced rather than stylistic: src/include/sys/types.h includes
# src/include/sys/param.h, which includes <signal.h>, and under the host SDK
# that redefines size_t against V8's own typedef.  Those headers are compiled
# by V8's compiler in this port -- so compiling them any other way would be
# measuring a configuration nothing uses.  Measured, not assumed: the first
# draft used host clang and died on exactly that redefinition.
#
# A missing compiler is a FAILURE and not a skip.  tests/cpp is the precedent
# for why: it wrapped its most valuable case in `if [ -d ... ]' and reported
# 12 passed from outside the repo root.
V8CC=$ROOT/rootfs/bin/cc
if [ ! -x "$V8CC" ]; then
	bad "width seam: $V8CC not built (run make first)"
elif clang -w -std=gnu89 -I"$ROOT/shim/kern/dev" -o "$WP-k" "$WP-k.c" 2>/dev/null &&
     V8ROOT=$ROOT/rootfs "$V8CC" -o "$WP-u" "$WP-u.c" 2>/dev/null; then
	kw=$("$WP-k"); uw=$("$WP-u")
	check "the width names agree across the kernel/userland seam" "$uw" "$kw"
	# And they are the widths the on-disk records were narrowed TO, which is
	# the fact the forwarding headers depend on.  Named, not inferred from
	# agreement -- two files can agree and both be wrong.
	check "...and they are 2 2 4 4, signed where they say signed" \
		"2 2 4 4 1 1" "$kw"
else bad "width-typedef seam probe (compile)"; fi

# --- §8a step 5c: THE FILESYSTEM RUNS -----------------------------------------
#
# Everything above this line is a statement about a BUILD -- hashes, diff
# shapes, what the archive imports, which header the seam resolves to.  Not one
# of them would have changed if alloc.c, iget.c, nami.c, rdwri.c, subr.c and
# bio.c had never executed an instruction, and until now they had not.
#
# The cases below drive Bell Labs' own path -- namei -> fsnami -> dsearch ->
# iget -> bmap -> readi -> bread -> a block driver -- over an image mkfs(8)
# wrote, and compare the bytes that come out against the bytes that went in.
# tests/streams/fsprobe.c is the harness and says why the driver lives there.
#
# THE WRITER AND THE READER ARE INDEPENDENT AND THAT IS THE WHOLE POINT.
# tests/mkfs already asks whether the image matches what this port believes a
# V8 filesystem is -- but mkfs and that suite's arithmetic were written from the
# same headers, so a shared misunderstanding satisfies both.  V8's kernel was
# written in 1985 against the real thing.
FSTMP=$TMP/fsdir
mkdir -p "$FSTMP"

MKFS=$ROOT/rootfs/etc/mkfs
if [ ! -x "$MKFS" ]; then
	# A missing input is a FAILURE, not a skip -- tests/cpp is the precedent
	# for why: it wrapped its best case in `if [ -d ... ]' and reported 12
	# passed from outside the repo root.
	bad "filesystem: $MKFS not built (run make first)"
else

# Two files, and the SIZES ARE THE ARGUMENT.  hello.txt is one block, so it
# exercises only bmap's direct arm; big.txt is 28000 bytes = 28 blocks, and
# blocks 0..9 are the NADDR-3 direct addresses in the inode while 10..27 are
# reached through the single indirect block.  A one-block test cannot tell a
# correct bmap from one whose indirect arm is broken, and the indirect walk is
# `daddr_t *bap' over a bread'd buffer -- the same shape as the free-map walk
# where this import's second NOLONG deviation was found.
printf 'hello from a V8 filesystem\n' > "$FSTMP/hello.txt"
awk 'BEGIN{for(i=0;i<1000;i++) printf "line %04d abcdefghijklmnopq\n", i}' \
	> "$FSTMP/big.txt"

printf '/dev/null\n2000 1280\nd--777 0 0\nhello\n---644 0 0 %s\nsub\nd--755 0 0\ndeep\n---644 0 0 %s\n$\n$\n' \
	"$FSTMP/hello.txt" "$FSTMP/big.txt" > "$FSTMP/proto"

if ! (cd "$FSTMP" && V8ROOT=$ROOT/rootfs "$MKFS" "$FSTMP/img" "$FSTMP/proto") \
     >"$FSTMP/mkfs.log" 2>&1; then
	bad "filesystem: mkfs failed" "$(head -3 "$FSTMP/mkfs.log")"
else

# A PRISTINE COPY, TAKEN HERE AND NOT LATER, for the §8a step 5e section at the
# bottom of this file.  fsprobe WRITES to $FSTMP/img -- step 5d creates a file,
# grows it past the superblock's cached free list and deletes it -- so a server
# started on that image at the end of the run is reading a filesystem an earlier
# section modified.  Measured, and it is not subtle: the 9P section first
# reported hello's length as 10248 against the 27 bytes mkfs put there.
#
# That is tests/crash-probe.sh's lesson arriving between two sections of one
# suite rather than between two programs in one directory: a case has to be a
# pure function of what it was given.
cp "$FSTMP/img" "$FSTMP/p9img"

# -DKERNEL IS NOT OPTIONAL FOR THIS PROBE and fsprobe.c says why at length:
# inode.h and buf.h declare namei, iget, bread and geteblk -- all
# pointer-returning -- inside #ifdef KERNEL, and KFLAGS carries
# -Wno-implicit-function-declaration for the imported half's sake.  Without the
# flag every one of those calls is an implicit int and the returned pointer is
# TRUNCATED to 32 bits, which is this port's ps -T bug exactly.  fsprobe.c has
# an #error so the flag cannot be dropped silently.
# THE DRIVER OBJECT IS ON THE LINK LINE, not in the archive, and both probe and
# server get the same one.  A driver set is part of a CONFIGURATION -- config(8)
# is what chooses one on a real V8, and v8k_bdconf stands in for config(8) --
# so putting imgdev.o in libv8kern.a would make the kernel library name _pread
# and _pwrite, which is what the "imports only V8's own three" case above exists
# to refuse.  Measured: it did, the first time it was tried.
IMGDEV=$BUILD/kern/imgdev.o
[ -e "$IMGDEV" ] || { echo "missing $IMGDEV -- run make"; exit 1; }
if ! clang $KFLAGS -DKERNEL -fcommon -I"$ROOT/shim/kern/dev" -o "$TMP/fsprobe" \
     "$ROOT/tests/streams/fsprobe.c" "$IMGDEV" "$KERN" "$SETJMP" \
     > "$TMP/fsbuild.log" 2>&1; then
	grep -qv 'reducing alignment' "$TMP/fsbuild.log" &&
		{ echo "fsprobe build failed:"; head -5 "$TMP/fsbuild.log"; exit 1; }
fi
# A DEADLINE, for ttyprobe's reason plus one of its own: getblk sleeps on
# bfreelist[0] when no buffer is free and only brelse wakes it, so a leaked
# buffer is an unbounded hang rather than an error.
perl -e 'alarm 60; exec @ARGV' "$TMP/fsprobe" "$FSTMP/img" "$FSTMP/readback" \
	> "$TMP/fsout" 2>"$TMP/fserr" ||
	bad "fsprobe exited nonzero" "$(head -3 "$TMP/fserr")"
f() { awk -v k="$1" '$1==k {$1=""; sub(/^ /,""); print}' "$TMP/fsout"; }

# 1. Registration.  nblkdev 0 before anything registers is the state §8a step 5
# shipped, and it is what makes bio.c:352 reject a device rather than index an
# empty table.  The two rejections are ioconf.c's dense-prefix invariant: a row
# with a null d_open is a row upstream's own counter (main.c:218) would stop at,
# and a null d_strategy is what bread:115 dereferences without checking.
check "no block device before one registers"	"0"	"$(f nblkdev-before)"
check "bdconf refuses a null d_open"		"-1"	"$(f bdconf-rejects-nullopen)"
check "bdconf refuses a null d_strategy"	"-1"	"$(f bdconf-rejects-nullstrat)"
check "and a refused row does not count"	"0"	"$(f nblkdev-after-rejects)"
check "the driver gets major 0"			"0"	"$(f bdconf-major)"
check "and nblkdev is then 1"			"1"	"$(f nblkdev-after)"

# THE MINOR NUMBER IS THE BLOCK SIZE, which is easy to get wrong invisibly:
# param.h's BITFS(dev) is `dev & 64', so bit 6 selects a 4096-byte filesystem.
# mkfs writes 1024 here.  A minor with that bit set would make every
# BSIZE/BMASK/itod in the kernel describe a different disk, and the first
# symptom would be a garbage superblock.
check "minor 0 means a 1024-byte filesystem"	"0"	"$(f bitfs)"
check "so BSIZE(dev) is 1024"			"1024"	"$(f bsize)"

# 2. Startup.  v8k_kinit is the line that first executes bio.c and iget.c.
check "the kernel comes up"			"0"	"$(f kinit)"
# TWO reads and not one, which is the assertion: iinit breads the superblock,
# and the two igets of ROOTINO bread the ilist block -- the second iget finds
# the inode already in core and does no I/O.  A number other than 2 means the
# buffer cache is either missing a hit or doing a read nobody asked for.
check "and it took exactly two block reads"	"2"	"$(f reads-after-kinit)"

# getfs() PANICS with "getfs" when nothing is mounted, so reaching these lines
# at all is the mount assertion; the numbers say it is the right superblock.
# 2000 is the size given to mkfs.  83 is upstream's proto path adding three
# where its numeric path adds two -- tests/mkfs asserts the same number from
# the bytes, so the two suites agree by independent routes.
check "getfs finds the filesystem's size"	"2000"	"$(f fs-fsize)"
check "and its ilist size"			"83"	"$(f fs-isize)"
check "iinit named the mount point /"		"/"	"$(f fs-fsmnt)"
check "and the superblock carries a date"	"1"	"$(f fs-time-nonzero)"
# B_LOCKED is what stops the superblock buffer from ever being reused.  Without
# it the free-block map could be silently re-read from disk mid-allocation.
check "the superblock buffer is pinned"		"1"	"$(f superb-locked)"

# 3. The root inode, read out of the ilist by iget.
check "the root is inode 2"			"2"	"$(f root-ino)"
check "and it is a directory"			"1"	"$(f root-isdir)"
# TWO igets of the same inode, so i_count is 2 -- main.c:92-95 does this
# deliberately, so that releasing the current directory cannot free the root.
check "held twice, by rootdir and u_cdir"	"2"	"$(f root-count)"
# . .. and sub.  A link count of 2 would mean mkfs's subdirectory never landed,
# which would make the /sub/deep case below fail for an unrelated reason.
check "with three links: . .. and sub"		"3"	"$(f root-nlink)"
check "u_cdir is the root too"			"1"	"$(f cdir-is-root)"

# 4. namei.  "/" is fsnami's null-name arm and must give back the root rather
# than an error -- the one path with no component in it at all.
check "namei / is the root"			"2"	"$(f nami-slash)"

check "namei /hello finds inode 3"		"3"	"$(f hello-ino)"
check "a plain file, mode 0644"			"100644" "$(f hello-mode)"
check "27 bytes, as the inode says"		"27"	"$(f hello-size)"
check "and readi returned 27"			"27"	"$(f hello-n)"
check "with the bytes that went in"  "hello from a V8 filesystem." "$(f hello-text)"

# Two components, so fsnami loops and dsearch runs against a subdirectory
# rather than against the root it was handed.
check "namei /sub/deep walks two components"	"5"	"$(f deep-ino)"
check "and finds a 28000-byte file"		"28000"	"$(f deep-size)"

# bmap's two arms.  Logical block 0 is a direct address in the inode; logical
# block 10 is the first that must be fetched from the single indirect block,
# because blocks 0..NADDR-4 are direct.  Both valid AND different is the pair:
# an indirect arm that fell through to the direct one would return the same
# address twice, and either check alone would pass.
check "bmap resolves a direct block"		"1"	"$(f bmap-0-valid)"
check "and an indirect one"			"1"	"$(f bmap-10-valid)"
check "and they are different blocks"		"1"	"$(f bmap-differs)"

check "readi returns the whole file"		"28000"	"$(f deep-n)"
check "starting with the first line"	"line 0000 abcdef" "$(f deep-head)"
# The tail is the case the indirect block can fail: a wrong indirect address
# gives a right beginning and a wrong end.
check "and ending with the last"	"cdefghijklmnopq." "$(f deep-tail)"
check "the probe wrote what it read"		"1"	"$(f deep-written)"

# THE CENTRAL CLAIM, and it is a cmp rather than a hash.  fsprobe.c also prints
# a rolling sum, but a sum agreeing proves only that two implementations of an
# invented hash agree; this compares V8's kernel's answer against the actual
# file mkfs was handed.
if cmp -s "$FSTMP/big.txt" "$FSTMP/readback"; then pass=$((pass+1))
else bad "the bytes V8's kernel read back differ from the file mkfs was given" \
	 "$(cmp "$FSTMP/big.txt" "$FSTMP/readback" 2>&1 | head -2)"; fi

# 5. The failure paths.  Both are namei returning NULL and they differ ONLY in
# u_error, so a lookup failing for the wrong reason is invisible without the
# pair.  2 is ENOENT and 20 is ENOTDIR.
check "a missing name returns null"		"1"	"$(f enoent-null)"
check "with ENOENT"				"2"	"$(f enoent-err)"
check "a name under a plain file returns null"	"1"	"$(f notdir-null)"
check "with ENOTDIR"				"20"	"$(f notdir-err)"

# 6. The buffer cache, asked directly, with its negative control.  Every case
# above would pass identically against a cache that never hit -- and the hit
# case alone would pass against one that never did any I/O at all.
check "a second bread of a cached block hits"	"1"	"$(f cache-b-cache)"
check "and reaches no driver"			"1"	"$(f cache-no-io)"
check "while an uncached block does one read"	"1"	"$(f cache-miss-io)"

# 7. The READ path sent nothing to the DEVICE, which is a narrower claim than
# the two this case made before it and is the only true one.
#
# It first said it was protecting "an image tests/mkfs validated" -- the image
# is built fresh above, in $TMP, for this probe alone.  It then said the read
# path "dirties no buffer": rdwri.c:50 sets IACC at the top of readi and iput's
# IUPDAT bdwrites the inode, so it dirties one per lookup.  Mutating bdwrite to
# be synchronous turned this case red, which is how that was found.
#
# What is left is bdwrite's contract: dirty buffers stay in the cache.  That is
# worth asserting -- it says the buffer cache is write-back and not write-
# through -- and `and bdwrite reaches no driver' below is its pair for a write.
check "the read path sent nothing to the device" "0"	"$(f writes-after-read)"
check "and it did reach the driver"		"1"	"$(f reads-total-positive)"

# ---------------------------------------------------------------------------
# 8. THE WRITE HALF -- §8a step 5d.  Step 5c drove namei/iget/bmap/readi/bread
# and left every one of their siblings unexecuted: bmap's ALLOCATING arm,
# alloc() and free(), ialloc() and ifree(), writei, itrunc, and nami.c's
# NI_CREAT and NI_DEL.
#
# The instrument is the superblock's own s_tfree/s_tinode rather than a count of
# device writes, because a device count depends on when a 32-buffer cache
# evicted something -- the host-property class these suites are swept for.  The
# one exception is the delayed-write case, where not doing a device write IS the
# property.
#
# THE ACCEPTANCE TEST IS AT THE END OF THIS BLOCK AND IT IS NOT A PROBE CASE:
# icheck, dcheck and fsck are handed the image afterwards.  Three programs of
# Bell Labs', which know nothing about this one, asked whether what V8's kernel
# wrote is a filesystem.
# ---------------------------------------------------------------------------
check "the free-block count starts positive"	"1"	"$(f w-tfree-start-positive)"
check "and so does the free-inode count"	"1"	"$(f w-tinode-start-positive)"

# 8.0 The u-area, asserted before anything reads it.  writei's IFREG arm tests
# u_offset+u_count against u_limit[LIM_FSIZE], and out of bss that is 0, so
# EVERY write to a regular file failed -- with EMFILE, upstream's own choice at
# rdwri.c:167, which reads as a file-table problem and cost a debugging round
# before shim/kern/sys/main.c grew v8k_uinit().  Both values are the port's own
# (vlimit.h's INFINITY, param.h's CMASK), not the host's.
check "v8k_kinit set the file-size limit"	"1"	"$(f w-limit-fsize)"
check "and the creation mask"			"1"	"$(f w-limit-cmask)"

# 8a. A write inside a block that already exists allocates nothing, and takes
# writei's bread+bdwrite arm.  bdwrite hands the block to the cache and the
# cache does not hand it to the driver, so `writes' must not move -- that is the
# only observable difference between a delayed write and a synchronous one, and
# it is the one place a device count is the assertion.
check "/hello is there to write to"		"1"	"$(f w-hello-found)"
check "an in-place write moves 5 bytes"		"5"	"$(f w-inplace-n)"
check "and allocates nothing"			"1"	"$(f w-inplace-noalloc)"
check "and bdwrite reaches no driver"		"1"	"$(f w-inplace-delayed)"
check "and readi gives the new bytes back"	"1"	"$(f w-inplace-readback)"

# 8b. Extending into a direct block that does not exist runs bmap's `nb == 0'
# arm -- the first line of alloc() this port has ever executed.  One block, so
# s_tfree drops by exactly one, and i_size becomes offset+count because
# writei:224-226 only ever grows it.
check "extending writes 6 bytes"		"6"	"$(f w-extend-n)"
check "and allocates exactly one block"		"1"	"$(f w-extend-alloc1)"
check "and i_size is offset plus count"		"2054"	"$(f w-extend-size)"
# The skipped block has no disk block at all.  bmap answers -1 for it, NOT 0
# (subr.c:31), and rdwri.c:85-87 turns that into geteblk+clrbuf -- so the zeros
# come from a buffer attached to no device rather than from a cleared block.
check "and the hole between reads as zero"	"1"	"$(f w-hole-zero)"

# 8c. Past block 9, bmap must allocate the INDIRECT block as well as the data
# block: two allocations for one logical block, subr.c:69-80 then :91-113.  The
# pair is the only thing that distinguishes this from 8b.
check "writing past block 9 moves 8 bytes"	"8"	"$(f w-indirect-n)"
check "and allocates TWO blocks, not one"	"1"	"$(f w-indirect-alloc2)"
check "and bmap can find block 10 again"	"1"	"$(f w-indirect-bmap)"
check "and the bytes come back"			"1"	"$(f w-indirect-readback)"

# 8c-bis. THE SIGNAL THAT MUST NOT BECOME A BROADCAST.  writei's over-limit arm
# is psignal(u.u_procp, SIGXFSZ) and this port's psignal is a real kill(2), so
# imported kernel code reaches out and touches the host there.
#
# THIS CASE EXISTS BECAUSE A MUTATION FOUND THE BUG BY KILLING THE TEST RUNNER.
# Mutating v8k_uinit to leave u_limit 0 sent every write down that arm, and the
# run produced no failing case at all -- it produced a dead shell.  fsprobe does
# not call v8k_procinit, so v8k_hostpid was 0, v8k_hostof returned 0, psignal's
# guard was `hp < 0', and the syscall was kill(0, SIGXFSZ): the whole process
# group.  Both functions now refuse a host pid of 0.
#
# `survived' looks like a strange thing to assert and is the only thing that can
# be asserted about a signal that must not arrive -- if it does, this line is
# never printed and the case reports an empty `got'.  EMFILE is its pair, so a
# writei that never reached the arm cannot pass it either.
check "an over-limit write is refused"		"1"	"$(f w-xfsz-refused)"
check "with EMFILE, upstream's own choice"	"24"	"$(f w-xfsz-emfile)"
check "and the process is still alive"		"1"	"$(f w-xfsz-survived)"

# 8d. ialloc and ifree.  A fresh inode has i_mode 0 -- that is ialloc's own test
# at alloc.c:302 for whether the number it pulled is really free -- and a number
# above ROOTINO.  It is freed by iput with i_nlink 0, which is how every caller
# does it and is what reaches ifree through iget.c:196; calling ifree by hand
# would leave the inode in core and prove less.
check "ialloc hands out an inode"		"1"	"$(f w-ialloc-ok)"
check "with i_mode 0"				"1"	"$(f w-ialloc-mode0)"
check "and a number above the root's"		"1"	"$(f w-ialloc-above-root)"
check "and the free-inode count drops by one"	"1"	"$(f w-ialloc-tinode)"
check "and iput with nlink 0 gives it back"	"1"	"$(f w-ifree-tinode)"

# 8e. namei with NI_CREAT: dsearch fails, nami.c:494 runs ialloc, iupdat writes
# the inode, and writei(dp) puts the NAME in the parent directory -- the part no
# direct ialloc can reach.  The mode carries no IFMT on purpose, so nami.c:503
# has to add IFREG.  This is also the first call in the port's history to reach
# access(dp, IWRITE), which is why §8a step 5d restored v8fs.c's s_ronly arm.
check "NI_CREAT returns an inode"		"1"	"$(f w-creat-ok)"
check "and nami.c added IFREG itself"		"1"	"$(f w-creat-isreg)"
check "with the mode asked for"			"644"	"$(f w-creat-perm)"
check "one link"				"1"	"$(f w-creat-nlink)"
check "and no blocks yet"			"0"	"$(f w-creat-size)"

# 8f. DRAIN THE SUPERBLOCK'S FREE LIST.  alloc() hands out s_free[--s_nfree]
# until s_nfree hits 0 and only THEN follows the chain (alloc.c:163-176): bread
# the block it just handed out, take df_nfree and df_free from it, carry on.
# That is V7's struct fblk, 716 bytes, the record sys/fblk.h had never been
# imported for -- and mkfs wrote it.  So this is the second half of step 5c's
# claim, for metadata rather than data.
#
# The count is READ FROM THE SUPERBLOCK and the probe then allocates s_nfree+24
# blocks, so the margin cannot go stale.  Every one of those writes succeeding
# is itself the proof: without a refill, alloc() takes `goto nospace' and the
# writes return ENOSPC.
check "every block of a bulk write landed"	"1"	"$(f w-bulk-blocks)"
check "and i_size is the whole of it"		"1"	"$(f w-bulk-size)"
check "and the free list refilled from the chain" "1"	"$(f w-drain-refilled)"
# At least one block per block written; more, because blocks 10.. need indirect
# blocks too.  A lower bound rather than a number, because how many indirect
# blocks a file of this size takes is a fact about NINDIR and not about alloc().
check "and s_tfree fell by at least that many"	"1"	"$(f w-bulk-tfree-atleast)"
check "and a block near the far end reads back"	"1"	"$(f w-bulk-readback)"

# 8g. An ORDINARY namei finds it.  Without this, 8e proves only that ialloc
# returned an inode; the directory entry writei(dp) wrote is what makes it a
# file with a name.
check "a plain lookup finds the created name"	"1"	"$(f w-lookup-found)"
check "at the size it was written to"		"1"	"$(f w-lookup-size-matches)"

# 8h. NI_DEL.  nami.c:298-328 clears d_ino, writei()s the directory -- writei's
# ONE synchronous arm, rdwri.c:207-217, guarded on exactly that -- and iputs the
# target with nlink now 0, reaching itrunc, which walks NADDR-1 down to 0 handing
# every block back to free(), then ifree for the inode.
#
# A SUCCESSFUL DELETE RETURNS NULL: fsnami's arm ends `goto out' and namei's
# case 1 returns NULL.  u_error is the only thing separating success from
# failure, the same shape as the enoent/notdir pair above.
check "NI_DEL returns null"			"1"	"$(f w-del-null)"
check "with no error, which is what says it worked" "0"	"$(f w-del-err)"
check "and the name is gone"			"1"	"$(f w-del-gone)"
check "with ENOENT"				"2"	"$(f w-del-gone-err)"

# THE ROUND TRIP, and it is the strongest single pair here.  Everything 8e and
# 8f took -- one inode and several hundred blocks -- has come back, so s_tinode
# is exactly what it was before the create and s_tfree exactly what it was after
# 8c.  An off-by-one anywhere in alloc, free, ialloc, ifree or itrunc moves one.
check "every inode taken came back"		"1"	"$(f w-roundtrip-tinode)"
check "and every block"				"1"	"$(f w-roundtrip-tfree)"

# 8h-bis. THE s_ronly ARM, MADE TO FIRE.  Restoring upstream's read-only check
# is not the same as exercising it: iinit sets s_ronly = 0 and nothing else ever
# sets it, so the arm was read on every create and could never be taken -- "a
# guard that has never been seen to fail is not a guard", pointed out by the
# lp64-auditor.  The probe sets the superblock field by hand, which is what
# mount(2) would do if smount were imported, and puts it back.
#
# EROFS (30) rather than EACCES is the point: uid is 0, so every other arm of
# access() short-circuits at `u.u_uid == 0' and this is the only one that can
# refuse a create to root.  The pair is what makes it a measurement -- the same
# create with the flag cleared has to SUCCEED, or the case would pass against an
# access() that refused everything.
check "a create on a read-only filesystem is refused" "1" "$(f w-ronly-refused)"
check "with EROFS, the only arm root cannot pass"     "30" "$(f w-ronly-err)"
check "and the same create works once it is cleared"  "1" "$(f w-ronly-cleared)"
# The file it just made has to go back through NI_DEL rather than by hand.  The
# first draft freed the inode and left the directory entry pointing at it; the
# three cases above stayed green and FSCK caught it -- "FILE SYSTEM WAS
# MODIFIED" plus a cmp difference.  The acceptance test finding a bug in the
# probe is the same argument as it finding one in the kernel.
check "and the probe cleaned up after itself"         "1" "$(f w-ronly-cleaned)"

# 8i. update() is sync(2)'s internal name and it does the whole job -- modified
# superblocks, modified inodes, and then its own last statement, bflush(NODEV)
# at alloc.c:530, which pushes every remaining B_DELWRI buffer at the driver.
# Until it runs, most of the work above is in a 32-buffer cache and the image on
# disk is a lie.
#
# This comment used to say "update() ... and bflush(NODEV)" as though the probe
# called both, and the probe did -- a second, DEAD bflush that changed nothing
# because update() had already flushed.  Deleted; the lp64-auditor found it and
# deleting it moved no case, which is what dead means.
check "the flush reached the driver"		"1"	"$(f w-flush-wrote)"
check "and the run wrote something overall"	"1"	"$(f w-writes-total-positive)"

# ---------------------------------------------------------------------------
# 8j. THE ACCEPTANCE TEST.  Three of Bell Labs' own programs, which know nothing
# about the probe, are handed the image V8's kernel just wrote.
#
# This is the property step 5c could not have: a reader agreeing with a writer
# proves they share a belief, and the two halves of THIS probe are one program.
# icheck walks the image independently and recomputes the block accounting;
# dcheck walks the directory tree and recomputes the link counts; fsck does both
# and REPAIRS, so its silence is the strongest of the three.
# ---------------------------------------------------------------------------
# EVERY CAPTURE HERE IS BOUNDED IN BOTH DIRECTIONS, and that is not tidiness.
# A mutation that corrupted the free list made fsck print for forty seconds and
# the SHELL died -- `xrealloc: cannot allocate 18446744071562067968 bytes' -- so
# the run produced no summary and no diagnosis.
#
# TWO BOUNDS, BECAUSE THEY CATCH DIFFERENT FAILURES.  `head -200' bounds VOLUME
# and kills a runaway that prints, by SIGPIPE; the deadline bounds TIME and is
# the only thing that catches one that loops silently -- which is not
# hypothetical for a V8 checker on a damaged image, since CLAUDE.md records
# fsck live-locking with an empty stdout on exactly that.  The first draft of
# this block gave the deadline to fsck alone and then claimed in this comment
# that all three had one; the lp64-auditor read the sentence against the code.
# On a healthy image they print six lines and finish instantly.
fsdeadline() { perl -e 'alarm 40; exec @ARGV' env V8ROOT=$ROOT/rootfs "$@"; }

icout=$(fsdeadline "$ROOT/rootfs/etc/icheck" "$FSTMP/img" 2>&1 | head -200)
# A POSITIVE CONTROL, AND IT WAS MISSING FROM THE ONE BLOCK OF THE THREE THAT
# NEEDED IT MOST.  The dcheck block below argues at length that a silence case
# needs one, because a checker that failed to run at all is also silent -- and
# icheck's cases are not silence cases, they are VALUE cases, which look immune
# and are not: `awk $1=="missing"' over no output at all yields the empty
# string, so the failure reads as `want [0] got []' and says nothing about why.
#
# Seen twice, consecutively, on a loaded machine and then not again in four
# runs: icheck produced NOTHING, while dcheck and fsck on the same image in the
# same run were fine.  Empty output with a `perl alarm' wrapper is what a
# deadline looks like from outside.  This does not explain it; it makes the
# next occurrence say which of "did not run", "died" and "ran and printed
# nothing" it was.  icheck names the image whatever it finds, so that line is
# the proof it ran -- the same control, and the same sentence, dcheck has.
if [ -z "$icout" ]; then
	bad "icheck produced no output at all" \
	    "the three value cases below cannot mean anything; deadline was 40s"
else
	check "icheck ran on the image" "1" \
	    "$(printf '%s\n' "$icout" | grep -c "^$FSTMP/img")"
fi
# `missing' is icheck's count of blocks that are in neither a file nor the free
# list -- exactly what a leak in alloc/free/itrunc produces, and the reason this
# is the first line to look at.
check "icheck finds no missing blocks" "0" \
    "$(printf '%s\n' "$icout" | awk '$1=="missing"{print $2}')"
# used + free must be every block outside the ilist.  fsize and isize come from
# the probe, which read them out of the superblock through getfs.
icused=$(printf '%s\n' "$icout" | awk '$1=="used"{print $2}')
icfree=$(printf '%s\n' "$icout" | awk '$1=="free"{print $2}')
check "and used+free accounts for every block" \
    "$(( $(f fs-fsize) - $(f fs-isize) ))" "$(( icused + icfree ))"

# dcheck prints one line per disagreement between an inode's link count and the
# number of directory entries pointing at it.  Silence is the assertion, and it
# is the one that would catch NI_CREAT or NI_DEL getting i_nlink wrong.
#
# A SILENCE CASE NEEDS A POSITIVE CONTROL, because a dcheck that failed to run
# at all is also silent -- the "a case that silently disappears is worse than
# one that asserts a host property" rule.  dcheck names the image it checked
# whatever it finds, so that line is the proof it ran.
dcraw=$(fsdeadline "$ROOT/rootfs/etc/dcheck" "$FSTMP/img" 2>&1 | head -200)
dcout=$(printf '%s\n' "$dcraw" | grep -v "^$FSTMP/img" | grep -v '^$')
check "dcheck ran on the image" "1" \
    "$(printf '%s\n' "$dcraw" | grep -c "^$FSTMP/img")"
check "and found no link-count disagreement" "" "$dcout"

# fsck -y so no case can block on stdin, </dev/null so a prompt that ignored -y
# fails rather than waits, and a deadline because a checker that repairs can
# loop.  tests/mkfs uses the same shape and says why.
cp "$FSTMP/img" "$FSTMP/img.prefsck"
fsout=$(fsdeadline "$ROOT/rootfs/etc/fsck" -y "$FSTMP/img" </dev/null 2>&1 |
        head -200)
# fsck's own three numbers, arrived at by a different walk from icheck's.
check "fsck agrees with icheck's three numbers" \
    "$(printf '%s\n' "$icout" | awk '$1=="files"{print $2}') files $icused blocks $icfree free" \
    "$(printf '%s\n' "$fsout" | grep 'files.*blocks.*free' | sed 's/^ *//')"
# The positive control for the silence case below, and it is not decoration:
# phase 5 is the FREE LIST, which is precisely what alloc() drained and refilled
# and what itrunc handed everything back to.  A fsck that never got that far
# would leave "nothing modified" true and meaningless.
check "and reached phase 5, the free list" "1" \
    "$(printf '%s\n' "$fsout" | grep -c 'Phase 5')"
check "and reports nothing modified" "" \
    "$(printf '%s\n' "$fsout" | grep 'FILE SYSTEM WAS MODIFIED')"
# AND THE BYTES SAY SO TOO.  fsck printing no complaint and fsck changing
# nothing are two different claims; tests/mkfs learned to assert the second.
if cmp -s "$FSTMP/img.prefsck" "$FSTMP/img"; then pass=$((pass+1))
else bad "fsck modified the image V8's kernel wrote" \
	 "$(cmp "$FSTMP/img.prefsck" "$FSTMP/img" 2>&1 | head -2)"; fi

# 9. No inode leaked.  ONE held at the end -- the root -- with i_count 2,
# because rootdir and u_cdir are two igets of the same (dev, ROOTINO) and the
# second finds it in the hash rather than taking a second slot.  This is the
# only case here that constrains the inode table, and it exists because
# mutation showed nothing else did: NINODE 80 -> 3 -> 2 all passed 308, and
# only NINODE 1 failed.  A missing iput would otherwise be invisible.
check "exactly one inode is still held"		"1"	"$(f inodes-held)"
check "and it is the root, held twice"		"2"	"$(f root-count-final)"
# Reaching the last line means none of v8fs.c's five PANIC services ran and
# neither getfs nor namei panicked, since panic() does not return.
check "no panic service was reached"		"1"	"$(f completed)"

# --- §8a step 5e: THE SERVER --------------------------------------------------
#
# Everything above drives the kernel IN PROCESS.  These cases drive it over a
# socket: shim/v8fsd/v8fsd.c holds an image open, speaks 9P2000, and
# tests/streams/p9probe.c asks it for the same file fsprobe read and compares
# the bytes.
#
# WHY THERE IS A SERVER AT ALL is the previous commit's measurement, not a
# preference: linking libv8kern beside a V8 program is 56 symbol collisions
# over 29 programs, 25 of them silent, and a descriptor table in process memory
# does not survive exec.  shim/kern/NOTES.md has both.
#
# THE PROBE IS AN INDEPENDENT READER, which is the property §8a step 5d had to
# learn the hard way: a probe that writes and reads is one program sharing its
# own beliefs, and the mutation that made alloc() hand out a block twice was
# caught only by icheck, fsck and cmp.  This one speaks the wire, so it can ask
# for a clunked fid, a walk off the end of a path and a write to a read-only
# server -- none of which the shim would ever send.
P9PROBE=$TMP/p9probe
V8FSD=$BUILD/v8fsd/v8fsd

if [ ! -x "$V8FSD" ]; then
	bad "9P: $V8FSD not built (run make first)"
elif [ ! -f "$FSTMP/p9img" ]; then
	bad "9P: no image -- the step 5c section did not get that far"
elif ! clang -std=gnu99 -Wall -o "$P9PROBE" "$ROOT/tests/streams/p9probe.c" \
     "$ROOT/shim/p9/p9.c" "$ROOT/shim/p9/p9io_libc.c" > "$TMP/p9build.log" 2>&1; then
	bad "9P: p9probe build failed" "$(head -3 "$TMP/p9build.log")"
else

# THE SOCKET PATH IS BOUND RELATIVE, AND THAT IS NOT TIDINESS.  sun_path is 104
# bytes on Darwin and $TMPDIR alone is around 50 on a Mac, so an absolute path
# here would be a case that passes on one machine and fails on another for a
# reason nothing in the output would name -- the host-property trap, arriving
# through a struct field.  cd into the directory and the length is bounded by
# the name.  (The server checks and says so; it is how this was found.)
P9DIR=$FSTMP
rm -f "$P9DIR/sock"
( cd "$P9DIR" && exec "$V8FSD" -r sock p9img ) > "$TMP/p9srv.out" 2>&1 &
P9PID=$!

# READY IS READ FROM THE SERVER, not waited for.  A sleep is a race dressed as
# a delay, and a connect() retry loop cannot tell "not yet" from "died on the
# image" -- which is the failure this would most like to report clearly.  The
# server prints one line and flushes; ten tries at 0.2s is five seconds, which
# is two orders of magnitude more than it takes.
p9ready=0
for _i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25; do
	grep -q "^v8fsd ready" "$TMP/p9srv.out" 2>/dev/null && { p9ready=1; break; }
	kill -0 $P9PID 2>/dev/null || break
	sleep 0.2
done

if [ "$p9ready" != 1 ]; then
	bad "9P: the server did not come up" "$(head -3 "$TMP/p9srv.out")"
else
check "the server announces itself" "1" "$p9ready"

# A DEADLINE, for fsprobe's reason and one of its own: every case here is a
# round trip, so a server that answers nothing leaves the probe blocked in
# read(2) forever and the suite reports nothing at all.
( cd "$P9DIR" && perl -e 'alarm 60; exec @ARGV' "$P9PROBE" sock p9readback ) \
	> "$TMP/p9out" 2>"$TMP/p9err"
p9rc=$?
kill $P9PID 2>/dev/null; wait $P9PID 2>/dev/null

if [ $p9rc -ne 0 ]; then
	bad "p9probe exited nonzero ($p9rc)" "$(head -3 "$TMP/p9err")"
else
q() { awk -v k="$1" '$1 == k {print $2}' "$TMP/p9out"; }

# 1. The handshake.  Tversion is the one message that may arrive mid-stream and
# mean "start again", so getting it right is what makes every case below
# meaningful rather than accidental.
check "the server speaks 9P2000"		"9P2000" "$(q version)"
check "and never proposes a larger msize than ours" "1"	"$(q msize-le-ours)"
check "attach gives a directory qid"		"1"	"$(q root-qtdir)"
# ROOTINO is 2 on a V7 filesystem (h/param.h:73), not 1 -- inode 1 was the
# bad-block file.  The qid path IS the inode number, so this is the assertion
# that the server named the file rather than invented a handle for it.
check "and its qid path is the root inode"	"2"	"$(q root-qpath)"

# 2. Walking, through Bell Labs' own namei -- v8fsd.c's kwalk moves u_cdir and
# hands it one name, so the lookup a Twalk performs IS the lookup the kernel
# performs.
check "a one-name walk returns one qid"		"1"	"$(q walk-hello)"
check "and hello is not a directory"		"0"	"$(q hello-isdir)"
check "a two-name walk returns two"		"2"	"$(q walk-sub-deep)"
check "and sub/deep is not a directory"		"0"	"$(q deep-isdir)"
check "a zero-name walk clones"			"0"	"$(q walk-clone)"
check "and the clone is the root"		"1"	"$(q clone-isdir)"

# THE PAIR IS THE POINT.  9P says a walk that fails on the FIRST name is an
# Rerror and one that fails later is a SHORT Rwalk, and a server that answered
# them the same way would be indistinguishable from a correct one on every path
# that exists.  A client tells "no such file" from "no such directory on the
# way to it" by exactly this difference.
check "a missing first name is an error"	"-1"	"$(q walk-missing)"
check "and it is ENOENT"			"ENOENT" "$(q walk-missing-err)"
check "a missing later name is a SHORT walk"	"1"	"$(q walk-short)"
check "and so is walking through a plain file"	"1"	"$(q walk-thru-file)"
# An open fid may not be walked -- the spec, and the reason is that a walk
# would move the file an offset already refers to.
check "an opened fid cannot be walked"		"-1"	"$(q walk-open-fid)"

# 3. Stat.  The two length prefixes are 9P2000's one real wart and the
# difference is asserted rather than described: conflating them lands two bytes
# into the name, which decodes as a plausible short string rather than an error.
check "a stat's two counts differ by exactly two" "1"	"$(q stat-outer-inner-differ-by-2)"
# THE QID PATH IS THE ONLY FIELD THAT IDENTIFIES THE FILE, and the case beside
# it cannot: s_name is the name the client sent in the Twalk, echoed back out of
# the fid, so a server that walked to the wrong inode would still print it.
# ncheck says /hello is inode 3 on this image, independently of anything here.
check "stat's qid path is the inode ncheck names" "3"	"$(q stat-qpath)"
check "stat names the file"			"hello"	"$(q stat-name)"
# 27 bytes is `hello from a V8 filesystem\n', which the section above wrote.
check "and reports its length"			"27"	"$(q stat-len)"
check "and its mode"				"644"	"$(q stat-mode)"
# A DECIMAL STRING, not a login name: 9P wants a name and a V7 inode stores a
# number, and turning one into the other means reading a passwd file -- of
# which there are two here, with no principled way to choose.  v8fsd.c argues it.
check "and its owner as a number"		"0"	"$(q stat-uid)"
check "and does not call it a directory"	"0"	"$(q stat-isdir)"

# 4. Reading.  THE CENTRAL CLAIM, and it is a cmp rather than a checksum.
check "a read on an unopened fid is refused"	"EBADF"	"$(q read-unopened)"
check "open succeeds"				"1"	"$(q open-deep)"
check "and reports an iounit"			"1"	"$(q open-iounit-positive)"
check "the whole file comes back"		"28000"	"$(q read-total)"
# A read PAST the end is zero bytes and not an error -- V8's own readi does the
# same, and every copy loop in the tree terminates on it.
check "a read past the end is zero bytes"	"0"	"$(q read-past-end)"
if cmp -s "$FSTMP/big.txt" "$P9DIR/p9readback"; then pass=$((pass+1))
else bad "the 28000 bytes that came over the wire are not the ones that went in" \
	 "$(cmp "$FSTMP/big.txt" "$P9DIR/p9readback" 2>&1 | head -2)"; fi

# 5. Directory reads, which are 9P STATS and not the raw 16-byte records the
# image holds -- that is what makes this plain 9P2000 rather than a private
# protocol.  "." and ".." are in the listing against the Plan 9 convention,
# because they are entries this filesystem CONTAINS and hiding them would be
# adopting Plan 9 semantics above the seam, which PLAN.md §8a rules out.
check "the root directory opens"		"1"	"$(q open-root)"
check "and lists four entries"			"4"	"$(q dir-entries)"
check "including ."				"1"	"$(q dir-dot)"
check "and .."					"1"	"$(q dir-dotdot)"
check "and hello"				"1"	"$(q dir-hello)"
check "and sub"					"1"	"$(q dir-sub)"

# 6. What it refuses.  The write half is §8a step 5f; the kernel underneath it
# is written and tested, so this is a boundary in the server rather than a gap
# in the port, and EROFS is the honest word for it.
check "a write is refused"			"EROFS"	"$(q write-refused)"
check "clunk succeeds"				"1"	"$(q clunk)"
check "and the fid is gone afterwards"		"-1"	"$(q stat-after-clunk)"
# A message type the server does not know must be an Rerror rather than
# silence: a server that ignored one would leave the client waiting forever,
# which reads as a hang rather than as an error.
check "an unknown message type is refused"	"EINVAL" "$(q unknown-type)"
check "and Tauth, which this server does not offer" "EPERM" "$(q auth-refused)"

# 7. WHAT A HOSTILE OFFSET MUST NOT DO.  A directory offset is unsigned on the
# wire and the server's bound was signed, so every offset at or above 2^63 read
# as negative and passed: measured before the fix, 2^64-1 returned the listing
# shifted one byte below its buffer, -7973 returned 7973 bytes of heap, and
# -2^40 was a SIGSEGV that took every other connection down with it.  Four
# messages, no authentication.  The file arm eight lines away in the server had
# the guard all along -- the fix landed on one line and the line beside it kept
# the assumption, which is this port's most repeated shape.
check "a directory read at 2^64-1 is refused"	"EINVAL" "$(q dir-read-huge-offset)"
check "and one that reads as negative"		"EINVAL" "$(q dir-read-negative-offset)"
# ...and an offset INSIDE an entry, which the server's own comment claimed was
# refused while nothing enforced it.  Three bytes in returns a length field out
# of the middle of a name.
check "and one inside an entry"			"EINVAL" "$(q dir-read-mid-entry)"
# The loudest of the three: if any of the above crashed the server, every case
# from here down fails as well.
check "and the server is still there"		"1"	"$(q alive-after-bad-offsets)"

# A WALK ELEMENT IS ONE PATH COMPONENT.  namei splits on `/' and restarts at the
# root for a leading one, so an unvalidated wname let "sub/deep" traverse two
# components while reporting one qid, and "/hello" escape the directory the fid
# was walked from.  Not a containment hole -- namei cannot leave the image --
# but the qid count is how a client tells how much of its path exists.
check "a wname containing a slash is refused"	"-1"	"$(q walk-embedded-slash)"
check "and one beginning with a slash"		"-1"	"$(q walk-leading-slash)"
check "and the empty name"			"-1"	"$(q walk-empty-name)"

# A REFUSED Tversion MUST NOT HAVE RESET THE CONNECTION.  The clunk loop ran
# before the msize was validated, so a client whose proposal was rejected was
# told the negotiation failed and silently lost every fid it held.
check "a tiny msize is refused"			"EINVAL" "$(q version-tiny-msize)"
check "and the fids survive the refusal"	"1"	"$(q fid-survives-failed-version)"

# 8. A SECOND CONNECTION, which is what the poll() loop is FOR and what nothing
# above touches: every case so far uses one, so a server that accepted a second
# and then ignored it would have passed all of them.  One connection per open
# file is the design -- the socket IS the descriptor -- so two at once is the
# ordinary case rather than the exotic one.
check "a second connection negotiates"		"1"	"$(q conn2-version)"
# THE SHARP ONE.  It attaches as fid 0, which is in use on the first
# connection.  9P fid spaces are per connection, so this must succeed; a server
# with one shared table would answer EEXIST and would have looked correct to
# every case above.
check "and attaches as a fid the OTHER one holds" "1"	"$(q conn2-attach-same-fid)"
check "and walks"				"1"	"$(q conn2-walk)"
check "and opens"				"1"	"$(q conn2-open)"
check "and reads hello's 27 bytes"		"27"	"$(q conn2-read)"
# Interleaved deliberately: six messages have gone by on the second connection
# since the first said anything, so a fid table belonging to the server rather
# than to the connection would have been overwritten by now.
check "the first connection still knows its fids" "1"	"$(q conn1-still-alive)"
# A close is the ordinary end of a connection -- a V8 program exiting closes
# every descriptor it holds -- so a server that took SIGPIPE or dropped its
# poll loop here would take every other client with it.
check "and survives the second one closing"	"1"	"$(q conn1-after-conn2-closed)"

# ===========================================================================
# 9. THE CLIENT -- §8a step 5e's other half, and the only section in this file
#    that runs no probe at all.
#
# Everything above this line is a program written to test something.  These
# cases run the SHIPPED BINARIES -- rootfs/bin/cat, ls, sh, rm and usr/bin/tail
# -- against a mount, with no argument they would not have been given in 1985.
# That is the whole claim of the step: not that a 9P server answers correctly,
# which section 8 established, but that Bell Labs' namei, iget, bmap and readi
# running in another process are reachable by `cat FILE'.
#
# THE SOCKET PATH IS RELATIVE FOR THE CLIENT TOO, and for the server's reason
# one step further along: sun_path is 104 bytes, $TMPDIR is around 50 on a Mac,
# and the client refuses a mount whose socket path will not fit -- silently,
# because a mount that does not exist is not an error.  Found the honest way:
# the first end-to-end run of this used an absolute path under the session
# scratch directory, at 130 characters, and cat said "No such file or
# directory" about a file that was there.  cd into the directory and the length
# is bounded by the name.
#
# V8MOUNT is set per-command rather than exported, so that the LAST case can
# ask what happens without it -- which is the control that says the mount is
# what does this rather than something in the jail.
# A SECOND SERVER, because line 1715 killed the first one the moment the probe
# exited -- every check in section 8 reads the probe's saved output, so nothing
# there needed it alive and the kill was invisible.  These cases run programs,
# so they do.  Diagnosed by this section failing eleven times with "No such
# file or directory" while the identical command worked by hand.
rm -f "$P9DIR/csock"
( cd "$P9DIR" && exec "$V8FSD" -r csock p9img ) > "$TMP/p9cli.out" 2>&1 &
CPID=$!
cready=0
for _i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25; do
	grep -q "^v8fsd ready" "$TMP/p9cli.out" 2>/dev/null && { cready=1; break; }
	kill -0 $CPID 2>/dev/null || break
	sleep 0.2
done
check "a server for the client half"		"1"	"$cready"

# A DEADLINE ON EVERY ONE OF THEM, for a reason this section is uniquely
# exposed to: a v8fs read is a round trip, so a server that answers nothing
# leaves an ORDINARY V8 PROGRAM blocked in read(2) forever -- and a hang in
# `make test' reports nothing at all, which is strictly worse than a failure.
#
# AND `perl -e "alarm N; exec"' IS NOT THAT DEADLINE, WHICH THIS SECTION FOUND
# BY HANGING FOR TWENTY-SIX MINUTES.  Two independent reasons, and the alarm
# mechanism itself is fine -- measured, `perl -e 'alarm 2; exec @ARGV' sleep 10'
# dies at two seconds with status 142:
#
#   V8's sh CATCHES SIGALRM ON PURPOSE.  src/cmd/sh/fault.c:123 is
#   `setsig(SIGALRM)' and :94 handles it -- the shell uses alarm(2) itself, for
#   $TIMEOUT (main.c:224) and for the fork retry (xec.c:466).  So the signal
#   that was supposed to kill it is one it was written to survive.
#
#   AND A DEADLINE ON ONE END OF A PIPE IS NOT A DEADLINE ON THE PIPE, which
#   CLAUDE.md already records from the ttyld harness.  Even against a program
#   that does die, `sh -c "cat < /mnt/f"' leaves cat holding the stdout the
#   command substitution is reading, so $( ) never returns.
#
# So the child is put in its own PROCESS GROUP and the whole group is killed.
# That reaches the grandchildren, and it does not care what the child thinks
# about SIGALRM.
deadline() {
	perl -e '
		my $pid = fork();
		die "fork: $!" unless defined $pid;
		if ($pid == 0) { setpgrp(0, 0); exec @ARGV; exit 127; }
		$SIG{ALRM} = sub { kill(-9, $pid); };
		alarm 30;
		waitpid($pid, 0);
		my $st = $?;
		alarm 0;
		exit($st & 127 ? 128 + ($st & 127) : $st >> 8);
	' "$@"
}
# export, NOT `env VAR=v deadline ...' -- env execs a PROGRAM and cannot call a
# shell function, so the first version of this ran /usr/bin/env looking for a
# binary named `deadline'.
v8run()  { ( cd "$P9DIR"; V8ROOT="$ROOT/rootfs"; V8MOUNT="/mnt=csock"
	     export V8ROOT V8MOUNT; deadline "$@" ); }
v8bare() { ( cd "$P9DIR"; V8ROOT="$ROOT/rootfs"; unset V8MOUNT
	     export V8ROOT; deadline "$@" ); }
# The same, mounted somewhere the HOST also has a directory -- see the
# containment cases at the end of this section for why that is not the same
# test twice.
v8host() { ( cd "$P9DIR"; V8ROOT="$ROOT/rootfs"; V8MOUNT="$P9DIR/hostdir=csock"
	     export V8ROOT V8MOUNT; deadline "$@" ); }
CAT=$ROOT/rootfs/bin/cat
LS=$ROOT/rootfs/bin/ls
SH=$ROOT/rootfs/bin/sh
RM=$ROOT/rootfs/bin/rm
TAIL=$ROOT/rootfs/usr/bin/tail

# The headline.  A V8 program, a V8 path, a V8 filesystem, three processes.
check "cat reads a file out of the mount" "hello from a V8 filesystem" \
	"$(v8run "$CAT" /mnt/hello 2>&1)"

# ...and the one that cannot pass by accident: 28000 bytes, two directories
# down, through bmap's indirect arm, compared against what mkfs was handed.
v8run "$CAT" /mnt/sub/deep > "$TMP/mntread" 2>"$TMP/mntread.err"
if cmp -s "$FSTMP/big.txt" "$TMP/mntread"; then pass=$((pass+1))
else bad "the 28000 bytes cat read through the mount are not the ones mkfs was given" \
	 "$(cmp "$FSTMP/big.txt" "$TMP/mntread" 2>&1 | head -2; head -2 "$TMP/mntread.err")"; fi

# A DIRECTORY IS A DIFFERENT PATH ENTIRELY -- 9P stat records converted to V7
# 256-byte records and handed to dir.c's snapshot machinery.  `ls' is the
# reader that made the conversion's first bug visible: it said "/mnt
# unreadable", because open(2) is what fails when the snapshot cannot be built.
check "ls lists the mount"			"hello sub" \
	"$(v8run "$LS" /mnt 2>&1 | tr '\n' ' ' | sed 's/ *$//')"
check "and the subdirectory"			"deep" \
	"$(v8run "$LS" /mnt/sub 2>&1)"
# ls -l is stat, which is a whole connection per entry, and it reports the
# IMAGE's numbers -- mode and size come off a V7 inode mkfs wrote.
check "ls -l reports the image's size"		"27" \
	"$(v8run "$LS" -l /mnt/hello 2>&1 | awk '{print $5}')"
check "and the image's mode"			"-rw-r--r--" \
	"$(v8run "$LS" -l /mnt/hello 2>&1 | awk '{print $1}')"

# THE INHERITANCE CASE, AND IT IS WHY getpeername IS IN THE DESIGN.  sh opens
# the file, dup2s it onto 0 and replaces itself with cat -- so cat's fd 0 is a
# 9P socket in a process whose descriptor table was destroyed by the exec.  A
# client that kept its types in process memory reads a raw socket here and
# BLOCKS FOREVER, because the server sends nothing unsolicited.
#
# It also caught a second bug that a directly-opened file never could: the
# close of the original descriptor was sending a Tclunk, which destroyed the
# fid the dup was still using, and cat printed nothing.
check "a redirection survives the program being replaced" "hello from a V8 filesystem" \
	"$(v8run "$SH" -c 'cat < /mnt/hello' 2>&1)"
check "and the byte count of the big one"	"28000" \
	"$(v8run "$SH" -c 'wc -c < /mnt/sub/deep' 2>&1 | tr -d ' ')"

# lseek, WHICH IS THE ONE THING 9P HAS NO MESSAGE FOR -- p9.h argues that at
# length.  tail(1) is a 1985 program that seeks, so the extension is exercised
# by something that was not written for it.
# The expected value is COMPUTED FROM THE SUITE'S OWN FILE and not written
# out, which is not tidiness: the first draft transcribed a line from the
# scratch file used while developing this, and failed against big.txt's actual
# contents.  A constant copied from somewhere else is a claim about that other
# place.
check "tail seeks to the end of a mounted file"	"$(tail -1 "$FSTMP/big.txt")" \
	"$(v8run "$TAIL" -1 /mnt/sub/deep 2>&1)"

# The negative half.  A name that is not there must be ENOENT from the SERVER's
# namei, not from the Mac.
check "a missing name is ENOENT"		"1" \
	"$(v8run "$CAT" /mnt/nope >/dev/null 2>&1; echo $?)"
# THE BOUNDARY IS A CHARACTER AND NOT A LENGTH.  /mntfoo is not in the mount,
# and a prefix test on length alone would claim it -- the same trap vfs.c's
# table carries a trailing slash to avoid.
check "/mntfoo is not the mount"		"1" \
	"$(v8run "$CAT" /mntfoo >/dev/null 2>&1; echo $?)"

# READ ONLY, AND FROM TWO DIFFERENT PLACES.  The write goes to the server and
# comes back EROFS; the unlink never leaves this process, because rm(1) reaches
# a syscall that has no slot in struct v8fstyp and would otherwise have asked
# the HOST to unlink /mnt/hello.
check "a write is refused"			"1" \
	"$(v8run "$SH" -c 'echo x > /mnt/hello' >/dev/null 2>&1; echo $?)"
# ...and two more, AND WHAT REFUSES THEM HAS CHANGED TWICE UNDER THIS COMMENT.
# It used to read "two syscalls that never leave this process, because they
# have no slot in struct v8fstyp ... MOUNTED() is what makes it a refusal".
# Both have slots now -- mkdir since §8a step 5f, chmod since 5f-b -- so both
# DO leave the process, and what refuses them is the SERVER: this section's
# v8fsd runs -r, iupdat returns at iget.c:248 before it breads anything, and
# the reply is EROFS.  The cases are unchanged and still right; only the reason
# is, which is this repository's most repeated shape and the reason a comment
# naming a mechanism has to be re-read every time the mechanism moves.
check "chmod on a mount is refused"		"1" \
	"$(v8run "$ROOT/rootfs/bin/chmod" 777 /mnt/hello >/dev/null 2>&1; echo $?)"
check "and mkdir"				"1" \
	"$(v8run "$ROOT/rootfs/bin/mkdir" /mnt/newdir >/dev/null 2>&1; echo $?)"
# rm -f EXITS 0 AND THAT IS UPSTREAM'S OWN BEHAVIOUR, not the mount lying:
# rm.c:107 is `if(unlink(arg) && (fflg==0 || iflg))', so -f suppresses the
# error and errcode is never incremented.  The case is here because the
# obvious way to test the unlink refusal reads as a pass either way, and the
# next person to write it should know before spending the round.  What says
# the refusal happened is the file still being there, below.
check "rm -f says nothing, as rm.c:107 does"	"0" \
	"$(v8run "$RM" -f /mnt/hello >/dev/null 2>&1; echo $?)"
# ...and the file is still there, which is the half that says the refusal was
# not a refusal to find it.
check "the file survived all three"		"hello from a V8 filesystem" \
	"$(v8run "$CAT" /mnt/hello 2>&1)"

# THE MOUNT IS WHAT DOES THIS.  Same binary, same path, no V8MOUNT -- and the
# answer must be that there is no such file, or every case above is consistent
# with /mnt being something in the jail.
check "without V8MOUNT there is no /mnt/hello" "1" \
	"$(v8bare "$CAT" /mnt/hello >/dev/null 2>&1; echo $?)"
# ...and the jail is untouched beside it, which is the other direction: a
# mount that shadowed the static table would break /etc.
check "the jail still answers for /etc"		"0" \
	"$(v8run "$CAT" /etc/group >/dev/null 2>&1; echo $?)"

# ---------------------------------------------------------------------------
# CONTAINMENT, AND THE CASES ABOVE CANNOT SHOW IT.
#
# `chmod on a mount is refused' passes on this machine whether or not MOUNTED()
# exists, because there is no /mnt here and the host's chmod fails too -- the
# guard and the absence of the directory are indistinguishable.  That is the
# host-property trap wearing a different hat: a case that is green for a reason
# the port does not control.
#
# So mount over a directory the host really has, holding a file with different
# contents, and ask two questions the absence of /mnt cannot answer.
mkdir -p "$P9DIR/hostdir"
printf 'THE HOST FILE, NOT THE IMAGE\n' > "$P9DIR/hostdir/hello"
chmod 600 "$P9DIR/hostdir/hello"

# A MOUNT SHADOWS WHAT IS UNDER IT.  Same path, and the answer must come off
# the disk image rather than out of the directory that is really there.
check "a mount shadows the host directory"	"hello from a V8 filesystem" \
	"$(v8host "$CAT" "$P9DIR/hostdir/hello" 2>&1)"
# ...and the host file is still what it was, which is the half that says the
# read went somewhere else rather than that the file was overwritten.
check "and the host file is untouched"		"THE HOST FILE, NOT THE IMAGE" \
	"$(cat "$P9DIR/hostdir/hello")"

# NOW THE GUARD, AND §8a step 5f-b MADE IT A BETTER ONE RATHER THAN A STALE
# ONE.  It was written when chmod had no slot: without MOUNTED() the call
# reached the host with the path verbatim and changed THIS file's mode, and the
# guard was the refusal.  chmod is a slot now, so the call goes to the server
# instead -- and the property being asserted is the one that actually matters
# and always was: A MOUNTED PATH NEVER REACHES THE HOST'S chmod.  Revert the
# dispatch to `rawsys2(SYS_chmod, vpath(p), m)' and vpath leaves a mounted path
# alone, so the mode below becomes 777 and this fires exactly as before.
#
# The mount here is read-only, so nothing lands on the image either; the
# writable half of the same claim is §9f-b, where the chmod does reach the
# image and the checkers at the end of the section still call it clean.
v8host "$ROOT/rootfs/bin/chmod" 777 "$P9DIR/hostdir/hello" >/dev/null 2>&1
# substr AND NOT $1, because macOS appends `@' to the mode column for a file
# with extended attributes -- which $TMPDIR files acquire without asking.  The
# first draft compared the whole field and failed with `600@' against `600',
# which is the host-property trap arriving inside the case written to avoid it.
check "chmod through a mount does not reach the host" "-rw-------" \
	"$(ls -l "$P9DIR/hostdir/hello" | awk '{print substr($1,1,10)}')"

# ---------------------------------------------------------------------------
# WHAT AN AUDITOR FOUND, TURNED INTO CASES.  Every one of these was a measured
# defect in code written the same day; the fixes are in and these are what stop
# them coming back.

# 1. THE MOUNT PARSER.  The strip loop and the socket-path scan shared an
# index, so a trailing slash made the socket path "=csock" and two slashes
# "/=csock" -- a mount that silently does not exist, which is the failure mode
# the parser's own comment says it was written to avoid.
check "a trailing slash in V8MOUNT still connects" "hello from a V8 filesystem" \
	"$( cd "$P9DIR"; V8ROOT="$ROOT/rootfs" V8MOUNT="/mnt/=csock" \
	    export V8ROOT V8MOUNT; deadline "$CAT" /mnt/hello 2>&1 )"
check "and two of them"				"hello from a V8 filesystem" \
	"$( cd "$P9DIR"; V8ROOT="$ROOT/rootfs" V8MOUNT="/mnt//=csock" \
	    export V8ROOT V8MOUNT; deadline "$CAT" /mnt/hello 2>&1 )"

# 2. A BARE "/" IS REFUSED.  vfs.c's comment claimed mounting on the root would
# "shadow /bin and the whole world"; measured, it shadowed exactly "/" and
# nothing under it, which is an incoherent half-mount nobody described.  With
# the refusal, /bin is the jail's and there is no mount at all.
# AND THE CASE HAS TO LOOK AT `/', NOT AT `/bin' -- the first draft asserted
# that `ls /bin' still worked, which a mutation showed is true either way: the
# whole point of the finding is that a "/" mount shadows exactly "/" and
# nothing beneath it.  What distinguishes is the ROOT's own listing: refused,
# it is the jail's and contains bin; allowed, it is the server's and does not.
check "V8MOUNT=/ is refused, and / is the jail's" "1" \
	"$( cd "$P9DIR"; V8ROOT="$ROOT/rootfs" V8MOUNT="/=csock" \
	    export V8ROOT V8MOUNT; deadline "$LS" / 2>/dev/null | grep -c '^bin$' )"

# 3. chdir INTO A MOUNT WAS A JAIL ESCAPE, and the mount point is what made it
# visible: with the prefix set to a name the HOST also has, `cd' returned 0 and
# put the program in the Mac's directory.  Measured before the fix with
# V8MOUNT=/etc=sock -- pwd said /private/etc and cat read the Mac's passwd.
check "cd into a mount is refused"		"1" \
	"$(v8host "$SH" -c "cd $P9DIR/hostdir" >/dev/null 2>&1; echo $?)"
# ...and the same shell can still cd anywhere else, so the refusal is about the
# mount and not about cd.
check "and cd elsewhere still works"		"0" \
	"$(v8host "$SH" -c 'cd /bin' >/dev/null 2>&1; echo $?)"

# 4. access() MUST AGREE WITH open().  It recomputed permission from the
# image's mode bits against the HOST's uid, while the server runs Bell Labs'
# access() with u_uid 0 and takes fio.c's root bypass -- so on every file of
# every image `test -r' said no and `cat' said yes.  mkfs makes everything
# root-owned, so this was total rather than an edge case.
check "test -r and cat agree on a mounted file"	"readable" \
	"$(v8run "$SH" -c 'if test -r /mnt/hello; then echo readable; else echo NOT; fi' 2>&1)"
# ...and the write half is still refused, which is the other direction: an
# access() that just said yes to everything would pass the case above.
check "and test -w says no on a read-only mount" "notwritable" \
	"$(v8run "$SH" -c 'if test -w /mnt/hello; then echo writable; else echo notwritable; fi' 2>&1)"

# 5. A DIRECTORY DESCRIPTOR THAT LEAVES ITS PROCESS MUST NOT RETURN RAW 9P.
# The snapshot is keyed on the fd in this process, so a dup or an exec loses
# it, and p9_t_read then fell through to a real Tread -- handing the program
# 222 bytes of 9P stat records and exit 0, with the first two bytes of a
# stat's size[2] read as a d_ino.  The server now refuses a cursor read on a
# directory fid, so it is an error instead.  Passthrough fails loudly here too
# (read(2) on a host directory fd the shim does not know is -1), which is the
# parity that says this is not a new rule.
# The construct matters and the first draft's did not reach it: `ls /mnt; cat
# /mnt' is two processes each opening the directory for itself, so both build
# their own snapshot and both succeed.  What is needed is ONE descriptor
# crossing a process -- sh opens the directory, dup2s it onto 0 and execs cat,
# which then has a v8fs directory descriptor and no snapshot for it.
# THE ASSERTION IS ON THE BYTES AND NOT ON THE EXIT STATUS, because V8's cat
# is `while ((n = read(fi, buf, BUFSIZ)) > 0)' -- a read error is not > 0, so
# it ends the loop and exits 0.  The first draft of these two checked $? and
# both passed for that reason, including the passthrough control, which is what
# gave it away.  What was actually wrong before the fix is that the program
# received 222 BYTES OF 9P STAT RECORDS and could not tell; so what has to be
# asserted is that nothing comes out.
check "a directory read that lost its snapshot yields nothing" "0" \
	"$(v8run "$SH" -c 'cat < /mnt' 2>/dev/null | wc -c | tr -d ' ')"
# ...and passthrough behaves identically, which is what says this is parity
# rather than a rule invented for v8fs.
check "and passthrough does the same on /etc"	"0" \
	"$(v8run "$SH" -c 'cat < /etc' 2>/dev/null | wc -c | tr -d ' ')"

# ---------------------------------------------------------------------------
# THE PATHS NO V8 PROGRAM PERFORMS.
#
# Everything above drives the client through SHIPPED BINARIES, which is the
# right headline claim and is why those cases are written that way.  It leaves
# three things with no case at all, because nothing in this tree does them:
# fstat on a directory descriptor, lseek in all three whences, and dup sharing
# one offset.  The last is the CENTRAL claim of the design -- the connection is
# the open file description -- and it had been argued rather than tested.
#
# $(BUILD)/v8sys/p9clprobe is the shim's own sources linked into a host binary,
# the shape tests/v8sys/test.c already has, and the Makefile builds it there
# rather than here so that $(SHIM_SRC) is not spelled twice.  It runs under
# v8run, so it gets the same mount, the same jail and the same deadline every
# program above got.
CLPROBE=$BUILD/v8sys/p9clprobe
if [ ! -x "$CLPROBE" ]; then
	bad "v8fs client: p9clprobe was not built" "$CLPROBE"
else
v8run "$CLPROBE" main > "$TMP/p9clout" 2>"$TMP/p9clerr"
clrc=$?
if [ $clrc -ne 0 ]; then
	bad "p9clprobe exited nonzero ($clrc)" "$(head -3 "$TMP/p9clerr")"
else
# The whole remainder of the line, not $2: pbytes renders a read as characters
# and one of them is a single space.
cq() { awk -v k="$1" '$1 == k { sub(/^[^ ]+ /, ""); print }' "$TMP/p9clout"; }

# FIRST, THAT THE PROBE IS TALKING TO THE IMAGE THESE CASES DESCRIBE.  Every
# offset below indexes into `hello from a V8 filesystem\n'; against a different
# image they would all be plausible wrong bytes rather than one clear failure.
check "the probe reads the image these cases assume" "1" "$(cq hello-is-expected)"
check "and hello is 27 bytes"			"27"	"$(cq hello-len)"

# --- 1. A DIRECTORY HAS TWO SIZES, AND THEY MUST DISAGREE -------------------
#
# THIS IS THE GUARD THAT WAS MEASURED VACUOUS.  p9_t_fstat overrides the
# server's i_size with the length of the V7 record snapshot -- dir.c:114's rule
# in a second filesystem -- and deleting that line left the suite at 466 passed,
# 0 failed, because every existing reader loops to EOF and never looks.  What
# was missing is that nothing had ever asked BOTH questions: stat reports what
# the image charges and fstat what read(2) will produce, and the observable is
# that the two differ.
#
# THE ASSERTION IS A RATIO, NOT A CONSTANT, and the reason is a bug this found.
# p9cl.c recorded the pair as "64 bytes on the image against 768 of records";
# 768 is three records and belongs to the SUBdirectory, while 64 bytes of
# 16-byte entries is four -- 1024.  The sentence was arithmetically impossible
# and described no directory at all.  So the case checks that the same count
# comes out of both units (stat/16 == readable/256) over TWO directories of
# different sizes, which no transcribed pair can satisfy by accident.
check "a directory's stat and fstat sizes differ" "1"	"$(cq dir-two-sizes-differ)"
check "and fstat is what read(2) will produce"	"1"	"$(cq dir-fstat-is-readable-bytes)"
check "and that is a whole number of V7 records" "1"	"$(cq dir-total-is-whole-records)"
check "and both units count the same entries"	"1"	"$(cq dir-entry-counts-agree)"
check "the mount root holds four of them"	"4"	"$(cq dir-entries)"
check "reading the directory to EOF gives no error" "0"	"$(cq dir-read-err)"
# ...and the subdirectory, which is where 768 actually came from.  Three
# entries, so both numbers move and the ratio does not.
check "the subdirectory's two sizes differ too"	"1"	"$(cq subdir-two-sizes-differ)"
check "and its units agree as well"		"1"	"$(cq subdir-entry-counts-agree)"
check "and it holds three entries"		"3"	"$(cq subdir-entries)"
# The numbers themselves, so a failure above is diagnosable rather than a bare
# 0 -- and so the pair this port got wrong is written down correctly once.
check "the root charges 64 bytes on the image"	"64"	"$(cq dir-stat-size)"
check "and offers 1024 of records"		"1024"	"$(cq dir-fstat-size)"
check "the subdirectory charges 48"		"48"	"$(cq subdir-stat-size)"
check "and offers 768"				"768"	"$(cq subdir-fstat-size)"

# --- 2. lseek, ALL THREE WHENCES --------------------------------------------
#
# tail(1) reaches Tseek end to end but only ever SEEK_END with a non-positive
# offset.  Nothing sent a negative SEEK_CUR -- which is the shape v8fsd's
# do_seek says produced its one remote crash, a p9_u64 in a signed comparison --
# and nothing reached the overflow guard at all.
check "lseek SEEK_SET"				"6"	"$(cq seek-set-6)"
check "and reads what is at the offset"		"from"	"$(cq seek-set-bytes)"
check "lseek SEEK_CUR forward"			"12"	"$(cq seek-cur-fwd)"
check "and lands on the space"			"_"	"$(cq seek-cur-fwd-byte)"
# THE ARM NOTHING ELSE REACHES.  Legal lseek(2), and the one the server's
# signed/unsigned seam is about.
check "lseek SEEK_CUR backwards"		"0"	"$(cq seek-cur-back)"
check "and is back at the start"		"hello"	"$(cq seek-cur-back-bytes)"
check "lseek SEEK_END is the file's length"	"27"	"$(cq seek-end-0)"
check "and SEEK_END with a negative offset"	"26"	"$(cq seek-end-neg)"
check "which is the trailing newline"		"."	"$(cq seek-end-neg-byte)"
check "a read at end of file is 0, not an error" "0"	"$(cq seek-read-at-eof)"
# Past the end is legal -- V7 files have holes -- and reads nothing.
check "seeking past the end is allowed"		"1000"	"$(cq seek-past-end)"
check "and reading there yields nothing"	"0"	"$(cq seek-read-past-end)"
# A negative RESULT is EINVAL, which is lseek(2)'s own answer, and 22 is it.
check "a negative result is refused"		"-1"	"$(cq seek-negative)"
check "with EINVAL"				"22"	"$(cq seek-negative-errno)"
# An unknown whence never leaves the process.
check "an unknown whence is refused"		"-1"	"$(cq seek-bad-whence)"
check "with EINVAL too"				"22"	"$(cq seek-bad-whence-errno)"

# THE OVERFLOW GUARD, WHICH AN AUDITOR FOUND COMPUTING base+off BEFORE TESTING
# FOR NEGATIVE.  That is signed overflow -- undefined -- and reachable from an
# ordinary lseek: 2^62 twice.  The PAIR is what makes the case meaningful: the
# SET must succeed, because a guard that simply refused large offsets would pass
# a case that only checked the second call.
check "lseek to 2^62 succeeds"		"4611686018427387904"	"$(cq seek-huge-set)"
check "and 2^62 again from there is refused"	"-1"	"$(cq seek-huge-cur)"
check "with EINVAL, not a wrapped offset"	"22"	"$(cq seek-huge-cur-errno)"
# ...and the fid still works, which says the guard refused the operation rather
# than leaving f_off somewhere unspeakable.  do_seek assigns only after the test.
check "and the descriptor still works after"	"0"	"$(cq seek-after-refusal)"
check "reading from the start again"		"hello"	"$(cq seek-after-refusal-bytes)"

# --- 3. dup SHARES ONE OFFSET, WHICH IS THE DESIGN --------------------------
#
# p9cl.c's header is one sentence -- the connection IS the open file
# description -- and dup is the first of the three consequences it names.  It
# follows structurally from the offset living on the server, and nothing
# asserted it: a shell cannot, because two stdio readers of one descriptor each
# pull a whole block, so the second sees end of file wherever the offset is.
check "a dup is a different descriptor"		"1"	"$(cq dup-is-a-new-fd)"
check "reading six bytes"			"hello_" "$(cq dup-first-read)"
check "the dup continues where it stopped"	"from"	"$(cq dup-continues)"
check "and the original sees the dup's move"	"_a"	"$(cq dup-original-sees-it)"
check "an lseek on one is an lseek on both"	"hello"	"$(cq dup-seek-is-shared)"
# THE CLUNK BUG, STATED DIRECTLY.  A Tclunk in t_close destroys a fid every dup
# shares, which is what made `cat < /mnt/hello' print nothing; the right number
# of clunks is zero, and the kernel drops the connection at the LAST close.
check "closing one does not close the file"	"_fro"	"$(cq dup-survives-sibling-close)"
check "and dup2 shares the offset as well"	"from"	"$(cq dup2-shares-offset-too)"

# --- 4. THE ERRNOS THAT CROSS THE WIRE --------------------------------------
#
# V7's namei has two answers one line apart and so does this server
# (v8fsd.c:699), but a SHORT Rwalk carries no errno -- so the reason is lost
# unless the client reconstructs it from the last qid.  It did not, and
# `open("/mnt/hello/beyond")' reported ENOENT where a V7 kernel reports ENOTDIR.
# Found here, fixed in p9walk.
#
# THE THREE ARE A SET, and the middle one is the reason it is not two.  The
# first fails on its first name and is an Rerror carrying the server's own
# errno; the other two are SHORT WALKS taking the same code path, and they must
# come out differently -- a client that always answered ENOTDIR would pass the
# third case on its own.
check "a missing name is ENOENT"		"2"	"$(cq err-open-missing-errno)"
check "missing inside a real directory is too"	"2"	"$(cq err-open-missing-deep-errno)"
check "but through a plain file it is ENOTDIR"	"20"	"$(cq err-walk-thru-file-errno)"
# The server refuses a write at the OPEN today, with EROFS (30).  §8a step 5f is
# what changes this, and it changes the SERVER: p9_t_write already sends its
# Twrite rather than refusing locally, so this number moves without the client
# being touched.
check "opening for write is EROFS"		"30"	"$(cq err-open-for-write)"
check "and the write itself fails"		"-1"	"$(cq err-write)"
# A directory descriptor is not writable, and this one never leaves the process.
check "writing to a directory is EISDIR"	"21"	"$(cq err-write-dir-errno)"

# --- 5. A DEAD SERVER IS AN I/O ERROR, NOT A SIGNAL -------------------------
#
# FOUND BY THE SANITIZED SERVER BELOW, WHICH IS WHY IT IS HERE.  When that
# server aborts, the probe came back 141 -- 128 + 13, SIGPIPE.  The transport is
# a socket and the caller is a V7 program that has no idea it is one, so a
# server that dies mid-conversation was killing `cat' with a signal instead of
# failing its read.  On a real V8, a disk that stops answering is EIO.
#
# p9dial now sets SO_NOSIGPIPE, which is PER SOCKET and that is the point:
# signal(SIGPIPE, SIG_IGN) would change the program's own disposition, and a V8
# program in a pipeline must still die when its reader goes away -- that is how
# `yes | head' terminates.
#
# The probe produces the condition with shutdown(2) on its own descriptor rather
# than by killing a server, so there is no second process and no timing.
check "the client socket refuses SIGPIPE"	"1"	"$(cq pipe-nosigpipe-set)"
check "and the descriptor worked before"	"hello"	"$(cq pipe-read-before)"
check "a read against a dead peer fails"	"-1"	"$(cq pipe-read-after)"
check "with EIO, which is what a disk would say" "5"	"$(cq pipe-read-after-errno)"
# The real assertion: the probe reached its last line.  A process killed by
# signal 13 never prints this, and its exit status is 141.
check "and the program is still alive"		"1"	"$(cq pipe-survived)"

# --- 6. THE SAME TRAFFIC, AGAINST A SANITIZED SERVER ------------------------
#
# THIS SECTION EXISTS BECAUSE A MUTATION WOULD NOT FIRE, which by this repo's
# rule means the case is vacuous or the code is dead -- and here it is neither.
# do_seek's overflow guard tests before adding; reverting it to the auditor's
# original (add, then test for negative) leaves all 525 cases green, because
# every overflow reachable here wraps to a NEGATIVE value and the broken form
# lands on the same EINVAL.  It gets there by executing undefined behaviour,
# which licenses the compiler to delete the check -- and no behavioural test
# can see that.  It is the strncat shape: the answer was right the whole time.
#
# So the instrument is a sanitizer rather than an assertion.  Measured, with the
# guard broken: `signed integer overflow: 4611686018427387904 +
# 4611686018427387904 cannot be represented in type long long' at v8fsd.c, and
# the process dies.  With it as written: silent.
#
# THE ASSERTION IS THAT THE SERVER IS STILL ALIVE, not that stderr is empty --
# -fno-sanitize-recover=all means the first diagnostic kills it, and a dead peer
# is harder to lose than a line of stderr.  Both are checked, in that order.
UBFSD=$BUILD/v8fsd/v8fsd-ubsan
if [ ! -x "$UBFSD" ]; then
	bad "v8fs client: the sanitized server was not built" "$UBFSD"
else
rm -f "$P9DIR/ubsock"
( cd "$P9DIR" && exec "$UBFSD" -r ubsock p9img ) > "$TMP/p9ub.out" 2>&1 &
UBPID=$!
ubready=0
for _i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25; do
	grep -q "^v8fsd ready" "$TMP/p9ub.out" 2>/dev/null && { ubready=1; break; }
	kill -0 $UBPID 2>/dev/null || break
	sleep 0.2
done
check "a sanitized server comes up"		"1"	"$ubready"
if [ "$ubready" = 1 ]; then
	v8ub() { ( cd "$P9DIR"; V8ROOT="$ROOT/rootfs"; V8MOUNT="/mnt=ubsock"
		   export V8ROOT V8MOUNT; deadline "$@" ); }
	# The probe first -- it is the only thing that reaches the seek guard --
	# and then two shipped binaries, because `ls -l' is a connection per
	# entry and `cat' of the 28000-byte file is bmap's indirect arm.  Three
	# different shapes of traffic through one instrumented server.
	v8ub "$CLPROBE" main > "$TMP/p9ubout" 2>&1
	check "the probe's whole run against it"	"0"	"$?"
	v8ub "$LS" -l /mnt > /dev/null 2>&1
	v8ub "$CAT" /mnt/sub/deep > "$TMP/ubdeep" 2>/dev/null
	check "and 28000 bytes still come back"		"28000" \
		"$(wc -c < "$TMP/ubdeep" | tr -d ' ')"
	# The two halves of the verdict.
	check "the sanitized server reported no UB"	"" \
		"$(grep -c 'runtime error' "$TMP/p9ub.out" | tr -d ' ' | sed 's/^0$//')"
	check "and it is still running"			"0" \
		"$(kill -0 $UBPID 2>/dev/null; echo $?)"
fi
kill $UBPID 2>/dev/null; wait $UBPID 2>/dev/null
fi	# the sanitized server was built

fi	# the client probe ran
fi	# the client probe was built

# 6. A SECOND IMAGE, FOR TWO PROPERTIES THE FIRST ONE CANNOT EXPRESS -- and
# both cases were written against the first image, ran green, and were shown
# VACUOUS by mutation.  That is the informative outcome and it is why they are
# here instead.
#
#   A 0600 FILE.  `hello' is 0644, so the `other' bits grant read and the old
#   uid-recomputing access() agreed with open() by accident.  The disagreement
#   only shows on a file whose mode denies the caller: the server takes
#   fio.c's root bypass (u_uid is 0 and nothing sets it) and opens it anyway.
#
#   A WIDE uid.  di_uid is v8_i16 and SIGNED, so 40000 loads as -25536 and
#   v8fsd renders it "-25536"; the client's parser met a '-' and returned 0,
#   which is root.  CLAUDE.md's contract for every 16-bit narrowing here is
#   "root maps to root, and non-root never maps to root", and both halves are
#   asserted below -- a case that only checked the second would pass against a
#   parser that returned the sentinel for everything.
UIDDIR=$FSTMP/uidfs
mkdir -p "$UIDDIR"
printf 'secret\n' > "$UIDDIR/s.txt"
printf '/dev/null\n200 32\nd--777 0 0\nsecret\n---600 0 0 %s\nwide\n---644 40000 40001 %s\n$\n' \
	"$UIDDIR/s.txt" "$UIDDIR/s.txt" > "$UIDDIR/proto"
if ! ( cd "$UIDDIR" && V8ROOT=$ROOT/rootfs "$MKFS" img proto ) >"$UIDDIR/mkfs.log" 2>&1; then
	bad "v8fs client: the uid image would not build" "$(head -3 "$UIDDIR/mkfs.log")"
else
rm -f "$UIDDIR/sk"
( cd "$UIDDIR" && exec "$V8FSD" -r sk img ) > "$TMP/p9uid.out" 2>&1 &
UPID=$!
uready=0
for _i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25; do
	grep -q "^v8fsd ready" "$TMP/p9uid.out" 2>/dev/null && { uready=1; break; }
	kill -0 $UPID 2>/dev/null || break
	sleep 0.2
done
check "a server for the uid image"		"1"	"$uready"
v8uid() { ( cd "$UIDDIR"; V8ROOT="$ROOT/rootfs"; V8MOUNT="/m=sk"
	    export V8ROOT V8MOUNT; deadline "$@" ); }

check "test -r agrees with open on a 0600 file" "readable" \
	"$(v8uid "$SH" -c 'if test -r /m/secret; then echo readable; else echo NOT; fi' 2>&1)"
check "and cat really can read it"		"secret" \
	"$(v8uid "$CAT" /m/secret 2>&1)"
# The contract, both halves.  A uid V8 cannot represent must not read as root...
check "a uid V8 cannot hold does not read as root" "0" \
	"$(v8uid "$LS" -l /m/wide 2>&1 | awk '{print $3}' | grep -c '^root$')"
# ...and a uid that IS root still does.
check "and a root-owned file still does"	"1" \
	"$(v8uid "$LS" -l /m/secret 2>&1 | awk '{print $3}' | grep -c '^root$')"

# THE SAME CONTRACT AS A NUMBER RATHER THAN AS A LOGIN NAME.  `ls -l' resolves
# the uid through /etc/passwd, so what the two cases above really compare is
# "root" against some other string -- they would pass against a parser that
# returned 1, or 40000, or anything else that is not 0.  The contract is
# narrower than that: P9UID_BAD is (short)-1, i.e. 65535, a value no V8 system
# issues, so an owner this port cannot represent reads as one it cannot
# represent.  The probe asks stat(2) directly.
if [ -x "$CLPROBE" ]; then
	v8uid "$CLPROBE" uid > "$TMP/p9uidout" 2>"$TMP/p9uiderr"
	uqrc=$?
	uq() { awk -v k="$1" '$1 == k { sub(/^[^ ]+ /, ""); print }' "$TMP/p9uidout"; }
	if [ $uqrc -ne 0 ]; then
		bad "p9clprobe uid exited nonzero ($uqrc)" \
		    "$(head -3 "$TMP/p9uiderr")"
	else
	check "root maps to root, as the number 0"	"0"	"$(uq uid-root-file)"
	check "and a uid V8 cannot hold is the sentinel" "65535" "$(uq uid-wide-file)"
	check "which is not root"			"1"	"$(uq uid-wide-is-not-root)"
	# ...and the 0600 file opens, because the server takes fio.c's root
	# bypass.  This is what v8s_access was rewritten to REPORT rather than
	# to recompute, asked of open(2) itself rather than through test -r.
	check "a 0600 root-owned file opens"		"1"	"$(uq uid-0600-opens)"
	check "and its bytes come back"			"secret" "$(uq uid-0600-bytes)"
	check "and its mode is what mkfs was given"	"600"	"$(uq uid-root-mode)"
	fi
fi

kill $UPID 2>/dev/null; wait $UPID 2>/dev/null
fi	# the uid image built

# THE SERVER MUST STILL BE THERE, and it is the cheapest possible assertion
# that nothing above took it down.  Sixteen client programs have connected and
# exited by now -- each one closing a socket mid-conversation, which is the
# event SIGPIPE would have killed it on.
check "and the server survived every client"	"0" \
	"$(kill -0 $CPID 2>/dev/null; echo $?)"
kill $CPID 2>/dev/null; wait $CPID 2>/dev/null

fi	# the probe ran
fi	# the server came up
fi	# p9probe built

# --------------------------------------------------------------------------
# §8a step 5f -- THE MOUNT IS WRITABLE, and every server above this line is
# NOT.
#
# THREE SERVERS SHARE $FSTMP/p9img and each of them now runs with -r, which is
# the change that made this section safe to add.  Before 5f they shared it
# "read-only by assumption": the protocol refused Twrite, and the filesystem
# underneath was never read-only at all -- readi sets IACC, so iput ran IUPDAT
# and dirtied the disk inode on every read, and the only reason nothing reached
# the image was that nothing called bflush().  With -r the superblock's s_ronly
# is set, iupdat returns at iget.c:248 before it breads anything, and the image
# fd is O_RDONLY as well.  So the shared artefact cannot change, for two
# independent reasons, and this section gets an image of its own regardless.
#
# THE WRITE IS INVISIBLE ON A PRISTINE IMAGE, which is why this needs saying at
# all.  `time' is set once by iinit from the superblock's s_time and upstream's
# clock interrupt is not imported, so `dp->di_atime = *ta' stored the value
# mkfs had already written -- measured with an instrumented driver: the pwrite
# happens, cmp on the image prints nothing.  Perturb one di_atime first and
# exactly four bytes move.  v8fs_clock() is what makes the clock advance now.
WFSTMP=$TMP/wfs
mkdir -p "$WFSTMP"
if [ ! -x "$V8FSD" ]; then
	bad "5f: $V8FSD not built"
elif [ ! -f "$FSTMP/p9img" ]; then
	bad "5f: no pristine image to copy"
else
cp "$FSTMP/p9img" "$WFSTMP/wimg"
rm -f "$WFSTMP/wsock"
( cd "$WFSTMP" && exec "$V8FSD" wsock wimg ) > "$TMP/p9w.out" 2>&1 &
WPID=$!
wready=0
for _i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25; do
	grep -q "^v8fsd ready" "$TMP/p9w.out" 2>/dev/null && { wready=1; break; }
	kill -0 $WPID 2>/dev/null || break
	sleep 0.2
done
check "a WRITABLE server comes up"	"1"	"$wready"

if [ "$wready" = 1 ]; then
	# THE READY LINE SAYS WHICH IT IS, which is what lets the read-only
	# assertion below be about the server rather than about this script's
	# memory of how it started it.
	check "...and says so, where the -r ones do not"	"" \
		"$(sed -n 's/^v8fsd ready wsock//p' "$TMP/p9w.out")"
	check "and the read-only one says ro"	" ro" \
		"$(sed -n 's/^v8fsd ready csock//p' "$TMP/p9cli.out")"

	w8() { ( cd "$WFSTMP"; V8ROOT="$ROOT/rootfs"; V8MOUNT="/mnt=wsock"
		 export V8ROOT V8MOUNT; deadline "$@" ); }

	# 1. THE HEADLINE: sh's `>' on a name that does not exist.  Three
	# messages the server refused an hour ago -- Twalk fails, Tcreate makes
	# the inode and the directory entry through nami.c's NI_NXCREAT arm,
	# Twrite runs writei -- and one V8 program that knows about none of it.
	check "sh can create a file on a mount"		"0" \
		"$(w8 "$SH" -c 'echo brand new > /mnt/fresh' >/dev/null 2>&1; echo $?)"
	check "and cat reads back what was written"	"brand new" \
		"$(w8 "$CAT" /mnt/fresh 2>&1)"

	# 2. TRUNCATION IS Topen's OTRUNC, WHICH IS itrunc().  A second `>' to
	# the same name must not append: the size is the assertion, because a
	# server that ignored OTRUNC would still produce readable output.
	w8 "$SH" -c 'echo x > /mnt/fresh' >/dev/null 2>&1
	check "a second > truncates rather than appending"	"2" \
		"$(w8 "$CAT" /mnt/fresh 2>&1 | wc -c | tr -d ' ')"

	# 3. `>>' IS NOT O_APPEND, and that is measured rather than assumed.
	# sh/service.c:76 is `lseek(fd, 0L, 2)' after an ordinary open, so it
	# rides Tseek -- which is why p9_t_open can refuse O_APPEND outright.
	w8 "$SH" -c 'echo y >> /mnt/fresh' >/dev/null 2>&1
	check "and >> appends, through Tseek rather than O_APPEND"	"x.y" \
		"$(w8 "$CAT" /mnt/fresh 2>&1 | tr '\n' '.' | sed 's/\.$//')"

	# 3b. A 28000-BYTE COPY WITHIN THE MOUNT, AND IT IS HERE BECAUSE A
	# MUTATION WOULD NOT FIRE.  Deleting `if (atcur) f->f_off = off + n'
	# from do_write -- the line that advances the server-side cursor --
	# left the whole section green, and for none of the three usual
	# reasons: the guard is not vacuous and the code is not dead.  Every
	# write above is a SINGLE write through one open, because `echo' writes
	# once, so a cursor that never advances is never asked to.
	#
	# `cat big > copy' is seven writes of 4096 (cat.c:8's BLOCK) through
	# one descriptor, so the second one lands on top of the first the
	# moment the cursor stops moving.  It also drives bmap's ALLOCATING
	# indirect arm, since 28000 bytes is 28 blocks and only ten are direct.
	# cmp(1) is the assertion rather than a byte count: a broken cursor
	# produces a file of the right length made of the wrong blocks.
	w8 "$SH" -c 'cat /mnt/sub/deep > /mnt/copy' >/dev/null 2>&1
	check "a 28000-byte copy through one descriptor"	"   28000" \
		"$(w8 "$CAT" /mnt/copy 2>/dev/null | wc -c)"
	check "...and cmp says it is the same file"		"same" \
		"$(w8 "$ROOT/rootfs/bin/cmp" /mnt/copy /mnt/sub/deep >/dev/null 2>&1 &&
		   echo same)"
	w8 "$RM" /mnt/copy >/dev/null 2>&1

	# 4. mkdir(1) IS mknod PLUS TWO link()s, WHICH IS V7's WAY.  The links
	# are of `d/.' and `d/..', both of which kmkdir has already written --
	# so they must succeed and do nothing, exactly as they do on macOS.
	# Before the dot-link arm moved above MOUNTED(), this created the
	# directory correctly and THEN printed `cannot link /mnt/d/.'.
	check "mkdir works on a mount"			"0" \
		"$(w8 "$ROOT/rootfs/bin/mkdir" /mnt/d 2>&1 >/dev/null; echo $?)"
	check "and it is a directory"			"d" \
		"$(w8 "$LS" -l /mnt 2>/dev/null | sed -n 's/^\(d\)rwx.*  *d$/\1/p')"
	check "and a file can be made inside it"	"inner" \
		"$(w8 "$SH" -c 'echo inner > /mnt/d/f' >/dev/null 2>&1
		   w8 "$CAT" /mnt/d/f 2>&1)"

	# 5. THE REMOVE PATH, AND THE PARENT IS THE WHOLE OF IT.  A Tremove
	# carries a fid and nothing else, while V7's unlink names a DIRECTORY
	# and an ENTRY -- so the server records the directory each walk stepped
	# through.  The first version used the root for a plain file: this case
	# is the one that failed, silently, and it needs the file to be one
	# level DOWN or it passes against the bug.
	check "rm removes a file one level down"	"0" \
		"$(w8 "$RM" /mnt/d/f 2>&1 >/dev/null; echo $?)"
	check "...and it is really gone"		"" \
		"$(w8 "$LS" /mnt/d 2>/dev/null)"
	check "rmdir then works"			"0" \
		"$(w8 "$ROOT/rootfs/bin/rmdir" /mnt/d 2>&1 >/dev/null; echo $?)"

	# 6. AND rmdir REFUSES A NON-EMPTY ONE, which is rmdir(1)'s own read of
	# the directory rather than the server's -- nami.c's NI_RMDIR arm says
	# "can rmdir non-empty directory" in as many words.  The pair matters:
	# without it, a t_remove that always succeeded would pass case 5.
	w8 "$ROOT/rootfs/bin/mkdir" /mnt/d2 >/dev/null 2>&1
	w8 "$SH" -c 'echo z > /mnt/d2/keep' >/dev/null 2>&1
	check "rmdir refuses a directory with something in it"	"1" \
		"$(w8 "$ROOT/rootfs/bin/rmdir" /mnt/d2 2>/dev/null >/dev/null; echo $?)"

	# 7. access() IS ASKED NOW, NOT COMPUTED -- Taccess, p9.h's second
	# extension.  On a WRITABLE mount `test -w' must say yes, which is the
	# answer the old fixed EROFS could never give; the -r server twenty
	# lines up still says no, and having both is what makes either mean
	# anything.
	check "test -r on a writable mount"	"readable" \
		"$(w8 "$ROOT/rootfs/bin/test" -r /mnt/hello && echo readable)"
	check "test -w on a writable mount"	"writable" \
		"$(w8 "$ROOT/rootfs/bin/test" -w /mnt/hello && echo writable)"
	# AND test -x SAYS YES ON A 0644 FILE, WHICH IS V7's ANSWER AND NOT A
	# BUG.  fio.c:193 is `if(u.u_uid == 0) return(0)' with no 0111 special
	# case -- that refinement is BSD's -- and the server runs as root
	# because main.c:370-379 argues at length that it must (folding the
	# HOST's uid in would make every permission answer a property of who
	# ran the test).  So this asserts the authentic answer.
	#
	# THE OLD CODE RETURNED EACCES HERE and it was a guess that happened to
	# match a system nobody was porting.  What it was reaching for is real
	# but belongs elsewhere: nothing can be EXECUTED off a mount, because
	# v8s_execve is passthrough.  That is a fact about execve and not about
	# the file, and no live caller asks -- V8's sh searches PATH by calling
	# execve on each directory rather than by asking access().
	check "and test -x says YES, because V7 grants root everything"	"exec" \
		"$(w8 "$ROOT/rootfs/bin/test" -x /mnt/hello && echo exec)"

	# 8. THE CLOCK ADVANCED, WHICH IS THE HALF OF 5f THAT IS NOT ABOUT
	# WRITING AT ALL.  `time' is set once, by iinit from the superblock's
	# s_time, and upstream's clock interrupt (sys/clock.c, a VAX interval
	# timer) is not imported -- so before v8fs_clock() every stamp a write
	# laid down was the moment mkfs made the image, which is a plausible
	# wrong answer rather than a visibly wrong one.
	#
	# ASSERTED ON THE SUPERBLOCK, NOT ON `ls -l', and the first draft used
	# `ls -l' and could not fail: its output is minute-granular and this
	# section runs seconds after mkfs, so the new file and the image were
	# stamped `Aug 10 18:13' either way.  A test whose resolution is coarser
	# than the effect it measures is not a test.
	#
	# update() writes `fp->s_time = time' whenever s_fmod is set (alloc.c),
	# and a create sets it -- so the served image's superblock timestamp is
	# the kernel's clock, in seconds, and the pristine copy's is mkfs's.
	# The offset is tests/mkfs's SB+216 and the relation is the one that
	# suite already argues for: no calendar, just before < after.
	d4() { od -An -tu4 -j "$2" -N4 "$1" | tr -d ' \n'; }
	check "the kernel's clock is not frozen at the image's own s_time"	"1" \
		"$( t0=$(d4 "$FSTMP/p9img" $((1024+216)))
		    t1=$(d4 "$WFSTMP/wimg" $((1024+216)))
		    [ -n "$t0" ] && [ -n "$t1" ] && [ "$t1" -gt "$t0" ] && echo 1 ||
			echo "mkfs=$t0 afterwrite=$t1" )"

	# 9. AND THE READ-ONLY MOUNT REFUSES ALL OF IT, on the same binaries,
	# the same paths and A COPY OF THE SAME IMAGE, seconds apart.  This is
	# the pair -r exists for: the two servers differ by one argument.
	#
	# A SECOND SERVER RATHER THAN THE ONE UPSTAIRS, and the first draft used
	# that one and was VACUOUS.  csock is killed a hundred lines above this
	# point, so every "refused" here was really a dial that could not
	# connect -- `rm' exited 0 and `cat' said No such file, both of which
	# read like the assertions passing and neither of which was about
	# read-only anything.  The tell was `rm' exiting 0 where a refusal must
	# exit 1: a dead server and a strict one do not fail the same way.
	cp "$FSTMP/p9img" "$WFSTMP/roimg"
	rm -f "$WFSTMP/rosock"
	( cd "$WFSTMP" && exec "$V8FSD" -r rosock roimg ) > "$TMP/p9ro.out" 2>&1 &
	RPID=$!
	roready=0
	for _i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
		grep -q "^v8fsd ready" "$TMP/p9ro.out" 2>/dev/null && { roready=1; break; }
		kill -0 $RPID 2>/dev/null || break
		sleep 0.2
	done
	check "a read-only server comes up beside it"	"1"	"$roready"
	r8() { ( cd "$WFSTMP"; V8ROOT="$ROOT/rootfs"; V8MOUNT="/ro=rosock"
		 export V8ROOT V8MOUNT; deadline "$@" ); }

	check "the read-only mount refuses a create"	"1" \
		"$(r8 "$SH" -c 'echo no > /ro/nope' >/dev/null 2>&1; echo $?)"
	check "...and a mkdir"				"1" \
		"$(r8 "$ROOT/rootfs/bin/mkdir" /ro/nodir >/dev/null 2>&1; echo $?)"
	# rm's EXIT STATUS IS NOT THE INSTRUMENT HERE, and reading it was wrong
	# in the way CLAUDE.md already records for `cat' and read errors.  V7's
	# rm is `if(!fflg) if(access(arg,02)<0) { print the mode; if(!yes())
	# return; }' -- so on a file it may not write it ASKS, gets no answer
	# from a non-tty, and returns having done nothing and having set no
	# error.  Exit 0.  With -f it skips the question, the unlink fails with
	# EROFS, and `fflg' suppresses the message and the error count.  Exit 0
	# again.  Both are correct rm and neither says anything about the mount.
	# So the assertion is the file.
	check "...and an rm removes nothing"		"still here" \
		"$(r8 "$RM" -f /ro/hello >/dev/null 2>&1
		   r8 "$CAT" /ro/hello >/dev/null 2>&1 && echo "still here")"
	check "...and test -w says no where the writable one said yes"	"notwritable" \
		"$(r8 "$ROOT/rootfs/bin/test" -w /ro/hello || echo notwritable)"
	check "...and the file is still readable"	"hello from a V8 filesystem" \
		"$(r8 "$CAT" /ro/hello 2>&1)"

	# THE READ-ONLY SERVER STAYS UP PAST THIS POINT, and moving the kill
	# below §9f-b was not tidying.  Section 9f-b has containment cases of
	# its own, and against a dead server every one of them would pass for
	# the wrong reason -- which is not hypothetical here: the read-only
	# pair in §9f was written against a socket killed a hundred lines
	# earlier and was vacuous until the `rm' exiting 0 gave it away.  Both
	# servers are alive for the whole of the next section, and the
	# byte-identical cmp that used to sit here now runs after it, so it
	# covers 9f-b's writes as well.

	# =====================================================================
	# 9f-b. chmod, chown AND utime -- §8a step 5f-b, and they are ONE
	#       message.  A Twstat carries a whole stat and the server applies
	#       whichever fields are not "do not touch", so all three syscalls
	#       are one wire format with different fields filled in.
	#
	# THE READ-ONLY SERVER IS STILL UP at this point, which is why the
	# containment case below can run against it.
	# ---------------------------------------------------------------------

	# chmod, through V8's own chmod(1).  The mode is read back with ls -l
	# rather than with a stat helper, because ls -l is what a person would
	# use and it is the only reader here that goes through the whole client.
	w8 "$ROOT/rootfs/bin/chmod" 600 /mnt/hello >/dev/null 2>&1
	check "chmod reaches the image"			"-rw-------" \
		"$(w8 "$LS" -l /mnt/hello 2>/dev/null | awk '{print $1}')"
	w8 "$ROOT/rootfs/bin/chmod" 644 /mnt/hello >/dev/null 2>&1
	check "...and back again"			"-rw-r--r--" \
		"$(w8 "$LS" -l /mnt/hello 2>/dev/null | awk '{print $1}')"

	# (A case asserting that chmod cannot change the type of a PLAIN FILE
	# stood here and was vacuous twice over -- mutation is what said so.
	# ls switches on st_mode & S_IFMT pre-set to `-' at ls.c:336 and NOT overwritten by the switch at :354, which has no default arm,
	# so a mode that lost its type bits entirely still prints as a plain
	# file; and p9tostat REBUILDS the type from DMDIR rather than passing
	# the server's IFMT through, so ls on a mount could not see the
	# server's mode word even if it wanted to.  The directory case below is
	# the one that can fail.)

	# chown, AND ITS CONSUMER IS mkdir(1) RATHER THAN chown(1) -- which is
	# not ported.  mkdir.c:69 is `chown(d, getuid(), getgid())', UNCHECKED,
	# straight after the mknod, so before this step every directory made on
	# a mount was silently left owned by root and nothing said so.
	#
	# THE ASSERTION IS A RELATION, NOT A NUMBER: `id -u' is a property of
	# whoever runs this, so the case compares the owner ls reports against
	# the owner the shell asks for.  A hardcoded 501 is the host-property
	# trap that has broken this repo's CI twice.
	w8 "$ROOT/rootfs/bin/mkdir" /mnt/owned >/dev/null 2>&1
	check "mkdir's chown reaches the image"		"$(id -un)" \
		"$(w8 "$LS" -ld /mnt/owned 2>/dev/null | awk '{print $3}')"
	# ...and the control that says the case above is about the chown and
	# not about mkdir: hello was made by mkfs and nothing has chowned it.
	check "...and a file nothing chowned is still root"	"root" \
		"$(w8 "$LS" -l /mnt/hello 2>/dev/null | awk '{print $3}')"

	# CHMOD A DIRECTORY, WHICH IS WHERE THE FILE TYPE IS OBSERVABLE.  The
	# server's arm is `ip->i_mode = (ip->i_mode & IFMT) | nm', and dropping
	# that IFMT is invisible on a plain file for two independent reasons
	# (see the note above) -- but statof sets DMDIR from
	# `(ip->i_mode & IFMT) == IFDIR', so a directory whose type bits were
	# overwritten stops being reported as one and ls prints `-'.  Measured
	# by mutation: without the IFMT the checkers at the end of this section
	# go red, and so does this.
	w8 "$ROOT/rootfs/bin/chmod" 700 /mnt/owned >/dev/null 2>&1
	check "chmod on a directory keeps it a directory"	"drwx------" \
		"$(w8 "$LS" -ld /mnt/owned 2>/dev/null | awk '{print $1}')"

	# AN OWNER THE SERVER CANNOT PARSE, which is the one guard in this step
	# that NO V8 PROGRAM CAN REACH.  9P specifies the field as a name and
	# this server takes a number; p9_t_chown formats an int, so it is
	# incapable of sending "nobody" -- only a foreign client, or this probe,
	# can ask the question.  It was atoi(), which has no error return, so
	# every unparseable owner became uid 0 WITH A SUCCESS REPLY, and 0 is
	# the identity fio.c:193 lets bypass every permission check.
	( cd "$WFSTMP" && perl -e 'alarm 30; exec @ARGV' "$P9PROBE" wsock /dev/null -w ) \
		> "$TMP/p9w.probe" 2>"$TMP/p9w.probeerr"
	wq() { awk -v k="$1" '$1 == k {print $2}' "$TMP/p9w.probe"; }
	check "the write probe ran"			"0"	"$(wq w-uid-before)"
	check "an owner that is a NAME is refused"	"EINVAL" "$(wq w-uid-name)"
	# ...and refused BEFORE assigning.  A wstat that rejected on the way out
	# would satisfy the case above and still have changed the file.
	check "...and the owner is untouched"		"0"	"$(wq w-uid-name-unchanged)"
	check "so is one that is all punctuation"	"EINVAL" "$(wq w-uid-dashes)"
	check "and one with a digit and then rubbish"	"EINVAL" "$(wq w-uid-trailing)"
	check "...and that one changed nothing either"	"0"	"$(wq w-uid-trailing-unchanged)"
	# THE POSITIVE HALF, without which the guard is indistinguishable from
	# a server that refuses every wstat.
	check "a plain number is still accepted"	"1"	"$(wq w-uid-number)"
	check "...and lands in the inode"		"7"	"$(wq w-uid-number-took)"
	# A NEGATIVE IS A NUMBER THIS SERVER ITSELF EMITS -- statof renders
	# i_uid with "%d" of a SIGNED short -- so refusing every '-' would mean
	# refusing to take back a value it had just handed out.
	check "and so is a negative one"		"1"	"$(wq w-uid-negative)"
	check "...which round-trips as itself"		"-3"	"$(wq w-uid-negative-took)"
	check "and the owner restores to root"		"0"	"$(wq w-uid-restored)"
	# An all-ones wstat is 9P's "sync me" and must not be an error.
	check "a wstat that touches nothing succeeds"	"1"	"$(wq w-sync)"
	check "and the server is still there"		"0"	"$(wq w-alive)"

	# utime, AND mv(1) IS WHY IT EXISTS.  On a mount link(2) has no slot
	# and is refused, so mv ALWAYS falls through to fork, /bin/cp and then
	# mv.c:129's `utime(target, &s1.st_atime)'.  Without the slot the copy
	# and the unlink both succeed and only the timestamps are lost --
	# silently, because mv does not check utime either.
	#
	# 1991 IS THE INSTRUMENT, and that is deliberate.  ls -l prints minutes
	# for a recent file and a YEAR for an old one, so a same-minute
	# comparison could not tell a working utime from a broken one -- the
	# resolution trap this suite already learned from the s_time case.  A
	# 1991 date is a difference ls can actually show.
	echo "aged content" > "$WFSTMP/aged"
	touch -t 199107150000 "$WFSTMP/aged"
	w8 "$ROOT/rootfs/bin/mv" "$WFSTMP/aged" /mnt/aged >/dev/null 2>&1
	check "mv carries a 1991 mtime onto the mount"	"Jul 15 1991" \
		"$(w8 "$LS" -l /mnt/aged 2>/dev/null | awk '{print $6, $7, $8}')"
	# AND THE atime WITH IT, which is the half a mtime-only server would
	# have passed the case above without.  do_wstat honoured s_mtime alone
	# until 5f-b and said why: "nothing in this world sets atime alone".
	# True, and it never covered mv, which sets BOTH -- utime(2) takes a
	# time_t[2] and mv hands it &st.st_atime.  THIS case is the one the
	# s_atime arm exists for; ORDER MATTERS, because the cat below moves it.
	check "...and the atime with it"		"Jul 15 1991" \
		"$(w8 "$LS" -lu /mnt/aged 2>/dev/null | awk '{print $6, $7, $8}')"
	# ...and it is a MOVE, so the bytes have to be there too.  A utime that
	# worked on an empty file would satisfy the cases above alone.
	check "...and the bytes came with it"		"aged content" \
		"$(w8 "$CAT" /mnt/aged 2>&1)"
	check "...and the source is gone"		"gone" \
		"$([ -e "$WFSTMP/aged" ] || echo gone)"
	# THAT cat MOVED THE atime, and asserting so is free here because the
	# file is the only one in the suite whose atime was a known constant a
	# moment ago.  readi sets IACC (rdwri.c:50) so iput runs IUPDAT on every
	# READ -- the finding §8a step 5f had to make three measurements to see,
	# because a frozen clock and a delayed write hid it.  The assertion is a
	# RELATION (it is no longer the value we put there) rather than today's
	# date, which is a property of the machine and not of the port.
	check "...and reading it moved the atime, as V7 says it must"	"moved" \
		"$(w8 "$LS" -lu /mnt/aged 2>/dev/null |
		   awk '{ t = $6 " " $7 " " $8
		          print (t == "Jul 15 1991") ? "frozen" : "moved" }')"

	# CONTAINMENT, and the read-only mount is the sharpest form of it: all
	# three of these are writes, and s_ronly makes iupdat return before it
	# breads anything, so not even a mode bit can move.
	check "chmod on a read-only mount fails"	"1" \
		"$(r8 "$ROOT/rootfs/bin/chmod" 700 /ro/hello >/dev/null 2>&1; echo $?)"
	check "...and the mode is unchanged"		"-rw-r--r--" \
		"$(r8 "$LS" -l /ro/hello 2>/dev/null | awk '{print $1}')"

	sleep 0.3
	kill $RPID 2>/dev/null; wait $RPID 2>/dev/null
	# NOT ONE BYTE, and this is the guarantee the EROFS arm never gave: a
	# read through a read-only mount cannot move an atime either, because
	# s_ronly makes iupdat return before it breads the inode.  It now also
	# covers the refused chmod two cases up -- a wstat that failed but had
	# already dirtied an inode would show here and nowhere else.
	check "and the read-only image is byte-identical to what it was given"	"same" \
		"$(cmp -s "$FSTMP/p9img" "$WFSTMP/roimg" && echo same || echo differs)"

	# THE ROUND TRIP CLOSES: everything this section made is removed again,
	# so the writable image must come back to the block count it started
	# with.  A leaked block passes fsck -- icheck calls it `missing' and
	# says 0 either way once it is off the free list -- so the exact
	# equality is the only thing that can see one.
	w8 "$RM" /mnt/aged >/dev/null 2>&1
	w8 "$ROOT/rootfs/bin/rmdir" /mnt/owned >/dev/null 2>&1
	w8 "$RM" /mnt/fresh >/dev/null 2>&1
	w8 "$RM" /mnt/d2/keep >/dev/null 2>&1
	w8 "$ROOT/rootfs/bin/rmdir" /mnt/d2 >/dev/null 2>&1
	check "and the mount is empty again"	"hello sub" \
		"$(w8 "$LS" /mnt 2>/dev/null | tr '\n' ' ' | sed 's/ $//')"

	sleep 0.3
	kill $WPID 2>/dev/null; wait $WPID 2>/dev/null

	# 10. THE INDEPENDENT CHECKERS, WHICH ARE THE ONLY THING THAT CAN SEE A
	# WHOLE CLASS.  §8a step 5d's lesson: a probe's writer and reader share
	# one program's beliefs, so a duplicate-block bug is invisible from
	# inside by construction -- when alloc() was mutated to hand out a block
	# twice, every case stayed green and only icheck, fsck and cmp went red.
	# These three know nothing about 9P.
	#
	# BOUNDED CAPTURES, because a corrupted free list once made `fsck -y'
	# print for its whole deadline and bash died on the substitution.
	check "icheck finds nothing missing after all that"	"missing    0" \
		"$( cd "$WFSTMP" && fsdeadline "$ROOT/rootfs/etc/icheck" wimg 2>&1 |
		    head -200 | grep '^missing' )"
	check "dcheck says nothing at all"			"wimg:" \
		"$( cd "$WFSTMP" && fsdeadline "$ROOT/rootfs/etc/dcheck" wimg 2>&1 |
		    head -200 )"
	check "and fsck reports nothing modified"		"" \
		"$( cd "$WFSTMP" && fsdeadline "$ROOT/rootfs/etc/fsck" -y wimg </dev/null 2>&1 |
		    head -200 | grep -i 'MODIFIED' )"

	# 11. AND THE ACCOUNTING CLOSES EXACTLY.  Create, write, truncate,
	# append, mkdir, a file one level down, three removes and an rmdir --
	# and then the same number of blocks in use as the image started with.
	#
	# A FIRST DRAFT ASSERTED `u0 + 4' AND WAS A GUESS DRESSED AS A
	# MEASUREMENT: it happened to be off by one (35 against 36) and had no
	# way to say which of the two numbers was wrong.  Removing everything
	# first turns it into an identity, which is the shape fsprobe's own
	# round-trip case already uses.
	check "and the block count is exactly what it was"	"1" \
		"$( u0=$( cd "$FSTMP" && fsdeadline "$ROOT/rootfs/etc/icheck" p9img 2>&1 |
			  head -200 | awk '/^used/{print $2}')
		    u1=$( cd "$WFSTMP" && fsdeadline "$ROOT/rootfs/etc/icheck" wimg 2>&1 |
			  head -200 | awk '/^used/{print $2}')
		    [ -n "$u0" ] && [ "$u0" = "$u1" ] && echo 1 ||
			echo "before=$u0 after=$u1" )"
fi
fi	# the writable server section

# ===========================================================================
# 11. WHAT HAPPENS WHEN THE DISK FILLS UP, which before this was: the server
#     DIED, and took every other client's connection with it.
#
# Its own image and its own server, because the whole point is to exhaust the
# free list and every case above depends on wimg's accounting being exact.
# 200 blocks is enough to reach nospace in about a second.
#
# WHY IT WAS UNREACHABLE UNTIL NOW.  Bell Labs' out-of-space path is a kludge
# they label one in capitals (alloc.c:187-195): print a message, sleep five
# clock ticks in the hope another process frees a block, then ENOSPC.  This
# port maps sleep() onto tsleep(), which PANICS when there is no device below
# and no timeout -- correctly, for the streams that were its only caller when
# it was written.  alloc.c:194 is a second caller with a different answer:
# `lbolt' is woken in exactly one place in the whole kernel (clock.c:290, the
# clock interrupt) and this port has neither the interrupt nor the file, so
# the sleep can never wake, and the wait is futile anyway because the caller
# is the only thing running.  Before §8a step 5f nothing could write, so
# alloc() could never reach nospace at all.
FILLTMP=$TMP/fill
mkdir -p "$FILLTMP"
printf 'x\n' > "$FILLTMP/seed"
printf '/dev/null\n200 32\nd--777 0 0\nhello\n---644 0 0 %s\n$\n' \
	"$FILLTMP/seed" > "$FILLTMP/proto"
if ! ( cd "$FILLTMP" && V8ROOT=$ROOT/rootfs "$MKFS" img proto ) \
     >"$FILLTMP/mkfs.log" 2>&1; then
	bad "fill: mkfs failed" "$(head -3 "$FILLTMP/mkfs.log")"
else
rm -f "$FILLTMP/sock"
( cd "$FILLTMP" && exec "$V8FSD" sock img ) > "$TMP/fill.out" 2>&1 &
FPID=$!
fready=0
for _i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25; do
	grep -q "^v8fsd ready" "$TMP/fill.out" 2>/dev/null && { fready=1; break; }
	kill -0 $FPID 2>/dev/null || break
	sleep 0.2
done
if [ "$fready" != 1 ]; then
	bad "fill: the server did not come up" "$(head -3 "$TMP/fill.out")"
else
f8() { ( cd "$FILLTMP"; V8ROOT="$ROOT/rootfs"; V8MOUNT="/mnt=sock"
	 export V8ROOT V8MOUNT; deadline "$@" ); }

awk 'BEGIN{for(i=0;i<20000;i++) printf "line %05d padding padding padding\n", i}' \
	> "$FILLTMP/big"
f8 "$SH" -c 'cat big > /mnt/fill' >/dev/null 2>&1
check "a write that fills the image fails"	"1" \
	"$(f8 "$SH" -c 'cat big > /mnt/fill' >/dev/null 2>&1; echo $?)"
# THE LOUD ONE.  Measured before the fix: `panic: tsleep: no device below, and
# no timeout', server exit 2, and every OTHER client's connection dropped
# mid-transaction -- so this is availability rather than a wrong answer, and
# reachable by an unprivileged program.
check "and the server is still alive"		"alive" \
	"$(kill -0 $FPID 2>/dev/null && echo alive || echo dead)"
# ...and alive is not the same as working.  A server that survived the write
# but lost its buffer cache would pass the case above.
check "...and still answers a read"		"x" \
	"$(f8 "$CAT" /mnt/hello 2>&1)"
# AND THE SECOND DEFECT, WHICH THE FIRST ONE HID.  kmkdir calls writei for the
# `.' and `..' entries and never looked at u.u_error, and do_create tests only
# for a null inode -- so on a full image the server answered SUCCESS for a
# directory fsck calls damaged (parent link count bumped for a `..' that was
# never written; the directory itself SIZE=0).  Upstream's mkdir() ignores the
# same return, and can afford to: it IS the system call, so u_error reaches the
# user.  Here it died in the wrapper.
check "mkdir on a full image reports failure"	"1" \
	"$(f8 "$ROOT/rootfs/bin/mkdir" /mnt/d >/dev/null 2>&1; echo $?)"

sleep 0.3
kill $FPID 2>/dev/null; wait $FPID 2>/dev/null
# The image is NOT clean here and must not be asserted so: namei's NI_MKDIR arm
# had already made the entry before writei failed, which is exactly the state a
# V7 kernel leaves and what fsck exists to repair.  What IS asserted is that
# icheck can still read it -- a server that died mid-write could have left a
# superblock nothing can parse.
check "and the image is still readable by icheck"	"1" \
	"$( cd "$FILLTMP" && fsdeadline "$ROOT/rootfs/etc/icheck" img 2>&1 |
	    head -200 | grep -c '^img' )"
fi
fi	# the fill section

fi	# mkfs succeeded
fi	# mkfs exists

echo "streams: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
