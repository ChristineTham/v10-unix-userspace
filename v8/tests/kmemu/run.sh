#!/bin/sh
# libkmemu -- the shim's system-facts half, and the boundary of the one
# exception that lets any part of the shim link the host's libc.
#
# Two different things get asserted here and they fail in different ways:
#
#   * THE ANSWER IS TRUE.  who(1) is authentic 1985 source reading a file this
#     port manufactures, so a mistake in the record layout does not crash it --
#     it prints a plausible name for a plausible tty at a plausible time. The
#     only defence is comparing against the host's own who, field by field.
#
#   * THE EXCEPTION STAYED NARROW.  libkmemu may take getutxent(3) and its
#     neighbours from libc and nothing else, and only grovelers may link it.
#     Both of those are visible in the symbol table, so both are checked there
#     rather than argued about in a comment.

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CC=$ROOT/rootfs/bin/cc
V8ROOT=$ROOT/rootfs
export V8ROOT
TMP=${TMPDIR:-/tmp}/kmemu.$$
mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT

LIBC=$ROOT/build/stage0/libc/libv8c.a
CRT=$ROOT/build/stage0/crt0.o
STUBS=$ROOT/build/stage0/v8sys/libv8stubs.a
SHIM=$ROOT/build/stage0/v8sys/libv8sys.a
KMEMU=$ROOT/build/stage0/kmemu/libkmemu.a
WHO=$V8ROOT/bin/who
UTMP=$V8ROOT/etc/utmp

pass=0 fail=0 notex=0
check() {
	if [ "$2" = "$3" ]; then pass=$((pass+1))
	else fail=$((fail+1)); echo "FAIL $1"; echo "  want [$2]"; echo "  got  [$3]"; fi
}
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL $1"; shift; [ $# -gt 0 ] && echo "    $*"; }
# A CASE THAT DID NOT RUN IS STILL A CASE THIS PORT HAS, and until this helper
# existed nothing said so: every branch below printed its message and returned,
# so the number of cases a run reported was a property of the MACHINE.  That
# reached CI as a red build -- 2702 on a runner against 2705 here, every suite
# 0 failed -- with the difference being five skips there against two here.  The
# rule this tree already states for a test is "assert a relation the port
# controls"; the total is one and the exercised count is not, so they are two
# numbers and the summary line now prints both.
#
# The COUNT is an argument because a branch does not guard one case: the
# over-long mount point block runs two.  Continuation lines are separate
# arguments and are laid out exactly as the hand-written echoes were, so no
# message changes.
skip() {
	n=$1; shift
	notex=$((notex + n))
	printf '  (not exercised: %s' "$1"; shift
	for l in "$@"; do printf '\n   %s' "$l"; done
	printf ')\n'
}

# THE PROCESS TABLE IS FIXED AND THE HOST'S PROCESS COUNT IS NOT, so which
# assertion is AVAILABLE depends on the machine -- and for the life of this
# suite only one of the three was written.  procfs.c's PR_NPROC is 1024, a
# recorded CHOICE (V8's own NPROC came from a per-machine config file that is
# not in the vendored tree), and its note says the excess is REPORTED rather
# than dropped.  So above the table /proc saturates, on purpose; the bracket
# below it cannot hold there, and a run on a busy machine failed two cases
# that were describing the host rather than the port.  Measured when a booted
# iOS Simulator runtime took this Mac to 1217 processes.
#
# $prnproc is MEASURED, not transcribed: the probe reports the directory size
# and the record stride is 256, so the table size is (dirsize / 256 - 2).  A
# constant copied from procfs.c would agree with the source by construction
# and could never catch the two disagreeing.
prnproc=
prtrack() {
	_w=$1 _h=$2 _v=$3 _sat=$4
	if [ -z "$prnproc" ]; then
		bad "$_w" "the table size was never measured"
	elif [ "$_h" -gt $((prnproc + 40)) ]; then
		# A BAND, NOT AN EQUALITY, AND FOR THE SAME REASON THE TRACKING
		# ARM BELOW HAS ONE.  The arm used to demand exactly $_sat and
		# went red twice in one day -- `want [1025] got [1023]', then
		# `got [1020]' -- because processes exit between /proc's
		# directory read and the per-process opens that follow, so the
		# count comes in a few SHORT of the table.  That is the host
		# changing under a two-step measurement, not the port failing.
		#
		# It still discriminates: a /proc that TRACKED the host would
		# report ~$_h, which at this arm is more than prnproc+40 away.
		# Saturation and tracking are hundreds apart, so a 40-wide band
		# separates them with room to spare.  Never one-sided upward --
		# the table cannot report MORE than it holds, and a value above
		# $_sat would be a real defect.
		awk -v s="$_sat" -v v="$_v" \
		    'BEGIN { exit !(v <= s && v > s - 40) }' &&
			ok || bad "$_w -- saturated at the table" \
			          "want $_sat (or up to 40 under), got $_v"
	elif [ "$_h" -lt $((prnproc - 40)) ]; then
		awk -v h="$_h" -v v="$_v" 'BEGIN { exit !(v > h - 40 && v < h + 40) }' &&
			ok || bad "$_w" "host $_h, v8 $_v"
	else
		skip 1 "this host has $_h processes against a $prnproc-slot table," \
		       "so neither tracking nor saturation is decidable"
	fi
}

for f in "$WHO" "$KMEMU"; do
	[ -e "$f" ] || { echo "missing $f -- run make"; exit 1; }
done

# --- the file is manufactured, not found ---------------------------------
# Deleting it and having who(1) answer anyway is the whole claim: nothing in
# this world writes /etc/utmp, so if who still works the shim made it.
rm -f "$UTMP"
out=$("$WHO" 2>&1)
[ -f "$UTMP" ] && ok || bad "who creates /etc/utmp when it is missing"
[ -n "$out" ] && ok || bad "who prints something with no utmp on disk" "$out"

# --- and it is live, not a snapshot from build time ----------------------
# A stale file would pass every check above. Backdating it and watching the
# mtime move forward is what separates "manufactured" from "manufactured once".
touch -t 200001010000 "$UTMP"
old=$(stat -f %m "$UTMP")
"$WHO" > /dev/null 2>&1
new=$(stat -f %m "$UTMP")
[ "$new" -gt "$old" ] && ok || bad "every who() refreshes utmp, not just the first"

# --- the answer agrees with the host's own who ---------------------------
# who(1) prints "%-8.8s %-8.8s %.12s" of name, line and ctime+4, so each field
# is compared on its own -- the column widths are V8's and deliberately differ.
hostwho=$(/usr/bin/who | head -1)
v8who=$("$WHO" | head -1)
check "login name matches the host's who" \
	"$(echo "$hostwho" | awk '{print $1}')" \
	"$(echo "$v8who"   | awk '{print $1}')"
check "tty line matches the host's who" \
	"$(echo "$hostwho" | awk '{print $2}' | cut -c1-8)" \
	"$(echo "$v8who"   | awk '{print $2}')"
check "login time matches the host's who" \
	"$(echo "$hostwho" | awk '{print $3, $4, $5}')" \
	"$(echo "$v8who"   | awk '{print $3, $4, $5}')"

# --- one record per logged-in session, and only USER_PROCESS ones --------
# The host's utmpx also carries BOOT_TIME and dead-session records; V7 has no
# type field to express them, so emitting them would show phantom logins.
hostn=$(/usr/bin/who | wc -l | tr -d ' ')
v8n=$("$WHO" | wc -l | tr -d ' ')
check "one line per session, no phantoms" "$hostn" "$v8n"
bytes=$(wc -c < "$UTMP" | tr -d ' ')
check "utmp is exactly N 24-byte records" "$((hostn * 24))" "$bytes"

# --- the two ends of the seam agree about the record ---------------------
# libkmemu spells struct utmp again in C compiled by clang; who reads it through
# the authentic V8 header compiled by v8cc. If those ever disagree the output is
# garbage that still looks like output, so the size is asserted from the V8 side.
cat > "$TMP/size.c" <<'EOF'
#include <stdio.h>
#include <utmp.h>
main()
{
	struct utmp u;
	printf("%d %d %d\n", sizeof(struct utmp),
	    (char *)u.ut_name - (char *)u.ut_line,
	    (char *)&u.ut_time - (char *)u.ut_line);
	return 0;
}
EOF
if "$CC" -c -o "$TMP/size.o" "$TMP/size.c" > "$TMP/size.log" 2>&1 &&
   clang -nostdlib -e _v8start -o "$TMP/size" "$CRT" "$TMP/size.o" \
	"$LIBC" "$STUBS" "$SHIM" -lSystem >> "$TMP/size.log" 2>&1; then
	check "V8's struct utmp is 24 bytes, name at 8, time at 16" \
		"24 8 16" "$("$TMP/size")"
else bad "struct utmp probe build" "$(head -3 "$TMP/size.log")"; fi

# --- V7 fixed-width fields are NOT terminated when full ------------------
# ut_name is bytes 8..15 of the record, and a name that exactly fills it must
# have no NUL after it -- a C-string habit here silently truncates every
# 8-character login to 7. Read as bytes, not as whatever printf made of them.
# WHOSE name is in record 0 is a host question, and this used to answer it with
# `id -un'.  The ut_line check on the next line already gets it right -- it
# compares against $hostwho, the FIRST record of the host's own who -- and the
# name check reached for the tester's login instead.  Those agree only while
# record 0 belongs to whoever is running the suite; a second user's session
# ordering first in getutxent (a shared Mac, an ssh, Screen Sharing) failed it,
# with a diff that reads like who naming the wrong person.  Neither
# /usr/bin/who nor shim/libkmemu/utmp.c sorts, so both walk getutxent in the
# same order and $hostwho is the record actually being examined.
rec0=$(echo "$hostwho" | awk '{print $1}')
name=$(dd if="$UTMP" bs=1 skip=8 count=8 2>/dev/null | tr -d '\0')
check "ut_name holds the login, terminator or not" "$(echo "$rec0" | cut -c1-8)" "$name"
line=$(dd if="$UTMP" bs=1 skip=0 count=8 2>/dev/null | tr -d '\0')
check "ut_line holds the tty" \
	"$(echo "$hostwho" | awk '{print $2}' | cut -c1-8)" "$line"
# CLAMPED TO THE FIELD WIDTH, and that is the bug this line used to have.  The
# else branch below was taken for names both SHORTER and LONGER than 8, and for
# a 9-character login `skip=$((8 + 9))' is byte 17 -- past ut_name, which is
# bytes 8..15, and into ut_time.  It asserted a byte of the timestamp was zero.
# `christie' is exactly 8 and `runner' is 6, so neither this host nor CI ever
# took the broken path; `administrator' does, and fails talking about zero-fill.
#
# A name longer than the field fills it, so clamping sends it to the branch
# that is actually true of it.
n=${#rec0}; [ "$n" -gt 8 ] && n=8
if [ "$n" -eq 8 ]; then
	# The case that would pass anyway with a NUL-terminating copy: the
	# eighth byte must be the eighth character, not '\0'.
	last=$(dd if="$UTMP" bs=1 skip=15 count=1 2>/dev/null)
	check "an 8-character login fills ut_name with no terminator" \
		"$(echo "$rec0" | cut -c8)" "$last"
else
	# Not this host's shape. Assert the other half of the rule instead: a
	# short name is zero-filled, which is what who(1) tests for a free slot.
	pad=$(dd if="$UTMP" bs=1 skip=$((8 + n)) count=1 2>/dev/null | od -An -tu1 | tr -d ' ')
	check "a short login is zero-filled to the field width" "0" "$pad"
fi

# --- -i reaches the tty, which is a host fact seen through the jail ------
# who -i stats /dev/<line> for its atime. That crosses the shim's /dev handling
# as well as libkmemu's, and it is the one who(1) path that reads anything
# beyond utmp.
#
# THIS CASE ASSERTED A PROPERTY OF THE MACHINE AND WAS WRONG ABOUT IT. It
# compared $hostwho -- which is `who | head -1', ONE line -- against every line
# of `who -i'. That is an equality only while the host has exactly one login
# session, and it held for months because this one did. Opening a second
# terminal broke it, with a diff that reads like who printing the user twice.
#
# Same disease as the p_nice and pid cases, running the other way: those passed
# here and failed on a CI runner, this passes on a runner (one session) and
# fails here. A host property is not safe just because the direction of the
# accident is unfamiliar. So compare first line to first line, as the three
# sibling checks above already do, and assert the RELATION separately.
"$WHO" -i > "$TMP/idle" 2>&1
check "who -i still names the user" \
	"$(echo "$hostwho" | awk '{print $1}')" \
	"$(head -1 "$TMP/idle" | awk '{print $1}')"
# The relation the port actually controls: -i must not add, drop or duplicate a
# record on its way through the shim's /dev. True at any number of sessions,
# which is the whole point of stating it this way.
check "who -i reports the same sessions as who" \
	"$("$WHO" | wc -l | tr -d ' ')" \
	"$(wc -l < "$TMP/idle" | tr -d ' ')"
# ...and the idle column itself, which is the only thing -i adds and the only
# reason it touches /dev. An ACTIVE tty prints no idle field, so whether any
# line carries one depends on the host having a session nobody has typed at.
# Printing "not exercised" rather than passing silently: a check that quietly
# matches nothing is indistinguishable from a check that passed.
idlecol=$(awk 'NF>=6 {print $NF}' < "$TMP/idle" | head -1)
if [ -n "$idlecol" ]; then
	case "$idlecol" in
	*:*|old) ok ;;
	*) bad "who -i idle column is not HH:MM or 'old': [$idlecol]" ;;
	esac
else
	skip 1 "no idle session on this host"
fi

# --- THE BOUNDARY: who takes the utmpx trio from libc and nothing else ---
# This is the exception written down as an assertion. If libkmemu ever reaches
# for a string function, an allocator or stdio, it shows up here as a fourth
# import -- which is exactly how an exception list stops meaning anything.
libcimports() {
	nm -mu "$1" 2>/dev/null | grep -v dyld_stub_binder |
	    sed 's/.*external _//;s/ (from.*//' | sort | tr '\n' ' ' | sed 's/ $//'
}
# The set is libkmemu's WHOLE surface, not just what each program happens to
# call: the synth table in synth.c names every generator, so linking any one of
# them pulls all of them. That is the honest statement and the one worth
# pinning. Six symbols, every one of them on PLAN.md section 7's sanctioned
# list -- getutxent(3) and its bookends, getfsstat(2), statfs(2), sysctl(3) --
# and all of them answering "what is mounted / what is running / who is logged
# in". A SEVENTH appearing here means the exception grew, and that is a decision
# to be made deliberately rather than noticed later.
#
# sysctlbyname arrived with load(1), for vm.loadavg. It is sysctl(3) under its
# by-name spelling, same man page, same family.
#
# proc_listpids arrived with /proc (shim/libkmemu/procfs.c), and it is the
# seventh -- so it went through the deliberation this check exists to force.
# It is named on PLAN.md section 7's sanctioned list already ("process/utmp
# facts from sysctl/libproc/utmpx"), it answers "what is running", and it is
# the interface Apple documents for exactly that. Sanctioned, and recorded here
# rather than absorbed: an eighth still has to argue its case.
#
# proc_pidinfo is the eighth, and it arrived with PIOCGETPR -- the same file,
# the same question, the next level of detail. proc_listpids answers "which
# processes exist" and this answers "what is true of one of them", which is the
# whole content of a struct proc. Same man page as proc_listpids, same
# sanctioned clause. Note what it did NOT bring: the tick rate it needs comes
# from sysctl and the wall clock from a raw syscall, because rawsys.h already
# covers gettimeofday and mach_timebase_info() would have been a second way to
# ask something sysctl can answer. Convenience is how an exception list stops
# meaning anything.
KMEMU_IMPORTS="endutxent getfsstat getutxent proc_listpids proc_pidinfo setutxent statfs sysctlbyname"
check "who imports libkmemu's whole surface and no more" \
	"$KMEMU_IMPORTS" "$(libcimports "$WHO")"
check "df imports the same set, being the same library" \
	"$KMEMU_IMPORTS" "$(libcimports "$V8ROOT/bin/df")"
check "and so does load" \
	"$KMEMU_IMPORTS" "$(libcimports "$V8ROOT/usr/bin/load")"
# ps is the reason proc_pidinfo is on the list at all, and the interesting one:
# it is the first program here that reads a FILESYSTEM this port implements
# rather than a file it manufactures. Same set, because it is the same library.
check "ps too, and it is the same library not a second one" \
	"$KMEMU_IMPORTS" "$(libcimports "$V8ROOT/bin/ps")"

# ------------------------------------------------------------------ df ---
DF=$V8ROOT/bin/df
MTAB=$V8ROOT/etc/mtab
FSTAB=$V8ROOT/etc/fstab

# Both files are manufactured, like utmp. fstab too, and that one is a
# DELIBERATE overwrite of an authentic artefact: the rootfs used to carry Bell
# Labs' own /etc/fstab, listing /dev/ra00 through /dev/ra23. df's devlen() reads
# fstab, merges anything not already mounted, and takes BOTH column widths from
# it -- so with the museum piece in place df printed rows for /usr1, /fsave and
# /v8, disks belonging to a VAX in New Jersey in 1985.
rm -f "$MTAB" "$FSTAB"
"$DF" > "$TMP/df.out" 2>"$TMP/df.err"
[ -f "$MTAB" ]  && ok || bad "df creates /etc/mtab"
[ -f "$FSTAB" ] && ok || bad "df creates /etc/fstab"

# No phantom disks. The specific failure this replaced, named by its symptom.
if grep -qE '\bra[0-9][0-9]\b|/usr1|/fsave' "$TMP/df.out"; then
	bad "df is reporting Bell Labs' VAX disks" "$(grep -m2 -E 'ra[0-9]|/usr1' "$TMP/df.out")"
else ok; fi

# THE BUG THAT COST THE MOST HERE: mtab fields are NUL-terminated and utmp's are
# not. df does strcpy(&specbuf[5], mtab[i].spec) into 38 bytes, so a field
# filled to its 32-byte brim with no terminator runs into the next record,
# overflows specbuf and smashes the static after it -- which was ecvt's digit
# buffer, so the %use column emitted hundred-digit strings SEVERAL ROWS LATER.
# Assert the terminator directly; the symptom is too far from the cause.
#
# THE OFFSETS ARE DERIVED, and hardcoding them silently disarmed this guard for
# a whole release of the port.  They were 31 and 63, the last bytes of fields 0
# and 1 when FSNMLG was 32.  Widening FSNMLG to 1024 (src/include/fstab.h:39)
# moved the field ends to 1023 and 2047 and left the constants behind, so both
# probes landed INSIDE field 0 -- and the case became "the first mount point's
# path is at most 31 characters", true here only because record 0 is `/'.  The
# bug it exists for was undetectable, and the comment above it still said 128
# while the code below said 1024.  Three numbers for one quantity.
#
# So FSNMLG comes from one place, above its first use, and every offset is
# computed from it.  This is dir.c's DIRSIZ lesson at test scope: a constant
# spelled more than once is a constant that will disagree with itself.
FSNMLG=1024
recsz=$((FSNMLG * 2))
#
# ...AND THE ASSERTION IS THE INVARIANT, NOT TWO BYTES.  Probing the last byte
# of each field only says anything when a path FILLS the field: shorter ones are
# zero-filled, so the byte is 0 either way and the case cannot fail on a host
# whose longest mount point is 60 characters.  Mutating mtab.c to drop the
# terminator changed nothing at all, which is how that was established.
#
# What df actually requires is that a NUL exist SOMEWHERE inside each field,
# because dfree() does strcpy(&specbuf[5], ...) and stops at one.  That is true
# and checkable at every path length, so it is what is checked -- per record,
# both fields, across the whole file.
badterm=""
nrec0=$(( $(wc -c < "$MTAB" | tr -d ' ') / recsz ))
r=0
while [ "$r" -lt "$nrec0" ]; do
	for fo in 0 $FSNMLG; do
		z=$(dd if="$MTAB" bs=1 skip=$((r * recsz + fo)) count=$FSNMLG 2>/dev/null |
		    tr -d '\0' | wc -c | tr -d ' ')
		[ "$z" -lt "$FSNMLG" ] || badterm="$badterm record $r field $fo;"
	done
	r=$((r + 1))
done
[ -z "$badterm" ] && ok || bad "an mtab field has no NUL inside its width" "$badterm"
# ...and the symptom, so a regression is caught even if the layout changes:
# every %use must be a number followed by %, never a digit avalanche.
if awk 'NR>1 {print $NF}' "$TMP/df.out" | grep -qvE '^[0-9]{1,3}%$'; then
	bad "df printed a malformed %use column" \
	    "$(awk 'NR>1 {print $NF}' "$TMP/df.out" | grep -vE '^[0-9]{1,3}%$' | head -1 | cut -c1-40)"
else ok; fi

# The numbers are the host's. df's kbytes column is s_fsize - s_isize, which
# libkmemu fills from statfs f_blocks scaled to 1K -- so it must equal what the
# host's own df -k calls 1024-blocks for the same filesystem.
#
# EVERY ROW, KEYED ON THE DIRECTORY, and both halves of that are corrections.
# This case used to take the device out of df's FIRST row and ask the host about
# /dev/<that>.  It passed for the life of the port and then failed the day the
# tree was moved to a second volume -- not because df regressed but because the
# case had a host property in it: in the V8 world "/" is $V8ROOT, so df's first
# row is labelled with whatever volume holds the REPO, and comparing that
# label's host figure against that row's numbers only agrees when the repo sits
# on the host's root volume.
#
# IT WAS ALSO HIDING A REAL DEFECT, which is why the fix is not merely to skip
# that row.  df resolved the row's dev and dir from the mtab entry it matched by
# device number and took its NUMBERS from the caller's path, so the `/' row said
# disk5s1 and reported the host root's block count -- 1948455240 against
# 976557016.  src/cmd/df/PORTING.md.  Checking every row against the host by
# MOUNT POINT is the relation the port controls, it is true on any machine, and
# it is what catches that class rather than one instance of it.
dfrows=0 dfbad=
while read -r dfdir dfkb; do
	[ -n "$dfdir" ] || continue
	dfhost=$(df -k "$dfdir" 2>/dev/null | awk 'NR==2 {print $2}')
	[ -n "$dfhost" ] || continue		# the host cannot describe it either
	dfrows=$((dfrows+1))
	[ "$dfhost" = "$dfkb" ] || dfbad="$dfbad $dfdir(v8=$dfkb host=$dfhost)"
done <<EOF
$(awk 'NR>1 {print $1, $3}' "$TMP/df.out")
EOF
if [ "$dfrows" -eq 0 ]; then
	bad "df named no filesystem the host could confirm"
else
	check "df's kbytes matches the host on all $dfrows rows" 'none' "${dfbad:-none}"
fi

# df -i reports the FORMAT's ceiling, not the volume's contents, and that is
# the honest answer rather than a plausible one: s_isize and s_tinode are 16-bit
# in V7, so a volume with 548 million inodes cannot be described. A V8 df could
# not have shown it either. What must never happen is a number in between.
#
# ASSERTED AS min(host, 65535) RATHER THAN AS 65535, because the flat constant
# assumes the volume has more than 65535 free inodes. This one has 447 million,
# so it saturates; a small or nearly-full filesystem arriving first would give a
# legitimate in-range number and fail. The relation is true at every volume and
# still catches a truncated hand-off, which is the whole point.
#
# KEYED ON THE DIRECTORY, for the reason the kbytes case above now is: this
# block used to share that case's `hostdev' -- df's own first row, which is
# labelled with whatever volume holds the repo -- and asking the host about
# /dev/<that> carries the same assumption.  Taking the row df prints and asking
# the host about the same MOUNT POINT is a relation rather than a coincidence.
"$DF" -i > "$TMP/dfi.out" 2>/dev/null
dfidir=$(awk 'NR==2 {print $1}' "$TMP/dfi.out")
ifree=$(awk -v d="$dfidir" '$1 == d {print $(NF-1); exit}' "$TMP/dfi.out")
hostff=$(/bin/df -i "$dfidir" 2>/dev/null | awk 'NR==2 {print $7}')
if [ -n "$hostff" ] && [ "$hostff" -eq "$hostff" ] 2>/dev/null; then
	want=$hostff; [ "$want" -gt 65535 ] && want=65535
	check "df -i reports min(host free inodes, the 16-bit ceiling)" "$want" "$ifree"
else
	# Without the host's number there is nothing to relate to. The ceiling is
	# still the only value V7's 16-bit s_tinode can express, so assert that
	# and say the weaker form was used.
	check "df -i saturates ifree at the 16-bit ceiling" "65535" "$ifree"
	echo "  (host free-inode count unavailable for $dfidir; ceiling only)"
fi

# -l walks the free-block list, and there is no free list -- there is no disk.
# df says so in its own words rather than being handed a fabricated one.
"$DF" -l > "$TMP/dfl.out" 2>&1
grep -q 'bad free count' "$TMP/dfl.out" && ok ||
	bad "df -l invented a free list instead of failing" "$(head -2 "$TMP/dfl.out")"

# --- a mount point in mtab MUST BE A PATH THAT RESOLVES ---------------------
# This is the invariant the 32-byte field broke, and it is not the cosmetic
# loss the comment in mtab.c used to claim. A truncated NAME is a wrong name;
# a truncated PATH stops resolving -- and df's dfree() branches on stat(file)
# succeeding, taking its "this string is a device name" arm when it does not.
# So /Library/Developer/CoreSimulator/Volumes/iOS_23F77 became
# /Library/Developer/CoreSimulato, failed to stat, and printed as a row with an
# empty dir column and the path's first nine characters in the dev column.
#
# FSNMLG is 1024 (src/include/fstab.h:39, src/include/PORTING.md), so a record
# is 2048 bytes rather than 64.  Defined once, above the terminator check that
# is its first use -- see the note there for what hardcoding it cost.
bytes=$(wc -c < "$MTAB" | tr -d ' ')
[ $((bytes % recsz)) -eq 0 ] && ok ||
	bad "mtab is a whole number of $recsz-byte records" "$bytes bytes"

# Every path, read out of the file at the record stride and checked against the
# filesystem. Nothing else in this suite would notice a truncated one: the
# bytes are well-formed, the field is terminated, and df prints SOMETHING.
badpath="" mtpaths=""
nrec=$((bytes / recsz))
i=0
while [ "$i" -lt "$nrec" ]; do
	p=$(dd if="$MTAB" bs=1 skip=$((i * recsz)) count=$FSNMLG 2>/dev/null |
	    tr -d '\0')
	if [ -n "$p" ]; then
		mtpaths="$mtpaths$p
"
		[ -d "$p" ] || badpath="$badpath [$p]"
	fi
	i=$((i + 1))
done
check "every mount point in mtab is a directory that exists" "" "$badpath"

# The symptom, asserted directly: df's dir column is fixed width and a row that
# took the wrong branch leaves it blank.
blank=$(awk 'NR>1 && substr($0,1,1) == " "' "$TMP/df.out" | head -2)
check "no df row has an empty dir column" "" "$blank"

# The two manufactured files must agree about which filesystems exist, because
# df's devlen() MERGES any fstab entry whose device is not already in mtab --
# so dropping a mount from one and not the other hands it back through the
# merge. They already disagreed about the CoreSimulator mounts by one
# character, field0 terminating where puts0 did not.
mt=$(printf '%s' "$mtpaths" | sort -u)
fs=$(awk -F: '{print $2}' "$FSTAB" | grep '^/' | sort -u)
check "mtab and fstab name the same mount points" "$fs" "$mt"

# --- both sides of the boundary, each only where the host provides one -------
# The first version of this asked for "the longest mount point" and asserted df
# showed it. That is another assertion about the machine: it holds only if the
# longest one FITS. A CI runner mounts a Siri asset bundle at 154 characters,
# which fitted in neither the old 32 nor the 128 first tried here -- so the
# check failed for the right reason and the wrong assertion. Ask each question
# of a mount point that can answer it, and say so when the host supplies none.
#
# Past the OLD 32-byte field but inside the new one: df must show it whole.
#
# THE QUALIFIER IS ABOUT MORE THAN LENGTH, and this block has now learned that
# twice. The first version asked for "the longest mount point" full stop, and
# a 154-character Siri asset bundle on a CI runner failed it. The fix added a
# length window -- and left the other assumption standing: that a mount point
# in the window is one df can put in a TABLE at all.
#
# A Time Machine local snapshot is not. Its device is
# `com.apple.TimeMachine.2026-...local@/dev/disk3s5', which is not a /dev node,
# so df prints upstream's own "mounted on unknown device" on STDERR and the
# path never reaches df.out. Correct behaviour, failing check. So the mount
# must also be one whose device is a real /dev entry -- `mount' prints those as
# `/dev/diskNsM on /path', which is exactly the filter.
longmp=$(mount | grep '^/dev/' | sed 's/.* on //; s/ (.*//' |
         awk -v n="$FSNMLG" 'length($0) > 31 && length($0) < n' |
         awk '{print length($0), $0}' | sort -rn | head -1 | cut -d' ' -f2-)
if [ -n "$longmp" ]; then
	grep -qF "$longmp" "$TMP/df.out" && ok ||
		bad "df shows a mount point past the old 32-byte field" "missing: $longmp"
else
	skip 1 "no /dev-backed mount point here between 32 and $FSNMLG characters"
fi

# Past the NEW field: reported on stderr and absent from the listing, never
# truncated into a row. Unreachable on a host whose paths all fit -- MAXPATHLEN
# is the host's own width for this field -- so this is a guard for a case the
# kernel cannot currently produce, verified by mutation instead (lower the
# threshold in toolong() and watch df report and drop).
toobig=$(mount | sed 's/.* on //; s/ (.*//' | awk -v n="$FSNMLG" 'length($0) >= n' | head -1)
if [ -n "$toobig" ]; then
	grep -qF "$toobig" "$TMP/df.err" && ok ||
		bad "an over-long mount point is reported" "silent: $toobig"
	grep -qF "$toobig" "$TMP/df.out" &&
		bad "...and left out of the listing" "still listed: $toobig" || ok
else
	skip 2 "every mount point here fits in $FSNMLG"
fi

# ---------------------------------------------------------------- load ---
# The first program to need a NAMELIST. load(1) does not ask the system for the
# load average: it looks up the address of the kernel's _avenrun in /unix, then
# seeks to that address in /dev/kmem and reads three doubles. So the shim
# manufactures a kernel, and one table in kmem.c drives both files -- get them
# out of step and load reads the wrong bytes and prints them without complaint.
LOAD=$V8ROOT/usr/bin/load
# SNAPSHOT BEFORE THE rm, AND RESTORE FROM THE SNAPSHOT -- not from a list.
# This block used to put back three names by hand (`dk', `pt', `drum'), and by
# the time /dev/fd, the four std nodes and /dev/null had arrived that was
# THREE OF 136.  So every `make test' left the BUILD TREE's rootfs without a
# /dev/null, a /dev/tty or a /dev/fd until the next `make' quietly rebuilt
# them -- invisible three ways: every suite that reads those runs BEFORE this
# one, running one alone rebuilds its prerequisites first, and $(PREFIX)/golden
# was never affected because `make install' opens with `make clean' for an
# unrelated reason.  The artefact anyone would inspect was the correct one.
# A hand-written restore list is the two-copies-of-a-list trap with the copies
# a year apart.
cp -a "$V8ROOT/dev" "$TMP/devsave"
rm -rf "$V8ROOT/unix" "$V8ROOT/dev"
"$LOAD" > "$TMP/load.out" 2>"$TMP/load.err"
[ -f "$V8ROOT/unix" ]     && ok || bad "load creates /unix"
[ -f "$V8ROOT/dev/kmem" ] && ok || bad "load creates /dev/kmem"

# The manufacturer makes its own directory; assert that rather than a build
# step nobody will remember.
[ -d "$V8ROOT/dev" ] && ok || bad "the /dev directory was created on demand"

# THE LOOP HERE USED TO ASSERT THAT rootfs/dev/null AND rootfs/dev/tty DO NOT
# EXIST, "because the jail lets /dev/null and /dev/tty reach the host precisely
# by NOT having them".  That was true when it was written and is now false for
# both, and it passed anyway -- because the `rm -rf' four lines up had just
# deleted them, so the loop was asserting a post-condition of its own cleanup
# rather than a property of the build.  The same sentence went stale in
# shim/v8sys/vfs.c and had to be corrected there too; one claim, two copies.
#
# What replaced it: /dev/tty has been a build product since the /dev/fd type
# landed (the NAME has to be real for `ls /dev'; the mount table claims the
# path first so the node is never opened), and /dev/null since the type that
# does the same for it.
#
# THE THIRD MEMBER, `console', WAS KEPT FOR ONE DRAFT AND THEN DROPPED, and
# the reason is worth more than the case was.  Rewritten as a single
# assertion it STILL SAT AFTER THE rm -- so it could only ever be true, which
# is the identical defect, reproduced by the person diagnosing it, inside the
# fix.  Mutation caught it: creating rootfs/dev/console changed nothing, and
# the snapshot below then faithfully restored the litter.  Moving it before
# the rm would make it checkable, but it would assert a gap nobody decided --
# this port does not build /dev/console by accident rather than on purpose,
# and nothing in the tree opens it.  An assertion about an absent thing with
# no consumer is the unconsumed-component rule pointed at a test.  Task #78's
# whole lesson is that /dev gaps get discovered when something finally needs
# them; that is the moment to decide, and a case written now would only
# freeze the accident.

# ...and put /dev back exactly as the build left it.  ps(1) calls error() and
# exits if /dev/dk, /dev/pt or /dev/drum is missing (ps.c:21-28), so the ps
# section at the end of this file would otherwise fail for the wrong reason --
# but the restore is not FOR ps, it is for the tree, and the snapshot covers
# whatever the build owns without this file having to know what that is.
# /dev/kmem and /unix are deliberately left as the manufacturer just made them.
for n in $(ls "$TMP/devsave"); do
	[ -e "$V8ROOT/dev/$n" ] || cp -a "$TMP/devsave/$n" "$V8ROOT/dev/$n"
done
# And say so, because a restore that silently does nothing is how this got
# lost in the first place.
[ -e "$V8ROOT/dev/null" ] && [ -d "$V8ROOT/dev/fd" ] && ok ||
	bad "the build's own /dev nodes are restored after the rm" \
	    "null=$([ -e "$V8ROOT/dev/null" ] && echo y || echo n) fd=$([ -d "$V8ROOT/dev/fd" ] && echo y || echo n)"

check "load prints V8's header" "    1m    5m   15m" "$(head -1 "$TMP/load.out")"

# The numbers are the host's -- but the load average is a MOVING TARGET, so an
# equality check against a sysctl taken at a different instant is flaky, and a
# flaky test is worse than no test. Seen failing exactly that way: 3.9 against
# 4.0, one sample apart.
#
# So bracket it. Sample sysctl immediately before and after a fresh run, and
# require each of V8's three numbers to lie inside that interval, widened by the
# 0.05 that rounding to one decimal can hide. That asserts what is actually
# true -- V8 read the same quantity the host did, over the same moment -- rather
# than asserting the machine was idle enough for two samples to agree.
before=$(sysctl -n vm.loadavg | tr -d '{}')
"$LOAD" > "$TMP/load2.out" 2>/dev/null
after=$(sysctl -n vm.loadavg | tr -d '{}')
v8la=$(sed -n '2p' "$TMP/load2.out")
verdict=$(awk -v b="$before" -v a="$after" -v v="$v8la" 'BEGIN {
	split(b, B); split(a, A); n = split(v, V);
	if (n != 3) { print "load printed " n " numbers, not 3"; exit }
	# 0.1 -- one full unit of the precision V8 prints with %6.1f, not the
	# 0.05 that rounding alone would suggest. 0.05 was tried and was ITSELF
	# flaky: 2.45 - 0.05 is 2.4000000000000004 in binary, so a printed 2.4
	# fell outside a bracket it was exactly on the edge of. The margin has to
	# be bigger than the arithmetic noise in computing the margin.
	#
	# Still a real check. What it must catch -- a wrong symbol address, a
	# wrong fscale, the wrong endianness -- is off by orders of magnitude,
	# not by a tenth.
	for (i = 1; i <= 3; i++) {
		lo = (B[i] < A[i] ? B[i] : A[i]) - 0.1;
		hi = (B[i] > A[i] ? B[i] : A[i]) + 0.1;
		if (V[i] < lo || V[i] > hi) {
			printf "field %d: %s outside [%.2f,%.2f]\n", i, V[i], lo, hi;
			exit
		}
	}
	print "ok"
}')
check "load's three averages track the host's" "ok" "$verdict"

# nlist(3) is authentic V8 libc reading a file this port writes, so the two ends
# must agree about a.out. Under LP64 these are NOT their 1985 sizes -- every
# field of struct exec is a long, so the header is 64 bytes where the VAX had
# 32. Asserted from the V8 side, because that is the end that must be right.
cat > "$TMP/aout.c" <<'EOF'
#include <stdio.h>
#include <a.out.h>
main()
{
	struct exec e;
	struct nlist n;
	printf("%d %d %d\n", sizeof e, sizeof n,
	    (char *)&n.n_value - (char *)&n);
	return 0;
}
EOF
if "$CC" -c -o "$TMP/aout.o" "$TMP/aout.c" > "$TMP/aout.log" 2>&1 &&
   clang -nostdlib -e _v8start -o "$TMP/aout" "$CRT" "$TMP/aout.o" \
	"$LIBC" "$STUBS" "$SHIM" -lSystem >> "$TMP/aout.log" 2>&1; then
	check "struct exec is 64 bytes and nlist 24, value at 16" \
		"64 24 16" "$("$TMP/aout")"
else bad "a.out layout probe build" "$(head -3 "$TMP/aout.log")"; fi

# The namelist is a real a.out: OMAGIC little-endian in the first eight bytes.
magic=$(od -An -N8 -tu8 "$V8ROOT/unix" | tr -d ' ')
check "/unix carries OMAGIC (0407)" "263" "$magic"

# /unix is an EXACT match in the jail, not a prefix. An entry without a trailing
# slash would also claim /unixfoo, which is the kind of rule that is wrong only
# for names nobody has created yet.
cat > "$TMP/px.c" <<'EOF'
#include <stdio.h>
main()
{
	int f = open("/unixfoo", 0);
	printf("%s\n", f < 0 ? "absent" : "FOUND");
	if (f >= 0) close(f);
	fflush(stdout); return 0;
}
EOF
: > "$V8ROOT/unixfoo"
if "$CC" -c -o "$TMP/px.o" "$TMP/px.c" > "$TMP/px.log" 2>&1 &&
   clang -nostdlib -e _v8start -o "$TMP/px" "$CRT" "$TMP/px.o" \
	"$LIBC" "$STUBS" "$SHIM" -lSystem >> "$TMP/px.log" 2>&1; then
	check "/unixfoo is not caught by the /unix entry" "absent" "$("$TMP/px")"
else bad "prefix probe build" "$(head -3 "$TMP/px.log")"; fi
rm -f "$V8ROOT/unixfoo"

# The manufactured kernel cannot be tampered with from outside, because it is
# rewritten on every open(2). Corrupting _avenrun's address to a wild value and
# getting the RIGHT answer back is the assertion -- the file the program read is
# not the file on disk a moment earlier.
#
# src/cmd/load/PORTING.md used to claim this suite mutated the namelist into
# lying and checked that the output went empty. It never did, and it could not:
# the mutation is undone by the very open that would have exercised it. The
# table lives in kmem.c, so the only way to make /unix lie is to rebuild the
# library, which is a build-time mutation and not something a suite can do.
# Corrected there; asserted here as the property that IS true.
before=$(od -An -tx8 -j80 -N8 "$V8ROOT/unix" | tr -d ' ')
printf '\x00\x90\x00\x00\x00\x00\x00\x00' |
	dd of="$V8ROOT/unix" bs=1 seek=80 conv=notrunc 2>/dev/null
mid=$(od -An -tx8 -j80 -N8 "$V8ROOT/unix" | tr -d ' ')
[ "$mid" != "$before" ] && ok || bad "the corruption did not take -- test is vacuous"
"$LOAD" > "$TMP/heal.out" 2>&1
after=$(od -An -tx8 -j80 -N8 "$V8ROOT/unix" | tr -d ' ')
check "a corrupted /unix is regenerated by the next open" "$before" "$after"
check "and the groveler still gets its header" \
	"    1m    5m   15m" "$(head -1 "$TMP/heal.out")"

# --------------------------------------------------------- w and uptime ---
# The fourth groveler, and TWO programs: upstream links /usr/bin/uptime to
# /usr/bin/w and w branches on argv[0]. Only the uptime half can run here --
# w's full path is 1981 Berkeley code that walks VAX page tables to find each
# process's u-area, and there is nothing here to walk.
W=$V8ROOT/usr/bin/w
UP=$V8ROOT/usr/bin/uptime

# One binary, two names, and `ln' rather than `cp' is what makes that true
# rather than merely described. Same inode, or the two can drift.
check "uptime is a hard link to w, not a copy" \
	"$(stat -f %i "$W")" "$(stat -f %i "$UP")"

# V8's format is the 1981 Berkeley one and deliberately not the host's: a
# 12-hour clock with am/pm, "1 users" with no plural logic, and commas between
# the three averages where macOS uses spaces.
upout=$("$UP" 2>&1); uprc=$?
check "uptime exits 0" "0" "$uprc"
echo "$upout" | grep -qE '^ *[0-9]+:[0-9][0-9](am|pm) +up .*[0-9]+ users,  load average: [0-9.]+, [0-9.]+, [0-9.]+$' &&
	ok || bad "uptime prints V8's format" "$upout"

# THE HONEST FAILURE, and it is an assertion rather than a known limitation:
# full w must say so in its own words. If this ever passes, something has
# manufactured a /dev/mem for VAX page tables to be walked in, and that is a
# decision to be argued rather than discovered.
werr=$("$W" 2>&1 >/dev/null); wrc=$?
check "w's full form refuses rather than inventing" "No mem" "$werr"
check "...and exits nonzero doing it" "1" "$wrc"

# -u is the same path reached by the flag instead of by argv[0]. It has to be
# checked separately: the argv[0] branch is what the hard link exercises, and
# this is what a user typing `w -u' gets.
wuout=$("$W" -u 2>&1); wurc=$?
check "w -u takes the uptime path too" "0" "$wurc"
echo "$wuout" | grep -q 'load average:' && ok || bad "w -u prints the uptime line" "$wuout"

# THE SENTINEL RULE, asserted from the V8 side through nlist(3) -- the same
# call w makes. w names nine symbols; two have exact answers from sysctl and
# seven describe a VAX proc table reached through VAX page tables. Those seven
# get NO row, so nlist leaves n_type zero and the program finds nothing rather
# than being handed a number. This is the check that fails if someone adds a
# plausible-looking _proc.
cat > "$TMP/nl.c" <<'EOF'
#include <stdio.h>
#include <nlist.h>
#include <sys/types.h>
struct nlist nl[] = {
	{ "_avenrun" }, { "_bootime" },
	{ "_proc" }, { "_nproc" }, { "_swapdev" }, { "_nswap" },
	{ "_ecmx" }, { "_Usrptmap" }, { "_usrpt" },
	{ 0 },
};
main()
{
	int i;
	nlist("/unix", nl);
	for (i = 0; nl[i].n_name && nl[i].n_name[0]; i++)
		printf("%s %s %lx\n", nl[i].n_name,
		    nl[i].n_type ? "found" : "absent", nl[i].n_value);
	printf("timet %d\n", sizeof(time_t));
	fflush(stdout);
	return 0;
}
EOF
if "$CC" -c -o "$TMP/nl.o" "$TMP/nl.c" > "$TMP/nl.log" 2>&1 &&
   clang -nostdlib -e _v8start -o "$TMP/nl" "$CRT" "$TMP/nl.o" \
	"$LIBC" "$STUBS" "$SHIM" -lSystem >> "$TMP/nl.log" 2>&1; then
	nlout=$("$TMP/nl")
	for s in _avenrun _bootime; do
		check "$s has a row, because sysctl answers for it" \
			"found" "$(echo "$nlout" | awk -v s=$s '$1==s {print $2}')"
	done
	for s in _proc _nproc _swapdev _nswap _ecmx _Usrptmap _usrpt; do
		check "$s has NO row, because nothing here can answer for it" \
			"absent" "$(echo "$nlout" | awk -v s=$s '$1==s {print $2}')"
	done

	# kmem.c sizes the _bootime row as sizeof(long) rather than the VAX's 4.
	# The two ends must agree or the uptime is plausible and wrong.
	check "a V8 time_t is 8 bytes, which is what the row is sized for" \
		"8" "$(echo "$nlout" | awk '$1=="timet" {print $2}')"

	# AND THE VALUE IS TRUE. Boot time does not move, so unlike the load
	# average this can be an exact comparison: read the eight bytes at the
	# address the namelist gives and require them to equal kern.boottime.
	# A wrong address, a wrong width or a wrong endianness all fail here.
	addr=$(echo "$nlout" | awk '$1=="_bootime" {print $3}')
	off=$(printf '%d' "0x$addr")
	kmemsec=$(od -An -tu8 -j "$off" -N8 "$V8ROOT/dev/kmem" | tr -d ' ')
	# ANCHORED on the leading brace. `.*sec = ' is greedy and matches through
	# to `usec = ', so the unanchored spelling silently captures the
	# microseconds -- which is a number, and looks like an answer.
	hostsec=$(sysctl -n kern.boottime | sed -n 's/^{ sec = \([0-9]*\).*/\1/p')
	check "the _bootime in /dev/kmem is the host's kern.boottime" \
		"$hostsec" "$kmemsec"
else bad "namelist probe build" "$(head -3 "$TMP/nl.log")"; fi

check "w imports libkmemu's whole surface and no more" \
	"$KMEMU_IMPORTS" "$(libcimports "$W")"

# ------------------------------------------------------------- /proc ---
# Killian's process filesystem, and the SECOND filesystem type in the shim's
# switch.  Not proca.c: PLAN.md section 8a step 3 records why importing it
# means importing the kernel.  The CONVENTIONS are proca.c's, so they are what
# gets asserted -- every one is cited in shim/libkmemu/procfs.c.
cat > "$TMP/pr.c" <<'EOF'
#include <stdio.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/dir.h>
main()
{
	struct stat sb;
	struct direct d;
	int fd, live = 0, dots = 0, fivedig = 1, nonzero = 1, self;
	char me[16];

	if (stat("/proc", &sb) < 0) { printf("nostat\n"); return 1; }
	printf("mode %o\n", sb.st_mode & 0170000);
	printf("rootino %d\n", sb.st_ino);
	printf("dirsize %ld\n", (long)sb.st_size);

	if ((fd = open("/proc", 0)) < 0) { printf("noopen\n"); return 1; }
	while (read(fd, (char *)&d, sizeof d) == sizeof d) {
		int i;
		if (d.d_ino == 0) continue;
		if (d.d_name[0] == '.') { dots++; continue; }
		live++;
		for (i = 0; i < 5; i++)
			if (d.d_name[i] < '0' || d.d_name[i] > '9') fivedig = 0;
		if (d.d_ino == 0) nonzero = 0;
	}
	close(fd);
	printf("dots %d\n", dots);
	printf("live %d\n", live);
	printf("fivedigit %d\n", fivedig);
	printf("nonzero %d\n", nonzero);

	/* our own entry must be openable, and a pid that cannot exist must not */
	self = getpid();
	sprintf(me, "/proc/%05d", self);
	fd = open(me, 0);
	printf("self %d\n", fd >= 0);
	if (fd >= 0) close(fd);
	printf("bogus %d\n", open("/proc/00000", 0) < 0);
	printf("notdigits %d\n", open("/proc/abc", 0) < 0);
	fflush(stdout);
	return 0;
}
EOF
if "$CC" -c -o "$TMP/pr.o" "$TMP/pr.c" > "$TMP/pr.log" 2>&1 &&
   clang -nostdlib -e _v8start -o "$TMP/pr" "$CRT" "$TMP/pr.o" \
	-Wl,-force_load,"$KMEMU" "$LIBC" "$STUBS" "$SHIM" -lSystem >> "$TMP/pr.log" 2>&1; then
	out=$("$TMP/pr" 2>/dev/null)
	g() { echo "$out" | awk -v k="$1" '$1==k {print $2}'; }
	check "/proc is a directory"            "40000" "$(g mode)"
	# ROOTINO is 2, "i number of all roots" (h/param.h:73); /proc reuses it
	# for its own directory, and both . and .. point at it (proca.c:109-110).
	check "...at ROOTINO, and . and .. both point there" "2" "$(g rootino)"
	check "both dot entries are present"    "2" "$(g dots)"
	# (nproc + 2) records of 256 bytes: a FIXED-size directory with holes,
	# which is proca.c:71's shape.  256 and not 16 because this port's DIRSIZ
	# is 254 -- /proc must speak the same dialect as every other directory.
	check "size is (nproc + 2) records"     "262656" "$(g dirsize)"
	# ...and the same number read as the table size, for prtrack above.
	prdir=$(g dirsize)
	case $prdir in
	''|*[!0-9]*) prnproc= ;;
	*)           prnproc=$((prdir / 256 - 2)) ;;
	esac
	check "names are five zero-padded digits" "1" "$(g fivedigit)"
	check "no live entry has inode 0"       "1" "$(g nonzero)"
	check "a process can open its own entry" "1" "$(g self)"
	check "pid 0 has no entry"              "1" "$(g bogus)"
	check "a non-numeric name is refused"   "1" "$(g notdigits)"

	# The count must track the host's -- or saturate, above the table.
	# Bracketed rather than equal because processes come and go between the
	# two samples, and a flaky test is worse than none.
	hostn=$(ps ax | wc -l | tr -d ' ')
	v8n=$(g live)
	prtrack "/proc lists roughly what ps ax does" "$hostn" "$v8n" "$prnproc"
else bad "/proc probe build" "$(head -3 "$TMP/pr.log")"; fi

# --- PIOCGETPR, and the struct it copies out -----------------------------
# prioctl answers this with iomove of the kernel's own proc slot, verbatim
# (proca.c:323), so struct proc's SHAPE IS THE ABI -- there is no marshalling
# step that could absorb a disagreement between the two compilers.  procfs.c
# _Static_asserts the offsets on the clang side, which says nothing about
# whether v8cc agrees; this is that half.
cat > "$TMP/gp.c" <<'EOF'
#include <stdio.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/dir.h>
#include <sys/user.h>
#include <sys/proc.h>
#include <sys/pioctl.h>
#include <sys/ioctl.h>
struct proc p;
extern int errno;
long sink;		/* global, because v8cc is 1985 C and has no `volatile' */
#define OFF(f)	((long)((char *)&p.f - (char *)&p))
main(argc, argv)
int argc; char **argv;
{
	int fd, dir, pct;
	char me[16];

	printf("size %ld\n", (long)sizeof(struct proc));
	printf("offs %ld %ld %ld %ld %ld %ld %ld %ld %ld\n",
	    OFF(p_stat), OFF(p_nice), OFF(p_flag), OFF(p_uid), OFF(p_pid),
	    OFF(p_ppid), OFF(p_dsize), OFF(p_clktim), OFF(p_pctcpu));

	sprintf(me, "/proc/%05d", getpid());
	if ((fd = open(me, 0)) < 0) { printf("noopen\n"); return 1; }
	if (ioctl(fd, PIOCGETPR, &p) < 0) { printf("noioctl\n"); return 1; }

	/*
	 * The width is asserted separately from the value, because the value
	 * only catches the bug ABOVE PID 32767 -- and on a freshly booted host
	 * every pid is below it, so the equality below would pass all morning
	 * with a 16-bit field and start lying after lunch.  This is how the
	 * truncation got in: it was invisible until the host's pid counter
	 * happened to be high enough.
	 */
	printf("pidwidth %d\n", (int)sizeof(p.p_pid));
	printf("pid %d\n",  p.p_pid  == getpid());
	printf("rawpid %d\n", getpid());
	printf("ppid %d\n", p.p_ppid == getppid());
	printf("uid %d\n",  p.p_uid  == getuid());
	printf("load %d\n", (p.p_flag & SLOAD) != 0);
	printf("stat %d\n", p.p_stat == SRUN);
	printf("rss %d\n",  p.p_rssize > 0);
	/*
	 * NOT `p_nice == NZERO'.  That asserts the HOST's baseline nice is 0,
	 * which is a property of the machine and not of this port -- it held on
	 * the development Mac and failed on the CI runner, whose jobs start
	 * renice'd.  What the bias actually has to do is TRACK, so the shell
	 * hands us a child started with `nice -n 10' and we report both values;
	 * the difference is 10 whatever the baseline is.
	 */
	printf("nice %d\n", p.p_nice);
	printf("nzero %d\n", NZERO);
	if (argc > 1) {
		int kid = atoi(argv[1]);
		int kfd;
		char kn[16];
		sprintf(kn, "/proc/%05d", kid);
		if ((kfd = open(kn, 0)) >= 0 &&
		    ioctl(kfd, PIOCGETPR, &p) >= 0)
			printf("kidnice %d\n", p.p_nice);
		else
			printf("kidnice X\n");
		if (kfd >= 0) close(kfd);
		if (ioctl(fd, PIOCGETPR, &p) < 0) { printf("nereread\n"); return 1; }
	}

	/*
	 * %cpu, AND THE POINT IS THE MAGNITUDE.  "Between 0 and 100" would pass
	 * with the tick rate wrong by 41.67x, which is precisely the mistake
	 * procfs.c's prtickhz() exists to avoid -- pti_total_user is in mach
	 * ticks, not the nanoseconds the header's "total time" suggests.  So
	 * burn two seconds of cpu in a process two seconds old and demand a
	 * number near 100: reading ticks as nanoseconds gives 2 instead.
	 *
	 * Self-calibrating rather than machine-timed -- it spins until the
	 * clock says two seconds have passed, so a slow host burns just as much
	 * cpu as a fast one and the ratio is the same on both.
	 */
	{
		long end = time((long *)0) + 2, i;
		while (time((long *)0) < end)
			for (i = 0; i < 200000; i++) sink += i;
	}
	if (ioctl(fd, PIOCGETPR, &p) < 0) { printf("noioctl2\n"); return 1; }
	/* the float crosses the seam too; keep it out of varargs to test the
	 * struct member rather than v8cc's argument passing for floats. */
	pct = (int)(100.0 * p.p_pctcpu);
	printf("pct %d\n", pct);

	/*
	 * THE SAME COMMAND, TWO DESCRIPTORS, TWO PATHS.  This pair is what says
	 * the dispatch is by filesystem and not by command number: PIOCGETPR on
	 * an ordinary file must never reach /proc's handler, and a terminal
	 * command on a /proc descriptor must never reach termios.
	 */
	dir = open("/etc/passwd", 0);
	printf("pass %d\n", ioctl(dir, PIOCGETPR, &p) < 0 && errno == 25);
	close(dir);
	printf("tioc %d\n", ioctl(fd, TIOCGETP, &p) < 0 && errno == 22);
	close(fd);

	/* prioctl calls pfind(i_number - PRMAGIC) before looking at the command
	 * (proca.c:312), and on the directory that is pfind(2 - 64). */
	dir = open("/proc", 0);
	printf("dir %d\n", ioctl(dir, PIOCGETPR, &p) < 0 && errno == 2);
	close(dir);
	fflush(stdout);
	return 0;
}
EOF
if "$CC" -c -o "$TMP/gp.o" "$TMP/gp.c" > "$TMP/gp.log" 2>&1 &&
   clang -nostdlib -e _v8start -o "$TMP/gp" "$CRT" "$TMP/gp.o" \
	-Wl,-force_load,"$KMEMU" "$LIBC" "$STUBS" "$SHIM" -lSystem >> "$TMP/gp.log" 2>&1; then
	# A child ten nicer than us, so the bias below can be checked as a
	# DIFFERENCE rather than against an assumed baseline of 0. nice(1) here
	# is the host's -- the shell arranging the world, not part of the test.
	nice -n 10 sleep 30 >/dev/null 2>&1 & kidpid=$!
	out=$("$TMP/gp" "$kidpid" 2>/dev/null)
	# ...asked BEFORE the kill, or ps has nothing left to report on.
	hostkid=$(ps -o nice= -p "$kidpid" 2>/dev/null | tr -d ' ')
	hostself=$(ps -o nice= -p $$ 2>/dev/null | tr -d ' ')
	kill "$kidpid" 2>/dev/null
	wait "$kidpid" 2>/dev/null
	g() { echo "$out" | awk -v k="$1" '$1==k {$1=""; sub(/^ /,""); print}'; }
	# 208 and not upstream's 200: the four pid-shaped fields are int here,
	# because a macOS pid does not fit in a short. src/include/PORTING.md.
	check "v8cc agrees struct proc is 208 bytes" "208" "$(g size)"
	check "...and on every offset PIOCGETPR's readers use" \
		"27 29 56 60 68 72 88 152 180" "$(g offs)"
	check "p_pid is 32 bits, at any pid the host happens to be at" \
		"4" "$(g pidwidth)"
	[ "$(g pid)" = 1 ] && ok ||
		bad "PIOCGETPR reports our own pid" "pid was $(g rawpid)"
	check "...our parent"                          "1" "$(g ppid)"
	check "...our uid"                             "1" "$(g uid)"
	# SLOAD is not decoration: without it ps's getuarea reads /dev/drum.
	check "...SLOAD, so ps reads the u-area from /proc" "1" "$(g load)"
	# macOS SRUN is 2 and V8's is 3; a straight copy would print 'w'.
	check "...SRUN translated, not copied"         "1" "$(g stat)"
	check "...a resident set size"                 "1" "$(g rss)"
	# V8's nice is 0..39 about NZERO, macOS's -20..19 about 0, so the shim
	# adds NZERO. Checked as a DIFFERENCE against a child started ten nicer,
	# because the absolute value depends on the host's baseline -- asserting
	# NZERO exactly held on the development Mac and failed on CI, whose jobs
	# start renice'd. A test that passes on one machine and fails on another
	# is testing the machine.
	#
	# NICE, CHECKED AGAINST THE HOST'S OWN ANSWER, and this took three tries
	# to get right because each earlier form asserted something about the
	# machine.  `p_nice == NZERO' assumed a baseline of 0 and failed on a
	# renice'd CI runner.  A DIFFERENCE against a ten-nicer child fixed that
	# and then failed too, because PROC_PIDTBSDINFO does not answer on the
	# runner at all -- and the shim's documented behaviour when the host will
	# not say is exactly NZERO, which a difference reads as zero.
	#
	# Comparing the shim's value against `ps -o nice=' looked like the fix and
	# is not, because the two host interfaces do not always agree: on the
	# runner ps reports the child at 0 while proc_pidinfo implies -10, and
	# from outside the shim there is no way to tell "the shim is wrong" from
	# "the host contradicts itself".  An absolute value cannot be the test.
	#
	# So: assert a RELATION the port controls, and only where the host has
	# supplied the precondition for it.  If ps says the two processes really
	# are ten apart, the shim's two values must be ten apart too (scale and
	# sign) and the nicer one must earn printp's N (the bias itself, which a
	# difference cannot see).  If ps says they are not, nothing here is
	# exercised and the run says so rather than asserting into fog.
	# ...AND ONLY WHERE proc_pidinfo AGREES THAT IT HAPPENED.  It does not
	# always: on a GitHub runner `ps' (which reads sysctl kern.proc) sees the
	# child ten nicer while pbi_nice reports both the same, and on another
	# run pbi_nice implied -10 for a child ps called 0.  The two host
	# interfaces disagree about nice on that machine, and the shim reads the
	# one that is wrong there.  That is a recorded limitation of the port --
	# src/cmd/ps/PORTING.md -- not something this test can assert through, so
	# where they disagree the run says so and prints both.
	nzero=$(g nzero); mynice=$(g nice); kidnice=$(g kidnice)
	nicelive=no
	if [ -z "$nzero" ] || [ -z "$kidnice" ] || [ "$kidnice" = "X" ]; then
		bad "could not read the nice values" "self=$mynice kid=$kidnice"
	elif [ -z "$hostkid" ] || [ -z "$hostself" ] ||
	     [ "$((hostkid - hostself))" -ne 10 ]; then
		skip 1 "nice -n 10 gave the host no difference to" \
		       "report -- ps says self=$hostself kid=$hostkid"
	elif [ "$((kidnice - mynice))" -eq 10 ]; then
		ok
		# The MARKER needs a stronger precondition than the difference
		# does, and they are not the same question. printp tests
		# `p_nice > NZERO', strictly -- so the child has to end up above
		# the host's zero, not merely ten above its parent. A runner
		# whose baseline is -10 puts a `nice -n 10' child at exactly 0,
		# which maps to NZERO, and 20 > 20 is false: no marker, and that
		# is correct behaviour rather than a bug.
		[ "$hostkid" -gt 0 ] && nicelive=yes
	elif [ "$((kidnice - mynice))" -eq 0 ]; then
		skip 1 "ps sees the renice and proc_pidinfo does" \
		       "not -- the host's two interfaces disagree, shim self=$mynice" \
		       "kid=$kidnice. src/cmd/ps/PORTING.md"
	else
		bad "...nice tracks the host's, ten apart" \
		    "ps self=$hostself kid=$hostkid; shim self=$mynice kid=$kidnice"
	fi
	# Ticks read as nanoseconds would give 2 here, not ~100 -- so the floor
	# only has to separate those two, and 40 was asserting something else.
	# procfs.c computes p_pctcpu as cpu-time over lifetime, which is the
	# fraction of a core the SCHEDULER actually handed the spinner; an
	# oversubscribed VM or a contended runner can legitimately deliver under
	# half a core and fail a check about arithmetic. 10 keeps every bit of
	# the discriminating power and stops claiming the machine was idle.
	pct=$(g pct)
	[ "$pct" -gt 10 ] && [ "$pct" -le 100 ] &&
		ok || bad "...a busy process reads a real %cpu (ticks, not ns)" "got $pct"
	check "PIOCGETPR on an ordinary file is ENOTTY" "1" "$(g pass)"
	check "a terminal command on /proc is EINVAL"  "1" "$(g tioc)"
	check "PIOCGETPR on /proc itself is ENOENT"    "1" "$(g dir)"
else bad "PIOCGETPR probe build" "$(head -3 "$TMP/gp.log")"; fi

# --- the u-area, which ps reads by seeking to a virtual address ----------
# /proc/<pid> IS the process's address space (proca.c serves it through
# prusrio), so the u-area is a REGION of that one file rather than a second
# format. This probe is ps's getuarea and getargs, spelled out: the same seek,
# the same read, the same sizes -- including the stack read that has to FAIL so
# that getargs takes its documented "(comm)" fallback.
cat > "$TMP/gu.c" <<'EOF'
#include <stdio.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/dir.h>
#include <sys/user.h>
#include <sys/proc.h>
#include <sys/pioctl.h>
#define SYSADR	0x80000000L
#define UBASE	(SYSADR-UPAGES*NBPG)
struct proc p;
union { struct user user; char chars[UPAGES*NBPG]; } usp;
#define u usp.user
main()
{
	int fd; long n; char me[16]; char stack[8192];
	struct stat sb;

	printf("usize %ld\n", (long)sizeof(struct user));
	printf("ubase %lx\n", (long)UBASE);	/* hex: it is a documented address */
	printf("uoffs %ld %ld %ld %ld %ld %ld %ld\n",
	    (long)((char *)&u.u_uid   - (char *)&u),
	    (long)((char *)&u.u_ruid  - (char *)&u),
	    (long)((char *)&u.u_procp - (char *)&u),
	    (long)((char *)u.u_comm   - (char *)&u),
	    (long)((char *)&u.u_start - (char *)&u),
	    (long)((char *)&u.u_ssize - (char *)&u),
	    (long)((char *)&u.u_vm.vm_utime - (char *)&u));

	sprintf(me, "/proc/%05d", getpid());
	if ((fd = open(me, 0)) < 0) { printf("noopen\n"); return 1; }
	ioctl(fd, PIOCGETPR, &p);

	/* getuarea, verbatim: Sread(fd, UBASE, up) */
	printf("seek %d\n", lseek(fd, UBASE, 0) == UBASE);
	n = read(fd, (char *)&u, sizeof(struct user));
	printf("read %d\n", n == sizeof(struct user));

	printf("procp %d\n", u.u_procp != 0);
	printf("uid %d\n",  u.u_uid  == getuid());
	printf("ruid %d\n", u.u_ruid == getuid());
	printf("comm %s\n", u.u_comm);
	printf("start %d\n", u.u_start > 1000000000L &&
	                     u.u_start <= time((long *)0));
	printf("times %d\n", u.u_vm.vm_utime >= 0 && u.u_vm.vm_stime >= 0);

	/*
	 * getargs, verbatim: seek to UBASE - ctob(u_ssize) and read it.  This
	 * MUST come up short -- there is no stack image -- because that is what
	 * sends getargs to "(u_comm)".  A zero u_ssize would make it a
	 * zero-length read, which succeeds, and getargs would then scan
	 * backwards past the start of its own buffer.
	 */
	n = ctob(u.u_ssize);
	printf("nstack %d\n", n == 8192);
	printf("staddr %d\n", lseek(fd, UBASE - n, 0) == UBASE - n);
	printf("stack %d\n", read(fd, stack, n) != n);

	/*
	 * THE EDGES OF THE REGION, which is where this shape of code breaks.
	 * One byte below UBASE must read as end of file and not as data: the
	 * shim indexes its buffer by (offset - UBASE), so a window that opened
	 * even slightly early would index it NEGATIVELY and hand out whatever
	 * precedes it.  And the last byte of the u-area must be readable while
	 * the one after it is not.
	 */
	printf("below %d\n", lseek(fd, UBASE - 1, 0) == UBASE - 1 &&
	    read(fd, stack, 16) == 0);
	printf("last %d\n", lseek(fd, UBASE + sizeof(struct user) - 1, 0) > 0 &&
	    read(fd, stack, 16) == 1);
	printf("past %d\n", lseek(fd, UBASE + sizeof(struct user), 0) > 0 &&
	    read(fd, stack, 16) == 0);

	/* proca.c:88 -- the file's size is the process image plus the u-area */
	printf("fstat %d\n", fstat(fd, &sb) == 0 && sb.st_size > UPAGES*NBPG);
	close(fd);
	fflush(stdout);
	return 0;
}
EOF
if "$CC" -c -o "$TMP/gu.o" "$TMP/gu.c" > "$TMP/gu.log" 2>&1 &&
   clang -nostdlib -e _v8start -o "$TMP/gu" "$CRT" "$TMP/gu.o" \
	-Wl,-force_load,"$KMEMU" "$LIBC" "$STUBS" "$SHIM" -lSystem >> "$TMP/gu.log" 2>&1; then
	out=$("$TMP/gu" 2>/dev/null)
	g() { echo "$out" | awk -v k="$1" '$1==k {$1=""; sub(/^ /,""); print}'; }
	check "v8cc agrees struct user is 4016 bytes" "4016" "$(g usize)"
	check "...and UBASE is 0x80000000 - UPAGES*NBPG" "7fffec00" "$(g ubase)"
	check "...and on every offset the u-area's readers use" \
		"282 286 296 2488 2744 2776 2784" "$(g uoffs)"
	check "a seek to UBASE lands there"        "1" "$(g seek)"
	check "...and reads a whole struct user"   "1" "$(g read)"
	# ps sets u_procp to 0 before the read and reads non-zero as "loaded".
	check "u_procp is non-zero, so ps caches the read" "1" "$(g procp)"
	check "the u-area reports our uid"         "1" "$(g uid)"
	check "...our real uid"                    "1" "$(g ruid)"
	check "...our command name"                "gu" "$(g comm)"
	check "...a start time in the past"        "1" "$(g start)"
	check "...cpu times that are not negative" "1" "$(g times)"
	# The one field that is a behavioural choice rather than a measurement.
	check "u_ssize is NSTACK's worth, so getargs reads once" "1" "$(g nstack)"
	check "...the stack address is seekable"   "1" "$(g staddr)"
	check "...and the stack read comes up short, as getargs needs" \
		"1" "$(g stack)"
	check "one byte below UBASE is EOF, not a negative index" "1" "$(g below)"
	check "...the last byte of the u-area is readable"  "1" "$(g last)"
	check "...and the byte after it is not"             "1" "$(g past)"
	check "the file's size is the image plus the u-area" "1" "$(g fstat)"
else bad "u-area probe build" "$(head -3 "$TMP/gu.log")"; fi

# THE NEGATIVE HALF, and it is the one that says the boundary is real.  /proc
# lives in libkmemu because it answers from libproc; a binary WITHOUT libkmemu
# gets noprocfs.c's null, no mount claims the path, and it falls through to the
# host -- where macOS has no /proc, so the program gets ENOENT rather than an
# empty directory it might believe.
cat > "$TMP/npr.c" <<'EOF'
#include <stdio.h>
main()
{
	int fd = open("/proc", 0);
	printf("%s\n", fd < 0 ? "absent" : "PRESENT");
	if (fd >= 0) close(fd);
	fflush(stdout); return 0;
}
EOF
if "$CC" -c -o "$TMP/npr.o" "$TMP/npr.c" > "$TMP/npr.log" 2>&1 &&
   clang -nostdlib -e _v8start -o "$TMP/npr" "$CRT" "$TMP/npr.o" \
	"$LIBC" "$STUBS" "$SHIM" -lSystem >> "$TMP/npr.log" 2>&1; then
	check "a binary without libkmemu has no /proc at all" \
		"absent" "$("$TMP/npr")"
else bad "no-/proc probe build" "$(head -3 "$TMP/npr.log")"; fi

# --- ...and NO other V8 binary imports anything at all -------------------
# EVERY binary in /bin, not a sample. This check is why the suite exists: it
# found three functions that had been resolving out of libSystem unnoticed --
# ftime, which is a syscall the shim had simply never implemented; tolower,
# which V8's libc has in C; and getgrent, which meant `ls -g' was reading the
# MAC's group database from inside the jail. None of them made anything look
# broken. tests/freestanding could not have caught them: it links its own small
# programs, so it only ever proved the shim was clean, never the world built
# on it.
#
# EVERY Mach-O in the rootfs, not just /bin: ccom, cpp, troff and refer's four
# helpers all install elsewhere, and a sweep that stops at /bin would have
# declared the world clean while missing a third of it.
#
# NO ALLOWED LEAKS. The list is empty, and both entries it used to hold came
# off the same way: someone built the V8 code that made the leak unnecessary,
# and this check refused to let the entry linger afterwards.
#
# libm was the last one, and the note beside it was wrong in a way worth
# keeping. It said Apple's math was "non-variadic, so it works and nothing
# looked wrong". It did not work. v8cc passes every argument positionally in
# x0-x7 INCLUDING doubles --
#
#	ldr d16, [x9] ; str d16, [sp,#384] ; ldr x0, [sp,#384] ; bl _sqrt
#
# -- and AAPCS64 puts a double argument in d0, so Apple's sqrt read whatever was
# there. Measured: sqrt(2.0) returned 0.000000 and pic rejected `circle rad 0.5'
# as "invalid radius 0.000000". Every drawing pic or grap made here was
# geometrically wrong. There is also no libm in V8's tree to port -- the math is
# in libc/math, which is why "port libm" was the wrong question. All eighteen
# files compile unmodified and put both ends of the call on one convention.
#
# The lesson for the next entry: "non-variadic so it is compatible" is an
# argument about the SHAPE of the call, and it is only as good as the register
# classes agreeing. They do not for floating point.
#
# `sleep' used to be the other entry, and the mechanism below is what took it
# off. V8's sleep(3) is alarm + a handler + `for(;;) pause()', and no V8 program
# in this port could catch a signal: v8s_signal handed the raw sigaction syscall
# a userland `struct sigaction' where the kernel wants `struct __sigaction',
# whose signal-trampoline pointer sits at offset 8 -- exactly where the userland
# struct keeps sa_mask. Every handler was installed with a null trampoline. With
# shim/v8sys/sigtramp.s in place, V8's own sleep.c builds and works, so nothing
# imports the host's any more; the staleness check below is what said so rather
# than anyone remembering to look.
ALLOWED=""

# ccom and cpp in the rootfs are the CLANG-BUILT stage-0 ones -- CLAUDE.md says
# so, and `nm' showing zero V8 symbols is how it says it. They import all of
# libc by construction and are not evidence of anything; replacing them with the
# self-hosted binaries is open Phase 6 work. Exempt by name, not by pattern, so
# that a third one appearing is a failure rather than a silence.
NOTV8="ccom cpp"

machos() {
	find "$V8ROOT" -type f -perm -u+x 2>/dev/null | while read -r f; do
		[ "$(od -An -N4 -tx1 "$f" | tr -d ' ')" = "cffaedfe" ] && echo "$f"
	done
}
allbins=$(machos)
n=$(echo "$allbins" | wc -l | tr -d ' ')
[ "$n" -gt 60 ] && ok || bad "only $n binaries found to sweep -- the world is not built"

# ...and the exemption is checked from the other side: these two must STILL be
# host-built. The day they are replaced by self-hosted ones they stop importing
# libc, and this fails so the exemption gets deleted with the reason for it.
stillhost=""
for b in $NOTV8; do
	f=$(echo "$allbins" | grep "/$b\$" | head -1)
	[ -n "$f" ] && [ -n "$(libcimports "$f")" ] || stillhost="$stillhost $b"
done
check "ccom and cpp are still the clang-built ones" "" "$stillhost"

leaked="" used=""
for b in $allbins; do
	# The grovelers link libkmemu on purpose and are checked against
	# KMEMU_IMPORTS above, by name and exactly -- skipped here, not exempt.
	case " who df load w uptime ps $NOTV8 " in *" ${b##*/} "*) continue ;; esac
	imp=$(libcimports "$b")
	for a in $ALLOWED; do
		case " $imp " in
		*" $a "*) used="$used $a"
		          imp=$(echo " $imp " | sed "s/ $a / /;s/^ *//;s/ *$//") ;;
		esac
	done
	[ -z "$imp" ] || leaked="$leaked ${b##*/}:[$imp]"
done
check "nothing but the named exception is taken from libc" "" "$leaked"

# ...and the exception is not stale. This is the half that works: when signal
# delivery was fixed and V8's own sleep.c came back, nothing imported the host's
# sleep any more and this failed, which is what prompted the entry's deletion.
for a in $ALLOWED; do
	case " $used " in
	*" $a "*) ok ;;
	*) bad "allowed-leak '$a' is no longer imported by anything -- drop it" ;;
	esac
done

# --- the OTHER half of that class, which nm -u structurally cannot see ---
#
# Everything above asks what a binary IMPORTS.  A symbol collision is about
# what an archive DEFINES, so none of it can see two archives defining the same
# name -- and the import that §8a step 5 is surveyed for brings sys/alloc.c,
# whose `free(dev, bno)' meets src/libc/gen/malloc.c's `free(ap)'.  Measured:
# with both members pulled the link says "duplicate symbol"; with only the
# kernel one pulled it is SILENT, and the block allocator gets handed a heap
# pointer and reads a superblock out of it.
#
# A blanket "no archive may define a libSystem name" is impossible -- libv8c.a
# overlaps libSystem on 153 of its 196 names, which is what being a libc means.
# So the assertion is PAIRWISE between our own archives.
adefs() { nm -g "$1" 2>/dev/null | awk '$2 ~ /^[TDBSC]$/ {print substr($3,2)}' | sort -u; }
KERNA=$ROOT/build/stage0/kern/libv8kern.a
LIBCA=$ROOT/build/stage0/libc/libv8c.a
SYSA=$ROOT/build/stage0/v8sys/libv8sys.a
# FIVE ARCHIVES, NOT THREE -- AND THE FIRST CORRECTION SAID FOUR, WHICH IS THE
# JOKE THIS BLOCK IS ABOUT.
#
# The sweep read kern, libc and sys.  libv8stubs.a was missing and it is the one
# holding the SYSCALL STUBS -- access(2), time(2), and every other name a V8
# program reaches the kernel by, i.e. exactly the names a kernel also defines.
# §8a step 5 found two collisions there (access, inverted in polarity from the
# kernel's, and time, a function against the kernel's clock VARIABLE) and this
# block could not have reported either, because it never opened the file.
#
# THEN THE FIX ITSELF WAS INCOMPLETE, in a sentence about completeness.  It
# said "four archives, not three" -- measured, the build produces FIVE, and the
# missed one is libkmemu.a.  Counted rather than recalled this time:
#
#	find build/stage0 -name '*.a'
#
# A pairwise sweep is only as good as its population, and the crash probe
# learned the identical lesson about $ROOT/usr/lib -- where the fix that added
# /etc and /usr/lib/refer STOPPED ONE DIRECTORY SHORT.  Same shape, and the
# second correction was as necessary as the first.
#
# libm.a is the sixth archive and is DELIBERATELY EXCLUDED.  It is this port's
# 216-byte reproduction of V8's own empty libm -- one member, one symbol, the
# name `_________'.  It would trip the vacuity check below for a reason that is
# the whole point of the file, and it defines nothing that can collide.
STUBA=$ROOT/build/stage0/v8sys/libv8stubs.a
KMEMUA=$ROOT/build/stage0/kmemu/libkmemu.a

# DUPOK IS NOW EMPTY, and the way it emptied is the point.
#
# It held max and min, explained rather than tolerated: src/libc/gen/min.c is
# `min(a,b) { return (a<b? a: b); }' with an implicit int return, while the
# kernel's is unsigned, which upstream's rdwri.c:249 (min; :235 is max's) and
# h/systm.h:61-62
# independently agree it should be.  Two different functions, one name, latent
# because nothing linked both archives.
#
# §8a step 5 made something link both, so it stopped being latent and got the
# psignal treatment instead -- shim/kern/h/param.h renames the kernel's to
# v8k_min and v8k_max along with six other names.  The staleness check below
# then fired, which is the only reason this comment is being rewritten rather
# than quietly kept: an entry on an allow list is a CLAIM, and nothing else
# audits one.  Same as ALLOWED, which is also empty.
#
# IT IS NOT EMPTY, THOUGH -- opening the fourth archive put ONE name on it, and
# it is the one case in this sweep that is correct rather than tolerated.
#
# `errno' is a COMMON in both libv8c.a (perror.o and every math object) and
# libv8stubs.a (errno.o).  Common against common is not a collision at all: the
# linker merges them into ONE four-byte object, which is exactly what a program
# must have -- a syscall stub setting errno and perror() reading it have to be
# talking about the same storage.  It is the K&R tentative-definition idiom
# doing its job, and it is why -fcommon is in the flag set.
#
# The distinction this sweep now draws is therefore three-way, not two:
#	T against T	a real collision; the linker will refuse
#	C against T	a SILENT collision; the common resolves to the text
#			symbol's address.  This is the class that needed
#			nm -g, and §8a step 5 found three (time, timezone,
#			mount), all renamed in shim/kern/h/param.h
#	C against C	deliberate sharing.  Only errno, and it is right.
#
# kmemu_procfs and kmemu_synth are T against T -- a REAL duplicate, and correct.
# shim/v8sys/noprocfs.c and nokmemu.c hold the fallback definitions and
# libkmemu.a the working ones; the two live in SEPARATE OBJECTS so that a link
# pulls only the one it needs.  noprocfs.c:10 quotes the duplicate-symbol error
# that forced that arrangement -- so the port already hit this and solved it,
# and what it never did was assert it.  Now a future merge of those objects
# into one file goes red instead of going quiet.
DUPOK="errno kmemu_procfs kmemu_synth"

if [ -f "$KERNA" ] && [ -f "$LIBCA" ] && [ -f "$SYSA" ] && [ -f "$STUBA" ]; then
	adefs "$KERNA" > "$TMP/d.kern"
	adefs "$LIBCA" > "$TMP/d.libc"
	adefs "$SYSA"  > "$TMP/d.sys"
	adefs "$STUBA" > "$TMP/d.stub"
	adefs "$KMEMUA" > "$TMP/d.kmemu"
	# every archive defines something, or the comm results below are
	# vacuously empty and this whole block passes while measuring nothing.
	# The floor is PER ARCHIVE because libkmemu is genuinely small -- ten
	# names, the sanctioned fact-readers and their two entry points -- and a
	# single threshold would either wave it through or fail it forever.
	for pf in kern:20 libc:20 sys:20 stub:20 kmemu:8; do
		p=${pf%:*}; floor=${pf#*:}
		[ "$(wc -l < "$TMP/d.$p" | tr -d ' ')" -ge "$floor" ] && ok ||
			bad "libv8$p defines almost nothing -- the sweep is vacuous"
	done
	dup=""
	for pair in "kern libc" "kern sys" "libc sys" \
	            "kern stub" "libc stub" "sys stub" \
	            "kern kmemu" "libc kmemu" "sys kmemu" "stub kmemu"; do
		set -- $pair
		for n in $(comm -12 "$TMP/d.$1" "$TMP/d.$2"); do
			case " $DUPOK " in *" $n "*) continue ;; esac
			dup="$dup $1/$2:$n"
		done
	done
	check "no two of our archives define the same name" "" "$dup"

	# ...and the known pair is not stale, the same way ALLOWED is checked.
	# ...and the known pair is not stale.  The staleness check now has to say
	# WHICH two archives, because DUPOK's one entry is a libc/stubs pair and
	# the old check only ever looked at kern/libc -- it would have passed
	# vacuously on a name that had stopped being duplicated anywhere.
	for n in $DUPOK; do
		c=0
		for p in kern libc sys stub kmemu; do
			grep -qx "$n" "$TMP/d.$p" && c=$((c + 1))
		done
		[ "$c" -ge 2 ] && ok ||
			bad "'$n' is no longer defined by two archives -- drop it from DUPOK"
	done
else
	bad "archives missing -- cannot sweep for duplicate definitions"
fi

# --- THE f77 RUNTIME IS A SIXTH AND SEVENTH ARCHIVE, AND ITS OVERLAP WITH ---
# --- libv8c IS DELIBERATE, SO IT GETS AN AIMED CASE RATHER THAN A DUPOK -----
#
# libF77.a and libI77.a collide with libv8c on six names.  They are NOT put on
# DUPOK, because DUPOK is global and an entry there would also wave the name
# through for kern/sys/stub -- an allow-list widened past what it was argued
# for, which is how tests/kmemu's own ALLOWED list went stale once already.
# These two archives are linked into a FORTRAN program and nothing else, so the
# claim about them is its own claim.
#
# WHY THE OVERLAP IS CORRECT.  f77's driver liblist is a fixed
# { "-lF77", "-lI77", "-lm", "-lc" } (drivedefs), and -lF77 -lI77 preceding -lc
# is what makes the Fortran versions win.  That is upstream's arrangement, not
# an accident: `cabs' is not even the same function on the two sides --
# libc/math/hypot.c declares cabs(struct complex) taking ONE struct by value
# where libF77/cabs.c declares cabs(double, double).  A Fortran program that got
# libc's would read its imaginary part out of the caller's second slot.
#
# WHY IT CANNOT FIRE, AND IT IS ONE NAME.  Archive semantics pull a member only
# to satisfy an undefined reference, so a doubly-defined symbol needs BOTH
# members pulled.  Measured, the four libv8c members holding these six names
# define almost nothing else:
#
#	hypot.o   cabs hypot        sinh.o   cosh sinh
#	tanh.o    tanh              ecvt.o   ecvt fcvt
#
# Every name there is one the f77 side also defines EXCEPT `hypot'.  So `hypot'
# is the entire hinge: reference it from a Fortran program and hypot.o comes in
# beside libF77's cabs.o, and the link fails on a duplicate `cabs'.  Nothing in
# the runtime does, and that is what the second case asserts.
#
# THE SET IS DERIVED EVERY RUN, not transcribed.  Three attempts at this count
# were wrong in three different ways, each instrument better than the last:
# comparing source FILENAMES against symbols gave 7 with 3 false positives and
# missed `cosh' (it lives in sinh.c); reading the definitions out of the source
# found cosh and missed `fcvt' (it lives in ecvt.c); nm on the built archives
# gives six.  Prefer the artefact.
F77A=$ROOT/build/stage0/libF77/libF77.a
I77A=$ROOT/build/stage0/libI77/libI77.a
if [ -f "$F77A" ] && [ -f "$I77A" ] && [ -f "$LIBCA" ]; then
	adefs "$F77A" > "$TMP/d.f77a"
	adefs "$I77A" > "$TMP/d.i77a"
	sort -u "$TMP/d.f77a" "$TMP/d.i77a" > "$TMP/d.f77"
	# Vacuity floor, for the reason the five above have one: an empty set
	# makes comm's answer empty and this whole block passes measuring nothing.
	[ "$(wc -l < "$TMP/d.f77" | tr -d ' ')" -ge 100 ] && ok ||
		bad "the f77 runtime defines almost nothing -- this sweep is vacuous"
	check "the f77 runtime overlaps libv8c in exactly the six upstream names" \
	    "cabs cosh ecvt fcvt sinh tanh" \
	    "$(comm -12 "$TMP/d.libc" "$TMP/d.f77" | tr '\n' ' ' | sed 's/ $//')"
	# The hinge.  If this ever reports a reference, the six above stop being
	# harmless and a Fortran link starts failing on a duplicate cabs.
	check "and nothing in it references hypot, which is what keeps both out" "" \
	    "$(nm -g "$F77A" "$I77A" 2>/dev/null |
	       awk '$1=="U"{print $2} $2=="U"{print $3}' |
	       grep -x '_hypot' | sort -u | tr '\n' ' ' | sed 's/ $//')"
else
	bad "the f77 runtime archives are missing -- cannot sweep them"
fi

# --- AND THE THIRD POPULATION, WHICH IS THE PROGRAM'S OWN OBJECTS --------
#
# The sweep above is archive against archive.  It cannot see a name that a
# PROGRAM defines and an archive also defines, and that is not a hypothetical
# gap: it is the measurement that decided §8a step 5e.
#
# Costing the mount began by linking libv8kern into cat, on the reading that a
# fourth filesystem type would need the kernel in the client.  ld said
#
#	tentative definition of '_buf' with size 4096 from bin/cat.o is being
#	replaced by real definition of smaller size 8 from libv8kern.a(main.o)
#
# -- cat.c:10 is `char buf[BLOCK]', BLOCK 4096, and shim/kern/sys/main.c:213 is
# `struct buf *buf = v8k_buftab'.  A K&R tentative definition is a COMMON, the
# kernel's is a real definition, and the linker is supposed to prefer the real
# one.  So cat's 4096-byte buffer became an eight-byte pointer.  Measured: the
# binary links with one warning, copies its input CORRECTLY, and then dies of
# SIGSEGV -- exit 139 -- which is mkdir's "a crash can happen after the work is
# done" arriving in the linker.
#
# Swept: 56 such names across 33 of this port's programs, and they are the
# 1985 vocabulary -- buf bread alloc bmap tty file bwrite getblk iput itrunc
# panic copyin copyout -- because the checkers reimplement the kernel's own
# algorithms under the kernel's own names.  Hiding libv8kern's globals behind
# `ld -r -exported_symbols_list' recovers 22 of the 27 and CANNOT recover the
# other five: ld will not make a common a private extern, and two commons merge
# silently by taking the larger size.
#
# Two assertions, because there are two separable claims.

# ONE: no V8 binary links libv8kern.  That is the step-5e decision, and the
# other half of its argument is not about symbols at all -- vfs.c:450 already
# said an in-process descriptor table "does not survive a program replacing
# itself", so `> /mnt/f' could never have worked in the client.  Both roads
# lead to a server, and this case is what notices if someone re-opens the
# in-process one without re-reading why it was closed.
#
# The witness is the v8k_ prefix: 30 names, defined by libv8kern and by no
# other archive here, measured rather than assumed by the vacuity check below.
if [ -f "$KERNA" ]; then
	adefs "$KERNA" | grep '^v8k_' > "$TMP/d.v8konly"
	nv8k=$(wc -l < "$TMP/d.v8konly" | tr -d ' ')
	[ "$nv8k" -ge 20 ] && ok ||
		bad "libv8kern exports almost no v8k_ names -- the witness is vacuous"
	other=""
	for p in libc sys stub kmemu; do
		grep -q '^v8k_' "$TMP/d.$p" && other="$other libv8$p"
	done
	check "only libv8kern defines the v8k_ names" "" "$other"

	# $allbins is the machos() list from the nm -u sweep above -- one walk of
	# the rootfs for the whole suite, and it already asserts its own floor.
	linked=""
	for f in $allbins; do
		nm -g "$f" 2>/dev/null |
		    awk '$2 ~ /^[TDSBC]$/ {print substr($3,2)}' |
		    grep -qxFf "$TMP/d.v8konly" && linked="$linked $(basename "$f")"
	done
	# 29 PROGRAMS, not 33 -- 33 is the object count, and the first draft of
	# this line said 33 while the word beside it said programs.  nroff and
	# troff each contribute two objects.
	check "no V8 binary links libv8kern (56 collisions, 29 programs)" "" "$linked"
fi

# TWO: the general class.  A program object's COMMON against an archive's TEXT
# is the same shape as cat's buf, and it exists in the live link lines too --
# swept, six of them today (od/max, dc/log10, mkfs/utime, nroff and troff/nlist,
# sh/tmpnam).  All six resolve the program's way, and only because nothing pulls
# the archive member in.  What would pull it in is -force_load, which this build
# ALREADY uses for libkmemu, so the hazard is one library-list edit away.
#
# So the assertion is on the artefact rather than on the pairing: in the built
# binary, a name the program declared as a common must live in program storage
# and not in __TEXT.  Derived every run -- a transcribed list of six would go
# stale the first time a program is imported.
for a in "$LIBCA" "$SYSA" "$STUBA" "$KMEMUA"; do
	nm -g "$a" 2>/dev/null | awk '$2 == "T" {print substr($3,2)}'
done | sort -u > "$TMP/arch.text"

[ -s "$TMP/arch.text" ] && ok || bad "no archive text symbols -- the C-vs-T sweep is vacuous"

# One walk of the rootfs, not one per object: `find' inside the loop below cost
# seven seconds of a five-second suite, which is the sort of thing that gets a
# guard deleted rather than fixed.
find "$V8ROOT" -type f ! -name '*.o' 2>/dev/null |
	awk -F/ '{print $NF, $0}' | sort -u -k1,1 > "$TMP/rootfs.index"

# ONE nm over every program object rather than one per object -- there are
# about 300 of them, and `nm -gA' prefixes each line with the file it came from,
# which is the whole reason to prefer it here.
find "$ROOT/build/stage0" -name '*.o' \
     ! -path '*/kern/*' ! -path '*/v8sys/*' \
     ! -path '*/libc/*' ! -path '*/kmemu/*' 2>/dev/null > "$TMP/prog.objs"
[ -s "$TMP/prog.objs" ] && ok || bad "no program objects found -- did the build run?"

xargs nm -gA < "$TMP/prog.objs" 2>/dev/null |
	awk 'NF == 4 && $3 == "C" { sub(/:$/, "", $1); print $1, substr($4, 2) }' |
	sort -u > "$TMP/prog.commons"

# ...and the intersection in one pass.  Sorted by object, so the nm -m below is
# done once per binary rather than once per pair.
awk 'NR == FNR { a[$1]; next } ($2 in a)' \
    "$TMP/arch.text" "$TMP/prog.commons" > "$TMP/ct.pairs"

ctpairs=0 ctskip=0 ctbad="" lastbin=""
while read -r o s; do
	d=$(basename "$(dirname "$o")"); b=$(basename "$o" .o)
	case "$d" in bin) name=$b ;; *) name=$d ;; esac
	bin=$(awk -v n="$name" '$1 == n {print $2; exit}' "$TMP/rootfs.index")
	if [ -z "$bin" ]; then ctskip=$((ctskip + 1)); continue; fi
	if [ "$bin" != "$lastbin" ]; then
		nm -m "$bin" 2>/dev/null > "$TMP/nm.bin"; lastbin=$bin
	fi
	line=$(grep -E "(^| )_$s\$" "$TMP/nm.bin" | head -1)
	case "$line" in
	'')	  ctskip=$((ctskip + 1)) ;;
	*__TEXT*) ctbad="$ctbad $name/$s" ;;
	*)	  ctpairs=$((ctpairs + 1)) ;;
	esac
done < "$TMP/ct.pairs"
# The pair count is printed rather than pinned: it moves with every import, and
# the crash probe's floor is the lesson about a number nobody wrote down.  What
# is asserted is that the sweep found SOMETHING to check and that none of it
# resolved into library text.
[ "$ctpairs" -ge 4 ] && ok ||
	bad "the common-vs-text sweep checked $ctpairs pairs -- too few to mean anything"
check "no program's common resolved into an archive's text" "" "$ctbad"
echo "  (common-vs-text: $ctpairs pairs checked, $ctskip not installed)"

# --- the stub really is what the others get ------------------------------
# Both halves define kmemu_synth, so `is it defined' proves nothing. What
# distinguishes them is that only one pulls libSystem in with it.
nm "$V8ROOT/bin/cat" 2>/dev/null | grep -q ' _kmemu_synth' && ok ||
	bad "cat links the do-nothing kmemu_synth from libv8sys"

# --- what the getgrent leak was hiding: whose /etc/group ----------------
# With getgrent coming from libSystem, this printed the Mac's groups from
# inside the jail -- and looked entirely plausible doing it, because a list of
# group names is a list of group names.
cat > "$TMP/grp.c" <<'EOF'
#include <stdio.h>
#include <grp.h>
main()
{
	struct group *g;
	struct group *getgrent();

	setgrent();
	while (g = getgrent()) printf("%s:%d\n", g->gr_name, g->gr_gid);
	endgrent();
	return 0;
}
EOF
if "$CC" -c -o "$TMP/grp.o" "$TMP/grp.c" > "$TMP/grp.log" 2>&1 &&
   clang -nostdlib -e _v8start -o "$TMP/grp" "$CRT" "$TMP/grp.o" \
	"$LIBC" "$STUBS" "$SHIM" -lSystem >> "$TMP/grp.log" 2>&1; then
	check "getgrent reads the jail's /etc/group, not the Mac's" \
		"$(awk -F: '{print $1 ":" $3}' "$V8ROOT/etc/group" | sort | tr '\n' ' ')" \
		"$("$TMP/grp" | sort | tr '\n' ' ')"
	# The decisive half: gid 0 is `wheel' on macOS and absent from the
	# jail's file, so finding it would mean the host database again.
	"$TMP/grp" | grep -q ':0$' && bad "gid 0 came from the host database" || ok
else bad "getgrent probe build" "$(head -3 "$TMP/grp.log")"; fi

# --- and what the ftime leak was hiding: whose timezone -----------------
# ftime(2) was missing from the shim entirely. tz.c implements it by reading
# the zone database, which is the piece that has to agree with the host or
# every timestamp in the world is silently offset.
# LC_ALL=C, because %a and %b render through LC_TIME and V8's date has no
# locale -- it indexes a compiled-in English table. The TIMEZONE is what this
# case is about, and a French host would fail it while agreeing perfectly about
# the zone. tests/wavea has the same three checks and the same note.
check "the shim's timezone agrees with the host" \
	"$(LC_ALL=C date '+%a %b %e %H:%M')" "$("$V8ROOT/bin/date" | cut -c1-16)"
check "ls -l timestamps agree with the host" \
	"$(LC_ALL=C ls -l "$V8ROOT/etc/group" | awk '{print $6, $7, $8}')" \
	"$("$V8ROOT/bin/ls" -l "$V8ROOT/etc" | awk '$NF == "group" {print $6, $7, $8}')"

# --- a path libkmemu does not know is left completely alone -------------
# The hook runs on EVERY open(2) in a groveler. If it ever manufactured a file
# for a path it was not asked about, it would be writing into the rootfs behind
# the program's back.
rm -f "$V8ROOT/etc/nosuchthing"
"$WHO" "$V8ROOT/etc/nosuchthing" > /dev/null 2>&1
[ -e "$V8ROOT/etc/nosuchthing" ] && bad "an unknown path was synthesised" || ok

# ------------------------------------------------------------------ ps ---
# The payoff for /proc, and the first program here that reads a FILESYSTEM this
# port implements rather than a file it manufactures.  Bell Labs' 1985 source,
# unmodified, listing macOS processes.
PS=$V8ROOT/bin/ps

# /dev/dk, /dev/pt and /dev/drum have to exist or ps error()s before doing
# anything (ps.c:21-28). The load section above removes all of /dev and puts
# those three back; tests/deps is what asserts the Makefile makes them.
# A renice'd child, so the nice marker has something to mark. Started before
# the listing so it is in it.
nice -n 10 sleep 30 >/dev/null 2>&1 & nicekid=$!
psout=$("$PS" hax 2>/dev/null)
psrc=$?
check "ps ax exits 0" "0" "$psrc"
echo "$psout" | head -1 | grep -q 'pid tty   stat  time command' &&
	ok || bad "ps -h prints V8's header" "$(echo "$psout" | head -1)"

# Bracketed rather than equal: processes come and go between the two samples.
# The saturated expectation carries a +1 that the /proc one does not, because
# `ps hax' prints V8's header and the line above asserts it -- so both sides of
# the bracket count one header each, and the table's 1024 entries come out as
# 1025 lines.  Measured: host 1218, v8 1025.
hostn=$(ps ax | wc -l | tr -d ' ')
v8n=$(echo "$psout" | wc -l | tr -d ' ')
prtrack "ps ax lists what the host's ps ax does" "$hostn" "$v8n" \
        "$((${prnproc:-0} + 1))"

# EVERY PID POSITIVE.  p_pid was a short and macOS pids reach 99998, so before
# src/include/sys/proc.h was patched every pid above 32767 printed NEGATIVE.
# This is the end-to-end form of that; the field-width assertion is above.
neg=$(echo "$psout" | tail -n +2 | awk '$1 <= 0' | head -3)
check "no process has a negative pid" "" "$neg"

# ...and the high ones are actually reached, or the check above proves nothing.
# ...and whether the high ones were actually reached, because if they were not
# the check above proves nothing. NOT a failure when they were not: a freshly
# booted host has low pids, which is the very property that let the 16-bit field
# survive in the first place, and a CI runner is always freshly booted. Reported
# so a green run does not silently claim coverage it did not have; the
# deterministic guard is the FIELD WIDTH assertion in the PIOCGETPR probe above,
# which holds at every pid.
himax=$(echo "$psout" | tail -n +2 | awk '{if ($1+0 > m) m = $1+0} END {print m+0}')
if [ "$himax" -gt 32767 ]; then ok
else skip 1 "highest pid here is $himax, inside 16 bits"; fi

# --- THE SAME CLASS FOR uid, AND ITS DETERMINISTIC GUARD IS A SWEEP ------
#
# A host uid is 32 bits and V8's u_uid and st_uid are `short', so a bare
# `(short)' cast maps every multiple of 65536 onto ZERO -- and zero is ROOT,
# the identity fio.c:193's access() bypasses and streamio.c:44 lets past an
# exclusive stream.  Measured: 65536 -> 0, 131072 -> 0.  shim/v8id.h is the one
# definition of the fold that avoids it.
#
# THIS HOST CANNOT REACH IT, exactly as it cannot reach a pid above 32767, and
# for the same reason a CI runner never will.  So the guard is not a value but
# the SOURCE: nothing in the shim may narrow an id with a cast again.  The
# arithmetic itself is asserted separately, over a table, in tests/v8sys --
# that is the analogue of the FIELD WIDTH assertion this pid block points at.
#
# THE INSTRUMENT MATCHES ITS OWN DOCUMENTATION, which is a trap this repo has a
# standing note about: v8id.h, syscall.c, procfs.c and fio.c all now DISCUSS the
# bare cast in prose, and a naive grep counts the explanation as an instance.
# Comment lines are excluded, and the count of what was excluded is printed, so
# a future reader can see the filter is doing something rather than hiding
# something.
idcast=$(grep -rnE '\(short\)[^;]*(uid|gid)' "$ROOT/shim" 2>/dev/null |
         grep -v '\.md:' | grep -vE ':[[:blank:]]*\*' | grep -vE ':[[:blank:]]*/\*')
[ -z "$idcast" ] && ok || bad "a host id is narrowed with a cast, not v8_foldid" "$idcast"
idprose=$(grep -rnE '\(short\)[^;]*(uid|gid)' "$ROOT/shim" 2>/dev/null |
          grep -v '\.md:' | wc -l | tr -d ' ')
echo "  (id-cast sweep: $idprose matches, all in comments)"
# ...and the fold is actually CALLED at each of the four narrowing sites, which
# the sweep above cannot see -- deleting a call and the cast together would
# leave it green.  Four files, and the count is derived rather than written
# down, so adding a fifth site is a decision rather than a silent omission.
#
# AND `.md' IS EXCLUDED HERE TOO, WHICH IT WAS NOT WHEN THIS WAS WRITTEN.  The
# case above carries a paragraph headed "THE INSTRUMENT MATCHES ITS OWN
# DOCUMENTATION" and excludes comment lines for exactly that reason; this line,
# added in the same edit, did not -- and went red the moment shim/NOTES.md
# gained a section describing the rule, reporting 5 files for 4 call sites.
# The fix landing on one line while the line beside it keeps the assumption,
# committed by the person who had just written the warning.
idfold=$(grep -rl "v8_foldid(" "$ROOT/shim" 2>/dev/null |
         grep -v '\.md$' | grep -v 'v8id\.h' | wc -l | tr -d ' ')
check "every component that narrows an id calls the shared fold" "4" "$idfold"

# ps must see ITSELF: the one process guaranteed to exist while it runs.
echo "$psout" | grep -q '(ps)' && ok || bad "ps lists itself"

# getargs cannot read a stack image here, so every command is V8's own
# swapped-out form -- "(name)".  Asserted so that the day a stack image exists
# this becomes a decision rather than a surprise.  See procfs.c's u_ssize note.
# Matched on the FORM, not a column index: the nice marker shifts the command
# one field right, so `$5' passed until the first renice'd process appeared.
bare=$(echo "$psout" | tail -n +2 | grep -v '([^)]*)$' | head -3)
check "every command reads as V8's swapped-out (comm) form" "" "$bare"

# Selecting one process by pid takes a different path through doselect.
#
# THE SUITE'S OWN PID, not whatever was third in /proc order.  This read
# `awk 'NR==2 {print $1}'' and then ran a SECOND ps against it, so an arbitrary
# system process exiting in the gap left $sel empty and the failure read as
# doselect being broken.  A rare flake that accuses the wrong code is worse than
# a common one.  $$ is the only pid in the run guaranteed alive at both calls,
# because it is the process doing the asking.
selp=$$
inlist=$(echo "$psout" | tail -n +2 | awk -v p="$selp" '$1 == p {print $1}')
if [ -n "$inlist" ]; then
	sel=$("$PS" "$selp" 2>/dev/null | awk '{print $1}')
	check "ps <pid> selects exactly that process" "$selp" "$sel"
else
	# ps lists every process, so the suite's own shell missing from it is a
	# finding rather than a reason to skip.
	bad "ps ax omits the running shell" "pid $selp not in the listing"
fi

# THE NICE MARKER, which is the only thing that can see the NZERO bias: printp
# prints " N"[p_nice > NZERO], so a renice'd process earns the N only if the
# bias is applied. Gated on `nicelive', set above when the host was willing to
# report nice at all -- where it is not (a GitHub runner), p_nice is NZERO for
# everything and no process can earn the marker, which is the shim's documented
# answer rather than a failure.
nmark=$(echo "$psout" | awk -v p="$nicekid" '$1 == p {print $4}')
kill "$nicekid" 2>/dev/null; wait "$nicekid" 2>/dev/null
if [ "$nicelive" = yes ]; then
	check "a renice'd process carries V8's N marker" "N" "$nmark"
else
	skip 1 "no process here ends up above the host's nice 0," \
	       "so none can be above NZERO and none earns the marker"
fi

# The time column comes from the u-area, which is the half PIOCGETPR does not
# carry -- so a non-zero time anywhere proves the u-area read reached ps.
#
# $4 IS NOT ALWAYS THE TIME, and the correction for that landed 25 lines above
# and not here.  printp.c prints state as `%-5.5s' built from the state letter
# plus an optional `W' and an optional `N', and awk collapses the run of
# spaces -- so a renice'd process shifts every later field right by one and $4
# is the marker:
#
#	986 ?     R N   0:12 (mdbulkimport)
#
# "N" != "0:00" is true, so a marker counts as a cpu time.  This suite starts
# `nice -n 10 sleep 30' before taking the listing, which guarantees at least one
# such row -- so the case could pass having read no cpu time at all, and would
# be a real check only on a host where nice does not apply.  Precisely the
# coverage it was meant to have, inverted.
nz=$(echo "$psout" | tail -n +2 |
     awk '{ t = $4; if (t == "N" || t == "W") t = $5; if (t != "0:00") print }' |
     wc -l | tr -d ' ')
[ "$nz" -gt 0 ] && ok || bad "some process has non-zero cpu time from the u-area"

# -T, THE START COLUMN, and the only ps option that had no case at all.  It
# SIGSEGV'd, and the cause was neither ps nor the u-area: printp.c:24 is
# `strcpy(sstr+4,ctime(&up->u_start)+4)' and ps.h was the one file in the tree
# that called ctime() without declaring it, so K&R gave it an implicit int
# return.  gencode.c deliberately does not narrow a signed-int CALL return --
# that is what makes undeclared malloc work -- so the pointer reached x0
# intact, and arm64_trunc() then sign-extended the `+4', which is correct for
# an int and fatal for a pointer.  Mach-O loads at 0x100000000, so the top half
# is never zero and the truncated address is always inside __PAGEZERO.
tout=$("$PS" haxT 2>/dev/null)
check "ps -T exits 0" "0" "$?"

# A RELATION, not a machine property: V8's START and the host's lstart are two
# views of one fact, so they must agree whatever the host's uptime or clock.
# $$ for the same reason the selection case uses it -- it is the only pid
# guaranteed alive at both samples.
selp=$$
v8start=$(echo "$tout" | awk -v p="$selp" '$1 == p' |
          grep -oE '[A-Z][a-z][a-z] +[0-9]+ +[0-9][0-9]:[0-9][0-9]' | tr -s ' ')
hoststart=$(ps -o lstart= -p "$selp" 2>/dev/null |
            awk '{printf "%s %s %s", $2, $3, substr($4,1,5)}')
if [ -n "$v8start" ] && [ -n "$hoststart" ]; then
	check "ps -T start time agrees with the host's lstart" \
	    "$hoststart" "$v8start"
else
	bad "ps -T prints a start time for the running shell" \
	    "v8 [$v8start] host [$hoststart]"
fi

# ...and -T must not disturb the rest of the line.  A truncated ctime would
# still have produced SOME string, so the case above could have passed on a
# wrong-but-parseable date; this one says the columns either side survived.
#
# THIS ASSERTED A HOST PROPERTY AND WENT RED IN CI, which is the class this
# suite has been swept for three times already.  It md5'd the first twenty
# sorted pids of `ps' and of `ps -T' and required them equal -- true only if
# NOTHING STARTED OR EXITED between two separate samples.  Measured: it failed
# once here (with a background `gh' poll running) and once on a runner, and the
# runner is where the log survived, because the local run had been piped to
# `tail -1' and the diagnosis thrown away.  Two hashes differing tells you
# nothing about which pid moved, which is its own argument against hashing a
# set you cannot then diff.
#
# THE STABLE RELATION.  Sample `ps' again AFTER the -T run and intersect: a
# process listed by both plain samples was alive across the whole window, so it
# was alive during the -T sample that happened between them, and -T must list
# it.  Churn can only remove pids from the intersection, never add a pid that
# -T should have had and did not -- so the case cannot go red for a process
# starting or stopping, and still goes red if the -T pid column is corrupted.
# Temp files rather than <(...): this script is #!/bin/sh, and process
# substitution is a bash/zsh extension.  Writing a host-shell dependency into
# the fix for a host-property case would be the same mistake one layer down.
psout2=$("$PS" hax 2>/dev/null)
_pids() { tail -n +2 | awk '{print $1}' | grep -E '^[0-9]+$' | LC_ALL=C sort -u; }
echo "$psout"  | _pids > "$TMP/pids.a"
echo "$psout2" | _pids > "$TMP/pids.b"
echo "$tout"   | _pids > "$TMP/pids.t"
comm -12 "$TMP/pids.a" "$TMP/pids.b" > "$TMP/pids.stable"
if [ ! -s "$TMP/pids.stable" ]; then
	bad "ps -T: two plain samples share at least one process" "intersection empty"
else
	check "ps -T lists every process alive across the whole window" '' \
	    "$(comm -23 "$TMP/pids.stable" "$TMP/pids.t" | tr '\n' ' ' | sed 's/ *$//')"
fi

echo "kmemu: $pass passed, $fail failed, $notex not exercised"
[ "$fail" -eq 0 ]
