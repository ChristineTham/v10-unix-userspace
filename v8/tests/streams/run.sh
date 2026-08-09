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

# -DKERNEL IS NOT OPTIONAL FOR THIS PROBE and fsprobe.c says why at length:
# inode.h and buf.h declare namei, iget, bread and geteblk -- all
# pointer-returning -- inside #ifdef KERNEL, and KFLAGS carries
# -Wno-implicit-function-declaration for the imported half's sake.  Without the
# flag every one of those calls is an implicit int and the returned pointer is
# TRUNCATED to 32 bits, which is this port's ps -T bug exactly.  fsprobe.c has
# an #error so the flag cannot be dropped silently.
if ! clang $KFLAGS -DKERNEL -fcommon -o "$TMP/fsprobe" \
     "$ROOT/tests/streams/fsprobe.c" "$KERN" "$SETJMP" \
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

fi	# mkfs succeeded
fi	# mkfs exists

echo "streams: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
