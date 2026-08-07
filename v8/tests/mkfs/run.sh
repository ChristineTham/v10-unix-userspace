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
dc() { perl -e 'alarm 20; exec @ARGV' "$DCHECK" "$1" 2>&1 | sq; }

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

echo "mkfs: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
