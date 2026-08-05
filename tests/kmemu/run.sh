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

pass=0 fail=0
check() {
	if [ "$2" = "$3" ]; then pass=$((pass+1))
	else fail=$((fail+1)); echo "FAIL $1"; echo "  want [$2]"; echo "  got  [$3]"; fi
}
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL $1"; shift; [ $# -gt 0 ] && echo "    $*"; }

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
me=$(id -un)
name=$(dd if="$UTMP" bs=1 skip=8 count=8 2>/dev/null | tr -d '\0')
check "ut_name holds the login, terminator or not" "$(echo "$me" | cut -c1-8)" "$name"
line=$(dd if="$UTMP" bs=1 skip=0 count=8 2>/dev/null | tr -d '\0')
check "ut_line holds the tty" \
	"$(echo "$hostwho" | awk '{print $2}' | cut -c1-8)" "$line"
if [ ${#me} -eq 8 ]; then
	# The case that would pass anyway with a NUL-terminating copy: the
	# eighth byte must be the eighth character, not '\0'.
	last=$(dd if="$UTMP" bs=1 skip=15 count=1 2>/dev/null)
	check "an 8-character login fills ut_name with no terminator" \
		"$(echo "$me" | cut -c8)" "$last"
else
	# Not this host's shape. Assert the other half of the rule instead: a
	# short name is zero-filled, which is what who(1) tests for a free slot.
	pad=$(dd if="$UTMP" bs=1 skip=$((8 + ${#me})) count=1 2>/dev/null | od -An -tu1 | tr -d ' ')
	check "a short login is zero-filled to the field width" "0" "$pad"
fi

# --- -i reaches the tty, which is a host fact seen through the jail ------
# who -i stats /dev/<line> for its atime. That crosses the shim's /dev handling
# as well as libkmemu's, and it is the one who(1) path that reads anything
# beyond utmp.
"$WHO" -i > "$TMP/idle" 2>&1
check "who -i still names the user" \
	"$(echo "$hostwho" | awk '{print $1}')" \
	"$(awk '{print $1}' < "$TMP/idle")"

# --- THE BOUNDARY: who takes the utmpx trio from libc and nothing else ---
# This is the exception written down as an assertion. If libkmemu ever reaches
# for a string function, an allocator or stdio, it shows up here as a fourth
# import -- which is exactly how an exception list stops meaning anything.
libcimports() {
	nm -mu "$1" 2>/dev/null | grep -v dyld_stub_binder |
	    sed 's/.*external _//;s/ (from.*//' | sort | tr '\n' ' ' | sed 's/ $//'
}
# The set is libkmemu's WHOLE surface, not just what who happens to call: the
# synth table in synth.c names every generator, so linking any one of them pulls
# all of them. That is the honest statement and the one worth pinning -- five
# symbols, all of them "what is mounted / who is logged in", and a sixth
# appearing here means the exception grew.
KMEMU_IMPORTS="endutxent getfsstat getutxent setutxent statfs"
check "who imports libkmemu's whole surface and no more" \
	"$KMEMU_IMPORTS" "$(libcimports "$WHO")"
check "df imports the same set, being the same library" \
	"$KMEMU_IMPORTS" "$(libcimports "$V8ROOT/bin/df")"

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
for off in 31 63; do
	b=$(dd if="$MTAB" bs=1 skip=$off count=1 2>/dev/null | od -An -tu1 | tr -d ' ')
	[ "$b" = "0" ] && ok || bad "mtab field at $off is not NUL-terminated (got $b)"
done
# ...and the symptom, so a regression is caught even if the layout changes:
# every %use must be a number followed by %, never a digit avalanche.
if awk 'NR>1 {print $NF}' "$TMP/df.out" | grep -qvE '^[0-9]{1,3}%$'; then
	bad "df printed a malformed %use column" \
	    "$(awk 'NR>1 {print $NF}' "$TMP/df.out" | grep -vE '^[0-9]{1,3}%$' | head -1 | cut -c1-40)"
else ok; fi

# The numbers are the host's. df's kbytes column is s_fsize - s_isize, which
# libkmemu fills from statfs f_blocks scaled to 1K -- so it must equal what the
# host's own df -k calls 1024-blocks for the same device.
#
# The device is taken from df's OWN first row rather than from the host's `/'.
# In the V8 world "/" is $V8ROOT, which lives on whichever volume holds the
# repo -- so df legitimately reports that one and not the host's root device.
# Asking the host about the device df named is the comparison that means
# something; asking about a device df never mentions only tests the test.
hostdev=$(awk 'NR==2 {print $2}' "$TMP/df.out")
hostkb=$(df -k "/dev/$hostdev" 2>/dev/null | awk 'NR==2 {print $2}')
v8kb=$(awk -v d="$hostdev" '$2 == d {print $3; exit}' "$TMP/df.out")
if [ -n "$hostkb" ]; then
	check "df's kbytes matches the host for $hostdev" "$hostkb" "$v8kb"
else bad "could not ask the host about /dev/$hostdev"; fi

# df -i reports the FORMAT's ceiling, not the volume's contents, and that is
# the honest answer rather than a plausible one: s_isize and s_tinode are 16-bit
# in V7, so a volume with 548 million inodes cannot be described. A V8 df could
# not have shown it either. What must never happen is a number in between.
"$DF" -i > "$TMP/dfi.out" 2>/dev/null
ifree=$(awk -v d="$hostdev" '$2 == d {print $(NF-1); exit}' "$TMP/dfi.out")
check "df -i saturates ifree at the 16-bit ceiling" "65535" "$ifree"

# -l walks the free-block list, and there is no free list -- there is no disk.
# df says so in its own words rather than being handed a fabricated one.
"$DF" -l > "$TMP/dfl.out" 2>&1
grep -q 'bad free count' "$TMP/dfl.out" && ok ||
	bad "df -l invented a free list instead of failing" "$(head -2 "$TMP/dfl.out")"

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
# TWO ALLOWED LEAKS, named rather than tolerated, and the list is the record.
#
# sleep -- V8's own sleep(3) is alarm + a handler + `for(;;) pause()', and NO V8
#   PROGRAM IN THIS PORT CAN CATCH A SIGNAL: v8s_signal hands the raw sigaction
#   syscall a userland `struct sigaction', where the kernel wants
#   `struct __sigaction' -- which has a signal-trampoline pointer at offset 8,
#   exactly where the userland struct keeps sa_mask. Every handler is installed
#   with a null trampoline, and delivery hangs or kills the process. Importing
#   V8's sleep.c on top of that made make, rm and tail hang, so it was taken
#   back out. Comes off this list the moment signal delivery works.
#
# libm -- V8 shipped one and this port has never built it, so pic and grap do
#   their geometry with Apple's. Non-variadic, so it works and nothing looked
#   wrong; found by this sweep rather than by a bad drawing. Porting libm is its
#   own piece of work, and until then this is what says so.
ALLOWED="sleep
	sin cos atan2 sqrt exp log log10 pow floor ceil"

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
	case " who df $NOTV8 " in *" ${b##*/} "*) continue ;; esac
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

# ...and the exception is not stale. When signal delivery is fixed and V8's own
# sleep.c comes back, nothing will import it and this fails, which is the
# prompt to delete the entry.
for a in $ALLOWED; do
	case " $used " in
	*" $a "*) ok ;;
	*) bad "allowed-leak '$a' is no longer imported by anything -- drop it" ;;
	esac
done

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
check "the shim's timezone agrees with the host" \
	"$(date '+%a %b %e %H:%M')" "$("$V8ROOT/bin/date" | cut -c1-16)"
check "ls -l timestamps agree with the host" \
	"$(ls -l "$V8ROOT/etc/group" | awk '{print $6, $7, $8}')" \
	"$("$V8ROOT/bin/ls" -l "$V8ROOT/etc" | awk '$NF == "group" {print $6, $7, $8}')"

# --- a path libkmemu does not know is left completely alone -------------
# The hook runs on EVERY open(2) in a groveler. If it ever manufactured a file
# for a path it was not asked about, it would be writing into the rootfs behind
# the program's back.
rm -f "$V8ROOT/etc/nosuchthing"
"$WHO" "$V8ROOT/etc/nosuchthing" > /dev/null 2>&1
[ -e "$V8ROOT/etc/nosuchthing" ] && bad "an unknown path was synthesised" || ok

echo "kmemu: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
