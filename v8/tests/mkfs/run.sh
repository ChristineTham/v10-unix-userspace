#!/bin/sh
# mkfs(8), and the on-disk formats it writes -- PLAN.md section 8a step 4.
#
# THIS SUITE EXISTS BECAUSE MKFS IS THE FIRST PROGRAM HERE WHOSE OUTPUT HAS AN
# OTHER END THAT IS NOT THIS PORT.  Everything before it answers to macOS or to
# another of our own components, so a struct only has to agree with itself; a
# filesystem image has to agree with 1985, and with a VAX if the SIMH
# cross-check in step 6 ever runs.  Four structures were silently wrong before
# this step -- dinode 80 instead of 64, filsys 7960 instead of 4096, fblk 1432
# instead of 716, NINDIR 128 instead of 256 -- because daddr_t, time_t and off_t
# are all `long' and V8's VAX compiler defined NOLONG.
#
# So there are two kinds of case here, and they fail differently:
#
#   * THE WIDTHS, asked of v8cc through the real headers.  A _Static_assert
#     would see one compiler; these structures have three readers.
#   * THE IMAGE BYTES.  Every width above can be right and the image still
#     wrong, because two of the three bugs this suite guards were in code
#     rather than in a header -- gmode() reading its own parameters at the
#     wrong stride, and ltol3() walking an array at the wrong one.  Only the
#     bytes catch those, and only the bytes catch a forgotten -DDIRSIZ=14.

ROOT=$(cd "$(dirname "$0")/../.." && pwd)		# the release tree, v8/
BUILD=$ROOT/build/stage0
V8ROOT=$ROOT/rootfs
export V8ROOT
MKFS=$V8ROOT/etc/mkfs
CC=$V8ROOT/bin/cc
TMP=${TMPDIR:-/tmp}/mkfs.$$
mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT

pass=0 fail=0
check() {
	if [ "$2" = "$3" ]; then pass=$((pass+1))
	else fail=$((fail+1)); echo "FAIL $1"; echo "  want [$2]"; echo "  got  [$3]"; fi
}
bad() { fail=$((fail+1)); echo "FAIL $1"; shift; [ $# -gt 0 ] && echo "    $*"; }

for f in "$MKFS" "$CC"; do
	[ -x "$f" ] || { echo "missing $f -- run make"; exit 1; }
done

# ---------------------------------------------------------------------------
# 1. The widths, from the V8 side.
# ---------------------------------------------------------------------------
if ! "$CC" -o "$TMP/probe" "$ROOT/tests/mkfs/probe.c" > "$TMP/cc.log" 2>&1; then
	echo "probe build failed:"; head -5 "$TMP/cc.log"; exit 1
fi
"$TMP/probe" > "$TMP/w" 2>&1 || bad "probe exited nonzero"
w() { awk -v k="$1" '$1==k {$1=""; sub(/^ /,""); print}' "$TMP/w"; }

# The VAX values.  Not preferences: sizeof(dinode) is 64 because param.h
# hardcodes INOPB 16 for a 1024-byte block, and the free-list superblock's
# 964 used bytes have to fit in one.
# THE TYPEDEFS THE RECORD STRUCTS ARE SPELLED WITH, and this case is the whole
# reason the spelling is worth anything.  `v8_i32 di_size' only says something
# about the disk if v8_i32 is really four bytes -- it is `int' underneath, and
# therefore four bytes ONLY because this port is LP64.  V8's ccom has exactly
# four integer types and no `long long', so char/short/int/long must cover
# 8/16/32/64 and that assignment IS LP64; measured, by building the tree with
# `int' at 64 bits, where 32 becomes unspellable and every field below would
# have to become char[4] with hand-packing.  The day the model moves, this line
# goes red first and the fields to move are the ones named v8_*.
check "the fixed-width typedefs are 2 2 4 4"	"2 2 4 4"	"$(w fixed)"

check "daddr_t is four bytes"		"4"	"$(w daddr)"
check "struct dinode is 64"		"64"	"$(w dinode)"
check "struct filsys is 4096"		"4096"	"$(w filsys)"
check "struct fblk is 716"		"716"	"$(w fblk)"
check "NINDIR(0) is 256"		"256"	"$(w nindir)"

# THE SELF-CONSISTENCY PAIR, which is what makes the four above more than
# transcribed constants.  Both of these were FALSE before step 4 and neither
# produced a diagnostic: INOPB said sixteen inodes per block while sizeof said
# twelve and a half, so itod()/itoo() would have put inode 17 at byte 1280 of a
# 1024-byte block; and NMASK said an indirect block holds 256 addresses while
# NINDIR computed 128, one line below it in the same file.
check "INOPB agrees with sizeof(dinode)"  "$(w inopb)"  "$(w inopbcalc)"
check "NINDIR agrees with NMASK"	  "$(w nindir)" "$(w nmaskplus)"
check "NINDIR agrees with NSHIFT"	  "$(w nindir)" "$(( 1 << $(w nshift) ))"

# ltol3/l3tol, the 3-byte packing.  Their strides have to match daddr_t's width
# and they were made to disagree with it once already -- l3tol has no caller in
# this port, so nothing but this case will ever notice.
check "ltol3/l3tol round trip"	"1193046 1 16777215 0"	"$(w l3back)"
check "ltol3 keeps the low three bytes, little end first" "86 52 18" "$(w l3bytes)"

# THE TAPE RECORD, which is the same argument one layer out.  struct spcl is a
# WIRE format: dumptape.c writes each record from `char tblock[NTREC][BSIZE(0)]'
# as exactly 1024 bytes, so what reaches the tape is the first 1024 of the
# struct -- the header plus 924 of c_addr -- and widening a header field does
# not grow the record, it SLIDES every field after it and drops the tail.
#
# The struct already had one VAX-shaped half before this port touched it, which
# is what makes leaving c_date and c_ddate at eight bytes worse than either
# choice: c_dinode is a struct dinode, fixed at 64 by step 4a.  The offsets
# below are asserted as literals BECAUSE they are the format -- there is no
# other expression of them in the tree to compare against, unlike INOPB and
# NMASK, which is exactly the situation section 3 handles by reading bytes off
# a real image.
check "sizeof(struct spcl) is the VAX's 1124"	"1124"	"$(w spcl)"
check "and its header sits at the VAX's offsets" \
    "0 4 8 12 16 20 24 28 32 96 100"	"$(w spcloff)"
# c_dinode has to land where a dinode fits whole inside the 1024 that go out.
spo=$(w spcloff)
check "c_addr starts after a whole dinode" \
    "$(( $(echo $spo | cut -d' ' -f9) + $(w dinode) + 4 ))" \
    "$(echo $spo | cut -d' ' -f11)"
# restor's checksum() walks BSIZE(0)/sizeof(int) ints, and dump writes a word
# to make the total come out.  It cannot see a layout change -- both ends are
# ours -- but it CAN see sizeof(int) drifting, which would break the tape
# against a real V8 silently.
check "the checksum walks 256 ints, so the record is 1024 bytes" "256" \
    "$(w spclwords)"

# ---------------------------------------------------------------------------
# 2. DIRSIZ, both ways.
#
# The port raises DIRSIZ 14 -> 254 for host filenames, and mkfs is compiled
# -DDIRSIZ=14 because what it writes is an image.  Both halves are asserted:
# the override has to WORK (it did not until sys/param.h got the #ifndef its
# own comment claimed it had) and the default has to stay 254 (or every
# directory the shim serves truncates at fourteen characters).
# ---------------------------------------------------------------------------
"$CC" -DDIRSIZ=14 -o "$TMP/probe14" "$ROOT/tests/mkfs/probe.c" >"$TMP/cc14.log" 2>&1 &&
    "$TMP/probe14" > "$TMP/w14" 2>&1 || bad "probe -DDIRSIZ=14 failed"
w14() { awk -v k="$1" '$1==k {$1=""; sub(/^ /,""); print}' "$TMP/w14"; }
check "the default DIRSIZ is still 254"		"254"	"$(w dirsiz)"
check "a default struct direct is 256 bytes"	"256"	"$(w direct)"
check "-DDIRSIZ=14 reaches sys/param.h"		"14"	"$(w14 dirsiz)"
check "and makes struct direct 16 bytes"	"16"	"$(w14 direct)"
# cpp must not merely warn and carry on, which is exactly what it did before.
if grep -q 'DIRSIZ redefined' "$TMP/cc14.log"; then
	bad "cpp still reports DIRSIZ redefined -- the #ifndef in param.h is gone"
else
	pass=$((pass+1))
fi

# ---------------------------------------------------------------------------
# 3. The image, numeric mode.  `mkfs image 2000' -- every number below follows
# from that one and from upstream's arithmetic, so nothing here is a sample of
# what this machine happened to produce.
# ---------------------------------------------------------------------------
IMG=$TMP/fs.img
out=$("$MKFS" "$IMG" 2000 2>&1); st=$?
check "mkfs exits 0"		"0"		"$st"
check "mkfs reports its ilist"	"isize = 1280"	"$(echo "$out" | sed -n 1p)"
check "mkfs reports m/n"	"m/n = 3 1000"	"$(echo "$out" | sed -n 2p)"

# Reading the image.  -v on every od, because without it od collapses repeated
# lines to a single `*' -- which is exactly what a run of zero disk addresses
# looks like, so the case that checks for them would have passed on the literal
# string "*" and failed on the real answer.
u2() { od -An -v -tu2 -j "$2" -N 2 "$1" | tr -d ' '; }
d2() { od -An -v -td2 -j "$2" -N 2 "$1" | tr -d ' '; }
d4() { od -An -v -td4 -j "$2" -N 4 "$1" | tr -d ' '; }
bytes() { od -An -v -tu1 -j "$2" -N "$3" "$1" | tr -s ' \n' ' ' | sed 's/^ //;s/ $//'; }
# a 3-byte little-endian disk address -- what ltol3 packs into di_addr
l3at() {
	_b=$(bytes "$1" "$2" 3)
	echo $(( $(echo "$_b" | cut -d' ' -f1) \
	       + $(echo "$_b" | cut -d' ' -f2) * 256 \
	       + $(echo "$_b" | cut -d' ' -f3) * 65536 ))
}
# a NUL-padded on-disk name, with the padding dropped
dname() { dd if="$1" bs=1 skip="$2" count="$3" 2>/dev/null | tr -d '\000'; }

# The superblock is block 1 (param.h: SUPERB), and the offsets are struct
# filsys laid out at VAX widths -- s_fsize at 4 rather than 2 because ALINT is
# 32 on the VAX too, so a four-byte field aligns.
SB=1024
check "s_isize is 80 inode blocks + 2"	"82"	"$(u2 "$IMG" $((SB+0)))"
check "s_fsize is the size asked for"	"2000"	"$(d4 "$IMG" $((SB+4)))"
check "s_tinode is 1280 less the two used"  "1278" "$(u2 "$IMG" $((SB+224)))"
# data blocks are 82..1999, less the one the root directory took
check "s_tfree counts the data blocks"	"1917"	"$(d4 "$IMG" $((SB+220)))"
check "s_m is the default 3"		"3"	"$(d2 "$IMG" $((SB+226)))"
check "s_n is the default 1000"		"1000"	"$(d2 "$IMG" $((SB+228)))"
# bflist() lays the free list out with stride m, counting DOWN from the end.
check "the free list is interleaved by m" "497 494 491" \
    "$(od -An -td4 -j $((SB+252)) -N 12 "$IMG" | tr -s ' ' | sed 's/^ //;s/ $//')"

# The root inode is number 2 (param.h: ROOTINO).  itod/itoo, spelled out:
#	block = (2 + 2*INOPB - 1) / INOPB	= 33/16 = 2
#	slot  = (2 + 2*INOPB - 1) % INOPB	= 33%16 = 1
# Derived here rather than written as 2112 so that a change to INOPB or to
# sizeof(dinode) moves the test with the format instead of past it.
INOPB=$(w inopb); DINODE=$(w dinode); BSZ=$(w bsize)
I2=$(( ((2 + 2*INOPB - 1) / INOPB) * BSZ + ((2 + 2*INOPB - 1) % INOPB) * DINODE ))
check "the root inode lands where itod/itoo say" "2112" "$I2"

# s_time is the only field whose value depends on when this ran, so it is the
# one place this suite could accidentally assert a property of the machine.  It
# is NOT compared to a date, in either direction: `> 2025' fails on a host whose
# clock is wrong and `= date +%s' fails on a slow one.  What is asserted is the
# invariant -- mkfs calls time() once and stamps every inode AND the superblock
# from that single value -- and then, in section 4, the RELATION between two
# images this suite made itself.  Neither half mentions a calendar.
NUMTIME=$(d4 "$IMG" $((SB+216)))
check "the superblock and the root inode share one timestamp" \
    "$NUMTIME" "$(d4 "$IMG" $((I2+52)))"

# 040777 octal = 16895.  THE GMODE GUARD: upstream's gmode() returns
# (&m0)[i], which walks its own parameters at a four-byte stride, and v8cc's
# argument slots are eight.  'd' is index 3, so the root inode came out with
# file type 0 and mkfs died in iput with `bad mode 777' on every single run.
check "the root inode is a directory, mode 0777" "16895" "$(u2 "$IMG" $((I2+0)))"
check "the root inode has . and .. linked"	 "2"	 "$(d2 "$IMG" $((I2+2)))"
check "the root directory is two 16-byte records" "32" "$(d4 "$IMG" $((I2+8)))"

# THE LTOL3 GUARD, and it is the strongest form available: di_addr is 40 bytes
# holding thirteen 3-byte addresses, the root has exactly one block, so twelve
# of them must be zero.  With the wrong stride the first seven were every
# second real address and the last six were read past the end of a stack
# `struct inode' -- one of them said block 6035408 of a 1935-block image.
b0=$(l3at "$IMG" $((I2+12)))
if [ "$b0" -ge 82 ] && [ "$b0" -lt 2000 ]; then pass=$((pass+1))
else bad "the root's first block is $b0, outside the data area 82..1999"; fi
# 37 bytes: di_addr is 40 and the first three are the address checked above.
rest=$(bytes "$IMG" $((I2+15)) 37 | tr -d ' 0')
check "the root's other twelve addresses are zero" "" "$rest"

# The directory records themselves.  THE DIRSIZ GUARD, and the reason it is on
# the bytes rather than on the compiler line: `..' at 16 is a V8 filesystem and
# `..' at 256 is not, and a forgotten -D looks identical everywhere else.
DB=$(( b0 * BSZ ))
check "the first record is inode 2"		"2"	"$(u2 "$IMG" $((DB+0)))"
check "and is named ."				"."	"$(dname "$IMG" $((DB+2)) 14)"
check "the second record is at offset 16"	"2"	"$(u2 "$IMG" $((DB+16)))"
check "and is named .."				".."	"$(dname "$IMG" $((DB+18)) 14)"

# ---------------------------------------------------------------------------
# 4. The image, prototype mode -- the path with the buffer that overflowed.
#
# `string' is 50 bytes upstream and holds host pathnames, and in the linked
# binary it is 56 bytes below `utime': a 57-character content path rewrote the
# timestamp every inode is stamped with, so the image came out wrong only in
# its dates.  The path below is built here rather than taken from $TMPDIR,
# because a runner's temporary directory is short and the case has to be long
# on every machine.
# ---------------------------------------------------------------------------
deep=$TMP/aaaaaaaaaa/bbbbbbbbbb/cccccccccc/dddddddddd/eeeeeeeeee/ffffffffff
mkdir -p "$deep" || bad "could not build a deep path"
echo 'hello from a deeply buried file' > "$deep/payload"
[ ${#deep} -gt 57 ] || bad "the deep path is only ${#deep} chars -- too short to test"

PROTO=$TMP/proto
printf '/dev/null\n2000 1280\nd--777 0 0\nhello\n---644 0 0 %s\n$\n' \
    "$deep/payload" > "$PROTO"
PIMG=$TMP/p.img
out=$("$MKFS" "$PIMG" "$PROTO" 2>&1); st=$?
check "mkfs accepts a prototype"	"0"		"$st"
check "and reports m/n from it"		"m/n = 3 1000"	"$(echo "$out" | sed -n 1p)"

# Upstream's proto path says s_isize = n+3 where the numeric path says n+2.
# Asserted because it is a real difference between the two and looks like an
# off-by-one if it is ever met without warning.
check "the proto path adds three, not two"	"83"	"$(u2 "$PIMG" $((SB+0)))"

# Root has three entries now: . .. hello -- 48 bytes of 16-byte records.
check "the proto root has three records"	"48"	"$(d4 "$PIMG" $((I2+8)))"
PDB=$(( $(l3at "$PIMG" $((I2+12))) * BSZ ))
check "the third record is inode 3"	"3"	"$(u2 "$PIMG" $((PDB+32)))"
check "at offset 32, and named hello"	"hello"	"$(dname "$PIMG" $((PDB+34)) 14)"

# Inode 3 is `hello'.  0100644 octal = 33188.  This is gmode's OTHER arm --
# '-' at index 0 was the one index that worked before the fix, so a run that
# only made a directory could not tell the two apart.
I3=$(( ((3 + 2*INOPB - 1) / INOPB) * BSZ + ((3 + 2*INOPB - 1) % INOPB) * DINODE ))
check "hello is a plain file, mode 0644"	"33188"	"$(u2 "$PIMG" $((I3+0)))"
check "hello is 32 bytes"			"32"	"$(d4 "$PIMG" $((I3+8)))"

# THE string[] GUARD, and it is a relation rather than a date.  Both images were
# made by the same binary seconds apart, so their timestamps must agree to
# within the runtime of this script.  The numeric-mode one CANNOT be corrupted
# -- its prototype is the compiled-in "d--777 0 0 $ ", whose longest token is
# six characters -- so it is a clean reference produced on this host, by this
# build, just now.  With upstream's char string[50] the proto run's timestamp is
# ASCII taken from the 195-character pathname instead: measured at 795046515,
# which is 1995-03-12, and thirty-one years is not within ten minutes of
# anything.  A host with a wrong clock moves both numbers together and the case
# still says exactly what it means to say.
pt=$(d4 "$PIMG" $((I3+52)))
delta=$(( pt - NUMTIME )); [ "$delta" -lt 0 ] && delta=$(( -delta ))
if [ "$delta" -lt 600 ]; then pass=$((pass+1))
else bad "the proto run's timestamp is $delta s from the numeric run's" \
	 "($pt vs $NUMTIME) -- a ${#deep} character path overflowed string[]"; fi

# And the contents actually arrived, which is the only case here that reads a
# data block mkfs copied rather than computed.
HDB=$(( $(l3at "$PIMG" $((I3+12))) * BSZ ))
check "hello's data block holds the file" "hello from a deeply buried file" \
    "$(dd if="$PIMG" bs=1 skip=$HDB count=31 2>/dev/null)"

# ---------------------------------------------------------------------------
# 5. The checkers, which is the point of having them.
#
# Everything above asks whether the image matches what THIS PORT believes a V8
# filesystem is -- and the belief and the bytes come from the same place, so a
# shared misunderstanding would satisfy both.  icheck and dcheck are 1985 code
# that reads the image without knowing anything about mkfs: icheck walks the
# free list block by block and the ilist inode by inode, dcheck walks every
# directory and counts the references.  They are also l3tol's first callers in
# this port -- ltol3 writes those 3-byte addresses and until now nothing read
# one back.
#
# THE CORRUPTION CASES ARE THE IMPORTANT HALF.  A checker that approves of a
# good image proves very little; the three below prove it can tell.
# ---------------------------------------------------------------------------
ICHECK=$V8ROOT/etc/icheck
DCHECK=$V8ROOT/etc/dcheck
for f in "$ICHECK" "$DCHECK"; do
	[ -x "$f" ] || { echo "missing $f -- run make"; exit 1; }
done
sq() { tr -s ' \t' ' ' | sed 's/^ *//;s/ *$//'; }
ic() { "$ICHECK" "$1" 2>&1 | sq; }

# dcheck runs UNDER A DEADLINE, and not as flake insurance.  Measured: on a
# superblock claiming more than 65535 inodes it does not terminate -- `ino' is
# 16-bit and `nfiles' is unsigned, so the loop's only exit condition can never
# be true.  6.7 million lines of `read error' in twenty seconds, on a
# 110,000-block filesystem.  That is upstream's, and faithful, and it is a HANG:
# run inline it takes the suite down and prints nothing, which is the failure
# mode tests/wavec's alarm exists for.  Every image this suite hands dcheck is
# one it built, so nothing should ever reach the deadline; that is the point.
#
# A FIRED DEADLINE MUST SAY SO, and `perl -e alarm; exec' cannot.  A killed
# child leaves TRUNCATED OUTPUT, and truncated output is compared as an ANSWER
# -- so a timeout here would read as "dcheck disagreed about a link count" and
# accuse exactly the wrong code.  That is not hypothetical given the hang above:
# it would be six million `read error' lines cut off mid-flight, or none.
#
# Prompted by one unreproduced occurrence of that case failing in a full run
# (once in five; dcheck takes 0.45-0.99s against a 20s limit, so a plain timeout
# needs a 20x slowdown).  Re-measured since, and the number is worth keeping:
# 300 hammered dcheck invocations, then **120 consecutive runs of this entire
# suite -- 18240 cases -- with zero failures**.
#
# So it is NOT INTRINSIC TO THIS SUITE, which is a result rather than a shrug:
# every surviving hypothesis has to live in the full-run context the suite
# cannot reproduce alone.  Two candidates, and the big one is now dead too.
#
# WHAT THE SIXTEEN EARLIER SUITES LEAVE IN rootfs/ -- the third shape of the
# host-property trap, which has already bitten tests/jail once.  Measured by
# hashing every regular file under rootfs/ either side of a full `make test':
# exactly ONE differs, /dev/kmem, which libkmemu regenerates and which no image
# tool opens.  And all ten $(IMGBIN) binaries are byte-identical to their
# build objects, so Admin/Mk has never clobbered one -- the mechanism worth
# fearing, since Mk compiles a bare cmd/*.c with no -DDIRSIZ=14 and a 254 dcheck
# reports a link-count disagreement on a PERFECTLY GOOD image.  It cannot
# happen: no $(IMGBIN) source is staged in usr/src/cmd, and tests/jail asserts
# $(IMGBIN) and $(V8BIN) stay disjoint.  Hypothesis closed.
#
# WHAT THEY LEAVE IN THE PAGE CACHE.  dcheck.c:108 is a sync(2), the only call
# in this suite that can wait on data sixteen suites just dirtied.  Measured:
# ~800MB written and left dirty immediately before each of twelve dcheck runs
# gives 0.05-0.26s and a clean answer every time -- no excursion at all, let
# alone the 20x one a 20s deadline needs.  Dead as well.  (And note that the
# 0.45-0.99s quoted above is the HARNESS: perl's exec plus the sq pipeline.
# dcheck itself is a fifth of that, which is worth knowing before anyone tunes
# the limit down.)
#
# So all three are closed, and what is left is not a property of this suite.
# The one occurrence sits between two commits, 18:16 and 18:35, in a session
# on record for editing source under a running `make test' TWICE -- and an edit
# reaching gencode.c mid-run rebuilds all ten $(IMGBIN) tools from a
# half-edited compiler before test-mkfs runs.  The captured log cannot rule
# that out, because it was filtered through a grep for `passed', `failed' and
# FAIL, which discards every compile line: THE ABSENCE OF BUILD OUTPUT IN A
# FILTERED LOG IS NOT EVIDENCE THAT NOTHING WAS BUILT, and reading it as such
# is what kept this open one round longer.  Not proven, but it is the only
# surviving explanation and it names its own preventive -- do not edit source
# while a suite is running.  The marker below stays, because a next occurrence
# has to name itself rather than be reasoned about a fourth time.
#
# 142 is 128+SIGALRM.  A program exiting 142 itself would be indistinguishable
# -- the shell cannot tell a signal from a status, which crash-probe.sh learned
# expensively -- but the marker is ADDITIVE, so a false positive costs one line
# and never hides real output.  The cap is the other half: a `got [...]' of six
# million lines is not a diagnosis either.
deadline() {
	_lim=$1; shift
	perl -e 'alarm shift; exec @ARGV' "$_lim" "$@" > "$TMP/.dl" 2>&1
	_s=$?
	head -200 "$TMP/.dl"
	[ "$(wc -l < "$TMP/.dl" | tr -d ' ')" -gt 200 ] &&
		echo "(output truncated at 200 lines)"
	[ "$_s" -eq 142 ] && echo "DEADLINE: did not finish within ${_lim}s"
	return 0
}
dc() { deadline 20 "$DCHECK" "$1" | sq; }

out=$(ic "$IMG")
# inode 1 is bflist()'s bad-block holder (IFREG), inode 2 the root (IFDIR).
check "icheck counts two files, one of each kind" "files 2 (r=1,d=1,b=0,c=0,l=0)" \
    "$(printf '%s\n' "$out" | grep '^files')"
check "icheck finds one used block, none indirect" "used 1 (i=0,ii=0,iii=0,d=1)" \
    "$(printf '%s\n' "$out" | grep '^used')"
check "and nothing unaccounted for"		"missing 0" \
    "$(printf '%s\n' "$out" | grep '^missing')"

# THE RELATION, and it is the one worth having.  icheck's `free' is walked out
# of the free list; the superblock's s_tfree is a counter mkfs maintained while
# writing.  Two independent computations, and they have to agree -- and used +
# free + the ilist has to be the whole volume.  If NICFREE, the fblk layout or
# daddr_t's width were wrong by one, the walk would end early and this would not
# balance.  Asserted as arithmetic rather than as 1917, so it still means
# something at another size.
icfree=$(printf '%s\n' "$out" | grep '^free' | cut -d' ' -f2)
icused=$(printf '%s\n' "$out" | grep '^used' | cut -d' ' -f2)
check "icheck's walked free count matches s_tfree" \
    "$(d4 "$IMG" $((SB+220)))" "$icfree"
check "used + free + ilist is the whole volume" \
    "$(d4 "$IMG" $((SB+4)))" "$(( icused + icfree + $(u2 "$IMG" $((SB+0))) ))"

# dcheck prints one row per inode whose link count disagrees with the number of
# directory entries naming it.  A clean filesystem gets the filename and nothing.
check "dcheck finds no link-count disagreement" "$IMG:" "$(dc "$IMG")"

# The proto image has one more file and one more block.
pout=$(ic "$PIMG")
check "icheck counts hello too"		"files 3 (r=2,d=1,b=0,c=0,l=0)" \
    "$(printf '%s\n' "$pout" | grep '^files')"
check "and its data block"		"used 2 (i=0,ii=0,iii=0,d=2)" \
    "$(printf '%s\n' "$pout" | grep '^used')"

# --- and now the three corruptions ------------------------------------------
# Note what is NOT asserted: the exit status.  V8's checkers report on stdout
# and return 0 whatever they find, which is upstream's behaviour and is why
# every case here reads the output.  A future change that started exiting
# nonzero would be a deviation and should be recorded as one.

# A. zero the root's only block address.  di_addr[0] is three bytes at I2+12.
# The block is then referenced by nothing and is not in the free list either.
cp "$IMG" "$TMP/c1.img"
printf '\0\0\0' | dd of="$TMP/c1.img" bs=1 seek=$((I2+12)) count=3 conv=notrunc 2>/dev/null
c1=$(ic "$TMP/c1.img")
check "an orphaned block is missing"	"missing 1" \
    "$(printf '%s\n' "$c1" | grep '^missing')"
check "and is no longer counted as used" "used 0 (i=0,ii=0,iii=0,d=0)" \
    "$(printf '%s\n' "$c1" | grep '^used')"

# B. root nlink 2 -> 3, at I2+2.  THE PAIR: dcheck sees it and icheck does not,
# because a link count is not a block.  Both halves are asserted, since a
# checker that answered everything would be the more suspicious result.
cp "$IMG" "$TMP/c2.img"
printf '\003' | dd of="$TMP/c2.img" bs=1 seek=$((I2+2)) count=1 conv=notrunc 2>/dev/null
check "dcheck reports 2 entries against 3 links" "2 2 3" \
    "$(dc "$TMP/c2.img" | sed -n 3p)"
check "icheck is silent about a link count"	"missing 0" \
    "$(ic "$TMP/c2.img" | grep '^missing')"

# C. point the root at block 0x7fffff, past the end of a 2000-block volume.
cp "$IMG" "$TMP/c3.img"
printf '\377\377\177' | dd of="$TMP/c3.img" bs=1 seek=$((I2+12)) count=3 conv=notrunc 2>/dev/null
c3=$(ic "$TMP/c3.img")
check "an out-of-range address is named, with its inode" \
    "8388607 bad; inode=2, class=data (small)" \
    "$(printf '%s\n' "$c3" | grep 'bad;')"
check "and the real block is orphaned by it"	"missing 1" \
    "$(printf '%s\n' "$c3" | grep '^missing')"

# --- clri, which says the same thing in V8's own words ----------------------
#
# The three cases above patch bytes at computed offsets.  That is precise and it
# ties them to the layout; clri(8) states the INTENT -- clear this inode -- and
# it is a V8 program doing the damage rather than dd.  It is also the second
# writer of images in this port, after mkfs.
#
# What makes it worth a case rather than a demonstration is that the two
# checkers see DIFFERENT HALVES of one act.  clri zeroes the inode and does not
# touch the directory entry naming it -- which is what clri(8) warns about, and
# why the man page says to run fsck afterwards -- so:
#
#	icheck	the inode is gone and its data block is now orphaned
#	dcheck	one directory entry still names an inode with zero links
#
# Neither could report the other's half, and a single checker would have made
# the filesystem look half-repaired.
CIMG=$TMP/clri.img
cp "$PIMG" "$CIMG"
cbefore=$(ic "$CIMG")
check "before clri, three files and nothing missing" "files 3 (r=2,d=1,b=0,c=0,l=0) missing 0" \
    "$(printf '%s\n' "$cbefore" | grep -E '^files|^missing' | tr '\n' ' ' | sq)"
cout=$("$V8ROOT/etc/clri" "$CIMG" 3 2>&1 | sq)
check "clri names the inode it clears"	"clearing 3"	"$cout"
cafter=$(ic "$CIMG")
check "icheck: the file is gone"	"files 2 (r=1,d=1,b=0,c=0,l=0)" \
    "$(printf '%s\n' "$cafter" | grep '^files')"
check "icheck: and its block is orphaned"	"missing 1" \
    "$(printf '%s\n' "$cafter" | grep '^missing')"
check "dcheck: one entry still names it, at zero links"	"3 1 0" \
    "$(dc "$CIMG" | sed -n 3p)"

# --- icheck -s, the only thing here that WRITES an image --------------------
# It rebuilds the free list from scratch, so the image has to still check clean
# afterwards and the superblock has to still describe it.  Two reasons to have
# this beyond exercising the write path.
#
# s_time is `int' here -- narrowed with the rest of the disk record -- while
# time_t is eight bytes, so upstream's `time(&sblock.s_time)' wrote four bytes
# past the field, onto s_tfree at offset 220.  It was invisible because the high
# half of a current time_t is zero AND s_tfree is assigned zero two lines later;
# both are accidents, one expiring in 2106 and the other on any reordering.
# icheck.c takes a time_t and assigns down now, and this case is what notices if
# the two statements ever swap.
#
# And -s is the reason the O_RDWR at icheck.c:124 matters: it opens for writing
# before it knows the file is a filesystem, and a V7 superblock has no magic.
cp "$IMG" "$TMP/s.img"
"$ICHECK" -s "$TMP/s.img" >/dev/null 2>&1
sout=$(ic "$TMP/s.img")
check "after icheck -s the image still checks clean" "missing 0" \
    "$(printf '%s\n' "$sout" | grep '^missing')"
check "and the rebuilt free list still balances" "$(d4 "$IMG" $((SB+4)))" \
    "$(( $(printf '%s\n' "$sout" | grep '^used' | cut -d' ' -f2) \
       + $(printf '%s\n' "$sout" | grep '^free' | cut -d' ' -f2) \
       + $(u2 "$TMP/s.img" $((SB+0))) ))"
# s_tfree is written LAST by makefree(), so a stray eight-byte store onto it
# from the s_time above would survive into the image.
check "s_tfree survived the s_time write" \
    "$(printf '%s\n' "$sout" | grep '^free' | cut -d' ' -f2)" \
    "$(d4 "$TMP/s.img" $((SB+220)))"

# ---------------------------------------------------------------------------
# 6. The indirect block, which nothing above reaches.
#
# Every image before this one reports `i=0' -- two files, one data block, all
# thirteen addresses fitting in the inode.  That leaves the riskiest structure
# in the format untested, and it is the one where daddr_t's width is most
# directly load-bearing: an inode's di_addr[] is THREE-byte packed and goes
# through ltol3, but an indirect block is a raw daddr_t array, 1024 bytes of
# them, and NINDIR(0) is BSIZE/sizeof(daddr_t).
#
# Note what would NOT have caught a wrong width here, because it is the shape
# this whole suite is built against: mkfs writes that array with sizeof(daddr_t)
# and icheck reads it with sizeof(daddr_t), so at eight bytes they would hold
# 128 entries and agree with each other perfectly.  Only a VAX would disagree --
# or the hardcoded NMASK(0) 0377 and NSHIFT(0) 8 in param.h, which is exactly
# why section 1 asserts those against NINDIR rather than trusting either.
#
# So the case is read end to end: follow an address out of the indirect block
# and check the block it names holds the bytes it should.
# ---------------------------------------------------------------------------
# 20 blocks of 1024, each stamped with its own index so a misread address names
# itself.  LADDR is 10 (mkfs.c), so ten go in the inode and ten spill.
awk 'BEGIN { pad = sprintf("%1015s", "")
             for (i = 0; i < 20; i++) printf "block-%-2d%s\n", i, pad }' > "$TMP/big.dat"
check "the test file is exactly 20 blocks" "20480" "$(wc -c < "$TMP/big.dat" | tr -d ' ')"
printf '/dev/null\n2000 1280\nd--777 0 0\nbig\n---644 0 0 %s\n$\n' \
    "$TMP/big.dat" > "$TMP/proto.big"
BIMG=$TMP/big.img
"$MKFS" "$BIMG" "$TMP/proto.big" >/dev/null 2>&1 ||
	bad "mkfs refused the indirect-block prototype"

bout=$(ic "$BIMG")
# 21 data blocks (20 for the file, 1 for the root) and one indirect.
check "icheck sees one indirect block"	"used 22 (i=1,ii=0,iii=0,d=21)" \
    "$(printf '%s\n' "$bout" | grep '^used')"
check "and still nothing missing"	"missing 0" \
    "$(printf '%s\n' "$bout" | grep '^missing')"
bfree=$(printf '%s\n' "$bout" | grep '^free' | cut -d' ' -f2)
check "the bigger image balances too" "$(d4 "$BIMG" $((SB+4)))" \
    "$(( 22 + bfree + $(u2 "$BIMG" $((SB+0))) ))"

# di_addr[LADDR] is the indirect block: offset 12 into the dinode, three bytes
# per address, so 12 + 10*3.
IND=$(l3at "$BIMG" $((I3+12+30)))
if [ "$IND" -ge 82 ] && [ "$IND" -lt 2000 ]; then pass=$((pass+1))
else bad "the indirect block is $IND, outside the data area"; fi
# and the two addresses past it are unused
check "di_addr[11] and [12] are zero" "0 0" \
    "$(l3at "$BIMG" $((I3+12+33))) $(l3at "$BIMG" $((I3+12+36)))"

# THE END-TO-END CASE.  Entry k of the indirect block is block k+LADDR of the
# file, and each block says which one it is.  A four-byte stride puts entry 5 at
# byte 20; an eight-byte one would read byte 40 and land in entry 10's slot.
for k in 0 5 9; do
	blk=$(d4 "$BIMG" $(( IND * BSZ + k * 4 )))
	check "indirect[$k] reaches the file's block $(( k + 10 ))" "block-$(( k + 10 ))" \
	    "$(dd if="$BIMG" bs=1 skip=$(( blk * BSZ )) count=8 2>/dev/null | tr -d ' ')"
done
# mkfs shifts the tail down by LADDR and zeroes what it vacated, so only the
# first ten entries are in use.  246 addresses of four bytes each.
tail=$(bytes "$BIMG" $(( IND * BSZ + 40 )) 984 | tr -d ' 0')
check "the rest of the indirect block is zero" "" "$tail"

# ---------------------------------------------------------------------------
# 7. fsck, which is the first of these that REPAIRS.
#
# Everything above reports.  icheck says `missing 1' and dcheck prints a row;
# neither changes anything, so every case so far could be satisfied by a program
# that reads correctly and writes nothing.  fsck closes that: each corruption
# already in this suite is handed to it, and THE OTHER PROGRAM is asked whether
# the repair happened.  That is the property worth having -- fsck agreeing with
# itself would prove only that it is self-consistent.
#
# It also runs UNDER A DEADLINE, and for a measured reason rather than as flake
# insurance.  fsck's pinode() prints an inode's mtime, and until this step it
# did that by handing a four-byte di_mtime to ctime(), which dereferences eight:
# di_ctime became the high half, and gmtime()'s year loop counted towards 2.3e11
# one year at a time.  A LIVE LOCK WITH NO OUTPUT, because nothing flushes.
# pinode() runs only on a damaged filesystem, so every clean-image case passed
# throughout.  The deadline is the assertion.
# ---------------------------------------------------------------------------
FSCK=$V8ROOT/etc/fsck
[ -x "$FSCK" ] || { echo "missing $FSCK -- run make"; exit 1; }
# -y so no case can block on stdin, </dev/null so a prompt that ignored -y
# would fail rather than wait, and a deadline for the hang above.
fs() { deadline 25 "$FSCK" -y "$1" </dev/null | sq; }

# --- a clean image: agree with icheck, and change nothing --------------------
cleanout=$(fs "$IMG")
# icheck's `files' counts allocated inodes and `used' counts blocks; fsck's
# n_files and n_blks are the same two quantities, arrived at by a different
# walk.  Named separately because reusing one for both is exactly the mistake
# that makes a case look like it is comparing two programs when it is not.
icfiles=$(printf '%s\n' "$out" | grep '^files' | cut -d' ' -f2)
check "fsck's summary is icheck's three numbers" \
    "$icfiles files $icused blocks $icfree free" \
    "$(printf '%s\n' "$cleanout" | grep 'files.*blocks.*free')"

# THE STRONGEST CASE HERE, and the cheapest.  A checker that repairs is a
# program that can destroy a filesystem it was asked to inspect, and the first
# thing to know about it is that it does nothing to a filesystem that is well.
cp "$IMG" "$TMP/clean.img"
fs "$TMP/clean.img" >/dev/null
if cmp -s "$IMG" "$TMP/clean.img"; then pass=$((pass+1))
else bad "fsck modified a clean image"; fi
check "and says so" "" \
    "$(printf '%s\n' "$cleanout" | grep 'FILE SYSTEM WAS MODIFIED')"

# A COMMAND-LINE ARGUMENT MUST NOT BE ABLE TO MAKE FSCK WRITE.  `-t <path>'
# names a scratch file and upstream copies it into scrfile[80] with an
# unbounded `while(*p++ = **argv)'.  Measured with the bound removed: a
# 2000-character path runs off the end into lfname, checklist, big and lncntp,
# and fsck then reports "***** FILE SYSTEM WAS MODIFIED *****" on a filesystem
# that was well -- so the damage is not a crash but a WRITE, in the one program
# here that writes to filesystems.  The case pairs a 2000-char argument with
# the byte-identical check above; either half alone would miss it, because the
# overflow neither crashes nor changes what fsck prints about the image.
cp "$IMG" "$TMP/t.img"
longt=/tmp/$(awk 'BEGIN { for (i = 0; i < 200; i++) printf "0123456789" }')
fs "$TMP/t.img" >/dev/null 2>&1   # warm the same code path without -t
perl -e 'alarm 25; exec @ARGV' "$FSCK" -y -t "$longt" "$TMP/t.img" \
    >/dev/null 2>&1 </dev/null
if cmp -s "$IMG" "$TMP/t.img"; then pass=$((pass+1))
else bad "a 2000-char -t argument made fsck modify a clean image"; fi

# --- A, the orphaned block: icheck reported it, fsck returns it --------------
cp "$IMG" "$TMP/f1.img"
printf '\0\0\0' | dd of="$TMP/f1.img" bs=1 seek=$((I2+12)) count=3 conv=notrunc 2>/dev/null
f1=$(fs "$TMP/f1.img")
check "fsck counts the block icheck called missing" "1 BLK(S) MISSING" \
    "$(printf '%s\n' "$f1" | grep 'BLK(S) MISSING')"
f1ic=$(ic "$TMP/f1.img")
check "and afterwards nothing is missing"	"missing 0" \
    "$(printf '%s\n' "$f1ic" | grep '^missing')"
check "the freed block is back in the list"	"$((icfree + 1))" \
    "$(printf '%s\n' "$f1ic" | grep '^free' | cut -d' ' -f2)"

# mklost+found(8) is a shell script that pre-creates 256 empty slots so fsck can
# reconnect an orphan without extending a directory.  It needs a MOUNTED
# filesystem, so it waits for step 5 -- and this is what its absence costs, in
# fsck's own words.  Asserted rather than left as a known gap: when step 5 lands
# and lost+found exists, this case goes red and says which sentence to rewrite.
check "with no lost+found, fsck can only clear" "SORRY. NO lost+found DIRECTORY" \
    "$(printf '%s\n' "$f1" | grep 'lost+found')"

# --- B, the link count: dcheck reported it, fsck adjusts it ------------------
# dcheck prints `2 2 3' -- inode 2, two entries, three links.  fsck says the
# same thing in words, and then makes it two.
cp "$IMG" "$TMP/f2.img"
printf '\003' | dd of="$TMP/f2.img" bs=1 seek=$((I2+2)) count=1 conv=notrunc 2>/dev/null
f2=$(fs "$TMP/f2.img")
check "fsck names the same disagreement dcheck saw" "COUNT 3 SHOULD BE 2" \
    "$(printf '%s\n' "$f2" | grep -o 'COUNT 3 SHOULD BE 2')"
check "and the link count is two again"		"2" \
    "$(d2 "$TMP/f2.img" $((I2+2)))"
check "dcheck now finds nothing"	"$TMP/f2.img:"	"$(dc "$TMP/f2.img")"

# THE ctime GUARD.  This is the case the hang would fail, and it fails it twice
# over: the deadline catches the live lock, and the string catches a wrong time.
# The relation is between the IMAGE BYTES and fsck's stdout -- di_mtime at
# offset 56, formatted by perl's ctime(3) rather than by V8's, so the two
# implementations check each other.  Nothing here depends on the host clock.
#
# The `| sq' is load-bearing and not tidying: ctime(3) pads the day of the
# month to two columns, so the first nine days of any month carry a double
# space that fs() has already squeezed out of fsck's side.  Without it this
# case passes for three weeks in four.
f2mt=$(d4 "$IMG" $((I2+56)))
want=$(perl -e 'my $s = scalar localtime($ARGV[0]);
                print substr($s,4,12), " ", substr($s,20,4)' "$f2mt" | sq)
check "the MTIME fsck prints is the inode's own" "MTIME=$want" \
    "$(printf '%s\n' "$f2" | grep -o "MTIME=$want")"

# --- C, the out-of-range address: both name the same block -------------------
cp "$IMG" "$TMP/f3.img"
printf '\377\377\177' | dd of="$TMP/f3.img" bs=1 seek=$((I2+12)) count=3 conv=notrunc 2>/dev/null
check "fsck names the block icheck called bad, with its inode" "8388607 BAD I=2" \
    "$(fs "$TMP/f3.img" | grep -o '8388607 BAD I=2')"

# --- clri: ONE ACT, TWO HALVES, AND FSCK FIXES BOTH IN ONE RUN ---------------
# Section 5 established that clri leaves damage neither checker can describe
# alone: icheck sees an orphaned block, dcheck sees a directory entry naming a
# cleared inode.  $CIMG is that image.  fsck repairs the pair, and each half is
# confirmed by the checker that could see it.
cbfree=$(printf '%s\n' "$cafter" | grep '^free' | cut -d' ' -f2)
f4=$(fs "$CIMG")
check "fsck removes the entry clri left, by name" "NAME=/hello" \
    "$(printf '%s\n' "$f4" | grep -o 'NAME=/hello')"
check "dcheck's half: no entry names a cleared inode"	"$CIMG:"  "$(dc "$CIMG")"
f4ic=$(ic "$CIMG")
check "icheck's half: the orphaned block is back"	"missing 0" \
    "$(printf '%s\n' "$f4ic" | grep '^missing')"
check "and it is exactly one block"		"$((cbfree + 1))" \
    "$(printf '%s\n' "$f4ic" | grep '^free' | cut -d' ' -f2)"

# THE s_time GUARD, and it is stricter than icheck's because fsck WRITES the
# superblock.  Upstream's `time(&superblk.s_time)' put four bytes onto s_tfree
# at offset 220 and then called sbdirty(), so a repaired filesystem would come
# back claiming zero free blocks -- while icheck, which walks the list rather
# than reading the counter, went on saying 1916.  The relation is between the
# two.
#
# IT HAS TO BE READ HERE, BEFORE THE CONVERGENCE CASE BELOW, and that is the
# non-obvious part.  Measured with the fix reverted: pass 1 leaves s_tfree 0,
# and pass 2 puts it back.  dfile.mod is set by bwrite(), and the superblock is
# not written until ckfini() -- after this test -- so a pass whose only change
# is the superblock never reaches the time() call at all.  The damage lands
# only on a pass that also writes a data block, and the following pass repairs
# it silently.  Written after the second fsck, this case passed with the bug in.
check "s_tfree survived fsck's s_time write" \
    "$(printf '%s\n' "$f4ic" | grep '^free' | cut -d' ' -f2)" \
    "$(d4 "$CIMG" $((SB+220)))"

# CONVERGENCE.  A repair that has to be run twice has not finished, and this is
# the case that notices a fix which merely moves the damage -- including, as it
# turned out, the s_time write above.
check "a second pass finds nothing to do" "" \
    "$(fs "$CIMG" | grep 'FILE SYSTEM WAS MODIFIED')"

# ---------------------------------------------------------------------------
# 8. What forgetting -DDIRSIZ=14 actually costs, built rather than argued.
#
# The Makefile names an $(IMGBIN) group so the flag cannot be forgotten, and
# that comment used to say the cost was "a healthy filesystem reported as
# corrupt".  Measured, the dangerous direction is the OTHER one, and it is the
# direction Bell Labs' own build description produces.
#
# Admin/Mk is upstream's build for the half of cmd/ with no makefile, and for a
# bare *.c it runs `cc $CFLAGS -o $B $B.c' -- no -D of any kind.  That is
# correct on a machine whose param.h says DIRSIZ 14.  Ours says 254, because
# host directories need it.  So this section compiles mkfs the way Mk would and
# asks what comes out.
#
# What comes out is an image with 256-byte directory records -- and ALL THREE
# CHECKERS PRONOUNCE IT CLEAN.  Not by luck in the loose sense: the root holds
# only `.' and `..', and the 240 bytes between them are zero, which is V7's own
# encoding for a deleted entry, so a 16-byte-record reader skips fifteen empty
# slots and finds `..' exactly where the 254 writer put it.  icheck never looks
# at directory contents at all; dcheck and fsck count two entries and two links
# and agree.  The mirror image of the accident CLAUDE.md already records in the
# other direction.
#
# So the byte-level cases in section 3 are not belt and braces.  They are the
# only thing between this port and a silently wrong image, which is why the
# rule is to assert the format on the BYTES and never on the compiler line.
# ---------------------------------------------------------------------------
if "$CC" -Od2 -o "$TMP/mkfs-nodirsiz" "$ROOT/src/cmd/mkfs.c" \
   > "$TMP/nd.log" 2>&1; then
	pass=$((pass+1))
else
	bad "mkfs would not build the way Admin/Mk builds it" "$(head -3 "$TMP/nd.log")"
fi
(cd "$TMP" && ./mkfs-nodirsiz nd.img 2000 >/dev/null 2>&1)
NDB=$(( $(l3at "$TMP/nd.img" $((I2+12))) * BSZ ))
# 512 = two 256-byte records; 32 = two 16-byte ones.  The assertion is against
# the OTHER image rather than against 512, so it still means something if the
# geometry changes.
check "without the flag the root is 16x too big" \
    "$(( $(d4 "$IMG" $((I2+8))) * 16 ))" "$(d4 "$TMP/nd.img" $((I2+8)))"
check "and '..' lands at 256, not 16"	"2 .."	\
    "$(u2 "$TMP/nd.img" $((NDB+256))) $(dname "$TMP/nd.img" $((NDB+258)) 14)"
check "with offset 16 left empty"	"0"	"$(u2 "$TMP/nd.img" $((NDB+16)))"
# And the part that makes the byte cases load-bearing: nothing else notices.
# These three record what the checkers DO, not what they should do, so that the
# three above are visibly the only guard.  If one of them ever goes red because
# a checker got stricter, that is a good change and this is where to delete the
# case -- but read it first, because the more likely cause is that the image
# stopped being written in the 254 format and the mkfs-nodirsiz build is stale.
check "icheck cannot notice: it never reads a directory"	"missing 0" \
    "$(ic "$TMP/nd.img" | grep '^missing')"
check "dcheck does not either: the gaps read as deleted entries" \
    "$TMP/nd.img:"	"$(dc "$TMP/nd.img")"
check "nor does fsck, in any of its five phases"	"" \
    "$(fs "$TMP/nd.img" | grep 'FILE SYSTEM WAS MODIFIED')"

# ---------------------------------------------------------------------------
# 9. ncheck and quot, and the two RELATIONS they add.
#
# Five readers is not five times one reader.  What a sixth and seventh are
# worth is whether they compute something the others do not, and these two do:
#
#   icheck	walks di_addr[] -- the blocks an inode actually OWNS
#   quot	computes ceil(di_size/BSIZE) -- the blocks its LENGTH implies
#   ncheck	walks the directory tree and builds a path for every inode
#
# So quot and icheck read different FIELDS of the same inodes and can be made
# to disagree, and the size of the disagreement is exactly the metadata:
#
#	quot's block total + icheck's indirect count == icheck's `used'
#	quot's file total                            == icheck's `files'
#
# Both are asserted as arithmetic against the other program's output, never
# against a transcribed number, so they still mean something at another size --
# and section 6's indirect-block image is included precisely because it is the
# one where the two computations DIFFER.
#
# ncheck's contribution is different in kind and is the 1985 workflow: every
# corruption case above ends in "inode=2", which is a number.  ncheck is what
# turns that into a filename, and it is the only program here that can.
# ---------------------------------------------------------------------------
NCHECK=$V8ROOT/etc/ncheck
QUOT=$V8ROOT/etc/quot
for f in "$NCHECK" "$QUOT"; do
	[ -x "$f" ] || { echo "missing $f -- run make"; exit 1; }
done

# A uid with no passwd entry, DERIVED rather than assumed.  rootfs/etc/passwd
# is generated from whoever ran the build (see the Makefile rule), so a
# hardcoded 3 is a claim about the tester's machine; this is a claim about the
# file.  uid 0 is safe to name because root is always written.
NOUID=3
while grep -q "^[^:]*:[^:]*:$NOUID:" "$V8ROOT/etc/passwd" 2>/dev/null; do
	NOUID=$((NOUID+1))
done

# An image with a subdirectory, so ncheck's pname() has to recurse, and with a
# file owned by that uid, so quot has to print the numeric form.  PIMG's flat
# root would exercise neither.
echo 'hello from v8' > "$TMP/n1"
echo 'aaa'           > "$TMP/n2"
{ printf '/dev/null\n2000 1280\nd--777 0 0\n'
  printf 'hello\n---644 0 0 %s\n' "$TMP/n1"
  printf 'sub\nd--755 0 0\n'
  printf   'deeper\n---600 %s 4 %s\n' "$NOUID" "$TMP/n2"
  printf '$\n$\n'
} > "$TMP/proto.n"
NIMG=$TMP/n.img
"$MKFS" "$NIMG" "$TMP/proto.n" >/dev/null 2>&1 ||
	bad "mkfs refused the subdirectory prototype"
nic=$(ic "$NIMG")
check "the ncheck image checks clean first"	"missing 0" \
    "$(printf '%s\n' "$nic" | grep '^missing')"

# --- ncheck ----------------------------------------------------------------
nc() { "$NCHECK" $2 "$1" 2>&1 | sed 1d | sq; }

# A directory gets `/.' appended by upstream, which is how ncheck marks one.
check "ncheck names every file, path built by recursion" \
    "3 /hello 4 /sub/. 5 /sub/deeper" "$(nc "$NIMG" | tr '\n' ' ' | sq)"
check "ncheck -a adds the dot entries"	"7" \
    "$("$NCHECK" -a "$NIMG" 2>&1 | sed 1d | wc -l | tr -d ' ')"
check "ncheck -i names one inode"	"5 /sub/deeper" "$(nc "$NIMG" '-i 5')"
check "and takes a list"	"3 /hello 5 /sub/deeper" \
    "$(nc "$NIMG" '-i 3 5' | tr '\n' ' ' | sq)"

# THE PAIRING WITH A CORRUPTION, which is what ncheck is for.  Section 5's
# case C reports `inode=2'; here the damage goes to a file two levels down so
# the answer is a path rather than the root, and the inode number is taken from
# icheck's own output rather than written here twice.
cp "$NIMG" "$TMP/nc3.img"
# inode 5, /sub/deeper -- itod/itoo as at I2 above, not I2 + 3*DINODE, because
# an inode three slots along could be in the next block.
I5=$(( ((5 + 2*INOPB - 1) / INOPB) * BSZ + ((5 + 2*INOPB - 1) % INOPB) * DINODE ))
printf '\377\377\177' | dd of="$TMP/nc3.img" bs=1 seek=$((I5+12)) count=3 \
    conv=notrunc 2>/dev/null
badino=$(ic "$TMP/nc3.img" | sed -n 's/.*inode=\([0-9]*\).*/\1/p')
check "icheck names the inode of the bad address"	"5"	"$badino"
check "and ncheck turns that number into a path" "$badino /sub/deeper" \
    "$(nc "$TMP/nc3.img" "-i $badino")"

# --- quot ------------------------------------------------------------------
# Two columns, one row per uid, sorted by blocks descending.  The `#N' row is
# the one that used to take the whole program down: 2046 of quot's 2048 du[]
# entries have a null name, qsort compares those against each other, and V8's
# qcmp reaches strcmp(0,0).  A row printed at all is the regression case.
# Columns are tab-separated, so awk on $1..$3 rather than sq-then-cut: sq turns
# the tabs into single spaces and cut -f then sees one field.
qf() { "$QUOT" -f "$1" 2>&1 | sed 1d | awk "$2" | sq; }
check "quot names uid 0 and the unowned uid numerically" \
    "root #$NOUID" "$(qf "$NIMG" '{printf "%s ", $3}')"
check "quot -f gives blocks and files per uid"	"3 4 1 1" \
    "$(qf "$NIMG" '{printf "%s %s ", $1, $2}')"

# THE RELATIONS.  Summed with awk rather than transcribed, on both images.
qsum() { "$QUOT" -f "$1" 2>&1 | sed 1d | awk -v c="$2" '{t+=$c} END {print t+0}'; }
icf() { printf '%s\n' "$1" | grep "^$2" | cut -d' ' -f2; }
# `used 22 (i=1,ii=0,iii=0,d=21)' -> 1
icind() { printf '%s\n' "$1" | sed -n 's/.*(i=\([0-9]*\),.*/\1/p'; }

check "quot's file total is icheck's file count"	"$(icf "$nic" files)" \
    "$(qsum "$NIMG" 2)"
check "quot's blocks plus icheck's indirect is icheck's used" \
    "$(icf "$nic" used)" "$(( $(qsum "$NIMG" 1) + $(icind "$nic") ))"

# The same two on section 6's image, where the indirect block makes the two
# computations differ -- 21 blocks of file against 22 blocks allocated.  Without
# this the relation above could be satisfied by both programs counting the same
# thing, since every other image here has i=0.
if [ "$(icind "$bout")" = 1 ]; then pass=$((pass+1))
else bad "the indirect-block image reports i=$(icind "$bout"), so the case below is vacuous"; fi
check "on the indirect image quot is one block short of icheck" \
    "$(icf "$bout" used)" "$(( $(qsum "$BIMG" 1) + $(icind "$bout") ))"
check "and that one block is the indirect block itself" "1" \
    "$(( $(icf "$bout" used) - $(qsum "$BIMG" 1) ))"

# THE COMPOSITION, WHICH IS UPSTREAM'S OWN AND NOT OURS.  quot(8)'s manual page
# says: "Cause the pipeline `ncheck filesystem | sort +0n | quot -n filesystem'
# to produce a list of all files and their owners."  Both new programs plus V8's
# sort, run verbatim.  A program that works alone and not in the composition its
# own documentation specifies is exactly the shape `grap | pic | troff' caught
# in Wave C, where each stage was fine and the seam was not.
# (`3' rather than `#3' is upstream: the -n arm prints %d where report() prints
# #%d.)
check "the manual's own ncheck | sort +0n | quot -n pipeline" \
    "root /hello root /sub/. $NOUID /sub/deeper" \
    "$("$NCHECK" "$NIMG" 2>/dev/null | sed 1d | "$V8ROOT/usr/bin/sort" +0n \
       | "$QUOT" -n "$NIMG" 2>&1 | sed 1d | tr '\n\t' '  ' | sq)"

# --- the two crashes, which are regression cases and nothing else -----------
# Both are the "V8 assumes address 0 is readable" class that refer5.c already
# records, and both are on ordinary command lines rather than exotic ones.
"$NCHECK" -i 5 >/dev/null 2>&1; nrc=$?
check "ncheck -i with no image exits, rather than faulting on atol(0)" "0" "$nrc"
# -s IS NOT IN THE SAME CATEGORY AND THIS CASE SAYS SO RATHER THAN PRETENDING.
# pass3() reaches `pr:' from both sides of the -i test but sets k only on one,
# so with -s and no -i list ilist[k] indexes by an uninitialised int -- ldrsw'd
# and shifted, a +-4 GB signed offset from ilist, which the VAX could read and
# macOS mostly cannot.  ncheck.c initialises k, and the disassembly says the
# read is real; MUTATION DOES NOT REPRODUCE THE FAULT.  Measured: with the
# initialiser removed, ten runs exit 0, because the slot holds a small stale
# value left by the same frame.  So this is not a guard on that fix and must
# not be read as one -- it asserts the CONTRACT, that -s adds a mode column
# only for inodes named by -i and therefore changes nothing without one, which
# is deterministic and true either way the fault lands.
check "ncheck -s with no -i list prints what a plain run prints" \
    "$(nc "$NIMG")" "$(nc "$NIMG" -s)"
"$QUOT" "$NIMG" >/dev/null 2>&1; qrc=$?
check "quot's default invocation exits, rather than faulting in qcmp" "0" "$qrc"

# AND THE TWO COPIES OF ncheck's LOOP THAT THE FIX ABOVE MISSED.  `n =
# atol(argv[1])' inside an option's number loop appears THREE times in this
# tree -- ncheck -i, icheck -b, dcheck -i -- byte for byte the same, and only
# ncheck was swept when it was found.  Both siblings SIGSEGV'd on the same
# shape of command line until the whole-tree sweep went looking.
"$ICHECK" -b 5 >/dev/null 2>&1; ibrc=$?
check "icheck -b with its number last exits, rather than faulting on atol(0)" \
    "0" "$ibrc"
"$DCHECK" -i 5 >/dev/null 2>&1; dirc=$?
check "dcheck -i with its number last exits, rather than faulting on atol(0)" \
    "0" "$dirc"
# ...AND THE OPTIONS MUST STILL WORK, which is the half an exit-status case
# cannot see: a bare `return' in front of the deref would stop the crash and
# also stop the loop consuming its numbers, and the very next argument -- the
# image itself -- would be eaten as one.  So assert the image was still read.
check "icheck -b still consumes its list and then checks the image" "1" \
    "$("$ICHECK" -b 5 "$NIMG" 2>&1 | grep -c '^files')"
check "dcheck -i still consumes its list and then checks the image" "1" \
    "$("$DCHECK" -i 2 "$NIMG" 2>&1 | grep -c 'n\.img:')"
# and -i 2 genuinely uses the number: inode 2 is the root, so it is named by
# its own `.' and `..' and once more by the `..' of /sub, which is inode 4.
# Three entries, and the third is what says the walk is real rather than the
# first two being echoed back.
check "dcheck -i 2 reports every reference to the root" \
    "2 arg; 2/. 2 arg; 2/.. 2 arg; 4/.." \
    "$("$DCHECK" -i 2 "$NIMG" 2>&1 | sed 1d | tr '\n' ' ' | sq | sed 's/ *$//')"

# --- section 8's question, asked of the reader that fails silently ----------
# mkfs built without the flag writes a wrong image that every reader accepts.
# ncheck is the mirror: built without the flag it reads a RIGHT image and
# prints nothing at all, exit status 0.  NDIR(dev) comes out 4 instead of 64
# and the step is 256 bytes rather than 16, so a root whose di_size is 64 is
# exhausted by its own `.', which dotname() then filters.
#
# This is why $(IMGBIN) is a group and why ncheck must never join $(V8BIN):
# that list is what $(SRCTREE) stages for Admin/Mk, and Mk compiles a bare
# cmd/*.c with `cc $CFLAGS -o $B $B.c' and no -D.
if "$CC" -Od2 -o "$TMP/ncheck-nodirsiz" "$ROOT/src/cmd/ncheck.c" \
   > "$TMP/nn.log" 2>&1; then
	pass=$((pass+1))
else
	bad "ncheck would not build the way Admin/Mk builds it" "$(head -3 "$TMP/nn.log")"
fi
check "without the flag ncheck prints nothing on a good image" "" \
    "$("$TMP/ncheck-nodirsiz" "$NIMG" 2>&1 | sed 1d)"
"$TMP/ncheck-nodirsiz" "$NIMG" >/dev/null 2>&1
check "and says so with exit status 0, which is the whole problem" "0" "$?"

# quot is in the same group and does NOT need the flag, which the Makefile
# claims and this measures: it reads inodes only, names no struct direct, and
# its object is byte-identical either way.  That is what leaves quot the one
# image tool whose own upstream makefile builds the same program -- see
# tests/jail for the rung-5 half.
"$CC" -Od2 -DDIRSIZ=14 -c -o "$TMP/q14.o" "$ROOT/src/cmd/quot/quot.c" 2>/dev/null
"$CC" -Od2            -c -o "$TMP/q0.o"  "$ROOT/src/cmd/quot/quot.c" 2>/dev/null
if cmp -s "$TMP/q14.o" "$TMP/q0.o"; then pass=$((pass+1))
else bad "quot.o differs with and without -DDIRSIZ=14 -- it reads directories now"; fi

# ---------------------------------------------------------------------------
# 10. THE ROUND TRIP: mkfs -> dump -> tape -> restor -> a second filesystem.
#
# Section 1 asserts struct spcl's layout and section 9 the readers, and NEITHER
# WOULD HAVE CAUGHT WHAT THIS CATCHES.  The tape format was correct -- an
# independent sum over every record of a written tape gives exactly CHECKSUM --
# and both readers rejected it anyway, because v8cc kept checksum()'s `register
# int' accumulator in a 64-bit register and never wrapped it at 32.  It printed
# the number it was looking for and took the not-equal branch.  So the format
# being right and the programs working are two different claims, and only
# running them asks the second.
#
# What makes the trip worth more than three separate cases: restor is the
# port's SECOND filesystem writer, and the five readers that judge its output
# know nothing about tapes.  A restored image that icheck, dcheck, ncheck and
# fsck all accept, holding the same tree, is a statement no single program here
# could make about itself.
#
# NOTE THE STDIN.  `restor r' reads a newline for "Last chance before
# scribbling on ...", and both readers spin forever on EOF rather than erroring
# -- upstream assumed an operator.  That is also why everything here runs under
# a deadline: the failure mode is a hang with no output, not a wrong answer.
# ---------------------------------------------------------------------------
DUMP=$V8ROOT/etc/dump
RESTOR=$V8ROOT/etc/restor
DUMPDIR=$V8ROOT/etc/dumpdir
for f in "$DUMP" "$RESTOR" "$DUMPDIR"; do
	[ -x "$f" ] || { echo "missing $f -- run make"; exit 1; }
done
dl() { perl -e 'alarm 40; exec @ARGV' "$@"; }

# dump exits 1 on success (X_FINOK), so the output is what is read.
dout=$( cd "$TMP" && dl "$DUMP" 0f "$TMP/tape" "$NIMG" </dev/null 2>&1 )
check "dump runs all four passes and finishes"	"DUMP IS DONE" \
    "$(printf '%s\n' "$dout" | sed -n 's/.*DUMP: \(DUMP IS DONE\).*/\1/p')"
if [ -s "$TMP/tape" ]; then pass=$((pass+1))
else bad "dump wrote no tape"; fi
# NTREC is 10 and each record is BSIZE(0), so a tape is a whole number of
# 10240-byte groups -- a partial group is still in dump's buffer and lost, which
# is why it writes NTREC copies of TS_END.
check "the tape is a whole number of NTREC groups"	"0" \
    "$(( $(wc -c < "$TMP/tape") % (10 * BSZ) ))"
check "record 0 is TS_TAPE, with the magic"	"1 60011" \
    "$(d4 "$TMP/tape" 0) $(d4 "$TMP/tape" 24)"
# ...and the format assertion that matters, on the bytes: c_date is FOUR bytes
# at offset 4 with c_ddate zero at 8.  At eight-byte times c_ddate would hold
# the high half of the date and every later field would have slid.
if [ "$(d4 "$TMP/tape" 4)" -gt 1700000000 ]; then pass=$((pass+1))
else bad "c_date at offset 4 is $(d4 "$TMP/tape" 4), not a plausible time"; fi
check "and c_ddate at offset 8 is zero for a level 0"	"0" \
    "$(d4 "$TMP/tape" 8)"

# dumpdir reads it back.  Flags are V7-style with no dash -- `dumpdir f tape' --
# and `-f' is silently ignored, which opens /dev/rmt2 instead.
ddout=$( cd "$TMP" && dl "$DUMPDIR" f "$TMP/tape" </dev/null 2>&1 )
# Inode 2 is the root, and it is named by `/.', `/..' and `/sub/..' -- dropping
# it leaves exactly what ncheck reports for the same image, which is the
# comparison worth making.
check "dumpdir lists the tree off the tape" \
    "/hello /sub/. /sub/deeper" \
    "$(printf '%s\n' "$ddout" | awk '$1 ~ /^[0-9]+$/ && $1 != 2 {printf "%s ", $2}' | sq)"
# THE DATE PAIR, and it is the narrowed-field seam.  ctime(&spcl.c_ddate) read
# c_volume as its high half and dated a level-0 dump to 2006; the fix is a
# time_t temporary, as in fsck's pinode().  Asserted as the epoch YEAR rather
# than a full date, because the epoch prints in local time.
check "dumpdir dates the dump to now, not to 2.4e11"	"2026" \
    "$(printf '%s\n' "$ddout" | sed -n 's/^Dump   date:.* \([0-9][0-9][0-9][0-9]\)$/\1/p')"
check "and reads c_ddate as the epoch, not as c_volume"	"1970" \
    "$(printf '%s\n' "$ddout" | sed -n 's/^Dumped from:.* \([0-9][0-9][0-9][0-9]\)$/\1/p')"

# ...and restor writes a SECOND filesystem from it.
"$MKFS" "$TMP/rest.img" 2000 >/dev/null 2>&1
( cd "$TMP" && printf '\n' | dl "$RESTOR" rf "$TMP/tape" "$TMP/rest.img" ) \
    >/dev/null 2>&1
rout=$(ic "$TMP/rest.img")
check "the restored image has the same five inodes" \
    "files 5 (r=3,d=2,b=0,c=0,l=0)" "$(printf '%s\n' "$rout" | grep '^files')"
check "and the same four blocks in use"	"used 4 (i=0,ii=0,iii=0,d=4)" \
    "$(printf '%s\n' "$rout" | grep '^used')"
check "with nothing unaccounted for"	"missing 0" \
    "$(printf '%s\n' "$rout" | grep '^missing')"
check "dcheck finds no link-count disagreement in it"	"$TMP/rest.img:" \
    "$(dc "$TMP/rest.img")"
check "ncheck finds the same tree by name" \
    "3 /hello 4 /sub/. 5 /sub/deeper" \
    "$(nc "$TMP/rest.img" | tr '\n' ' ' | sq)"
# fsck is the judge that would REPAIR a difference, so a clean pass is the
# strongest of the four -- and it must not modify anything.
check "and fsck accepts it without modifying it"	"" \
    "$(fs "$TMP/rest.img" | grep 'FILE SYSTEM WAS MODIFIED')"
# Anchored at the start of the line: a greedy `.*' in front eats the count and
# leaves " files 4 blocks", which reads as fsck having lost the number.
check "fsck counts what icheck counts"	"5 files 4 blocks" \
    "$(fs "$TMP/rest.img" | sed -n 's/^\([0-9]* files [0-9]* blocks\).*/\1/p' | tail -1)"

echo "mkfs: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
