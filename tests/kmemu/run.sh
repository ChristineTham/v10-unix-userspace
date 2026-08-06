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
	"$KMEMU_IMPORTS" "$(libcimports "$V8ROOT/bin/load")"
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

# ---------------------------------------------------------------- load ---
# The first program to need a NAMELIST. load(1) does not ask the system for the
# load average: it looks up the address of the kernel's _avenrun in /unix, then
# seeks to that address in /dev/kmem and reads three doubles. So the shim
# manufactures a kernel, and one table in kmem.c drives both files -- get them
# out of step and load reads the wrong bytes and prints them without complaint.
LOAD=$V8ROOT/bin/load
rm -rf "$V8ROOT/unix" "$V8ROOT/dev"
"$LOAD" > "$TMP/load.out" 2>"$TMP/load.err"
[ -f "$V8ROOT/unix" ]     && ok || bad "load creates /unix"
[ -f "$V8ROOT/dev/kmem" ] && ok || bad "load creates /dev/kmem"

# /dev did not exist, and nothing else has a reason to create it -- the jail
# lets /dev/null and /dev/tty reach the host precisely by NOT having them. The
# manufacturer makes its own directory; assert that rather than a build step
# nobody will remember.
[ -d "$V8ROOT/dev" ] && ok || bad "the /dev directory was created on demand"
for n in null tty console; do
	[ -e "$V8ROOT/dev/$n" ] && bad "rootfs/dev/$n exists -- /dev/$n now misses the host" || ok
done

# ...and put back the parts of /dev the BUILD owns, which that rm also took.
# /dev/dk, /dev/pt and /dev/drum are Makefile targets rather than manufactured
# files -- ps(1) calls error() and exits if any is missing (ps.c:21-28) -- so
# the manufacturer will not recreate them and the ps section at the end of this
# file would fail for the wrong reason. That the RULES exist is tests/deps's
# question; that ps works is this file's.
mkdir -p "$V8ROOT/dev/dk" "$V8ROOT/dev/pt" && : > "$V8ROOT/dev/drum"

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
W=$V8ROOT/bin/w
UP=$V8ROOT/bin/uptime

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
	check "names are five zero-padded digits" "1" "$(g fivedigit)"
	check "no live entry has inode 0"       "1" "$(g nonzero)"
	check "a process can open its own entry" "1" "$(g self)"
	check "pid 0 has no entry"              "1" "$(g bogus)"
	check "a non-numeric name is refused"   "1" "$(g notdigits)"

	# The count must track the host's.  Bracketed, not equal: processes come
	# and go between the two samples, and a flaky test is worse than none.
	hostn=$(ps ax | wc -l | tr -d ' ')
	v8n=$(g live)
	awk -v h="$hostn" -v v="$v8n" 'BEGIN { exit !(v > h - 40 && v < h + 40) }' &&
		ok || bad "/proc lists roughly what ps ax does" "host $hostn, /proc $v8n"
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
main()
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
	printf("nice %d\n", p.p_nice == NZERO);

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
	out=$("$TMP/gp" 2>/dev/null)
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
	# V8's nice is 0..39 about NZERO, macOS's -20..19 about 0.
	check "...nice biased to V8's NZERO"           "1" "$(g nice)"
	# Ticks read as nanoseconds would give 2 here, not ~100.
	pct=$(g pct)
	[ "$pct" -gt 40 ] && [ "$pct" -le 100 ] &&
		ok || bad "...a busy process reads near 100%cpu" "got $pct"
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
# ONE ALLOWED LEAK, named rather than tolerated, and the list is the record.
#
# libm -- V8 shipped one and this port has never built it, so pic and grap do
#   their geometry with Apple's. Non-variadic, so it works and nothing looked
#   wrong; found by this sweep rather than by a bad drawing. Porting libm is its
#   own piece of work, and until then this is what says so.
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
ALLOWED="sin cos atan2 sqrt exp log log10 pow floor ceil"

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

# ------------------------------------------------------------------ ps ---
# The payoff for /proc, and the first program here that reads a FILESYSTEM this
# port implements rather than a file it manufactures.  Bell Labs' 1985 source,
# unmodified, listing macOS processes.
PS=$V8ROOT/bin/ps

# /dev/dk, /dev/pt and /dev/drum have to exist or ps error()s before doing
# anything (ps.c:21-28). The load section above removes all of /dev and puts
# those three back; tests/deps is what asserts the Makefile makes them.
psout=$("$PS" hax 2>/dev/null)
psrc=$?
check "ps ax exits 0" "0" "$psrc"
echo "$psout" | head -1 | grep -q 'pid tty   stat  time command' &&
	ok || bad "ps -h prints V8's header" "$(echo "$psout" | head -1)"

# Bracketed rather than equal: processes come and go between the two samples.
hostn=$(ps ax | wc -l | tr -d ' ')
v8n=$(echo "$psout" | wc -l | tr -d ' ')
awk -v h="$hostn" -v v="$v8n" 'BEGIN { exit !(v > h - 40 && v < h + 40) }' &&
	ok || bad "ps ax lists what the host's ps ax does" "host $hostn, v8 $v8n"

# EVERY PID POSITIVE.  p_pid was a short and macOS pids reach 99998, so before
# src/include/sys/proc.h was patched every pid above 32767 printed NEGATIVE.
# This is the end-to-end form of that; the field-width assertion is above.
neg=$(echo "$psout" | tail -n +2 | awk '$1 <= 0' | head -3)
check "no process has a negative pid" "" "$neg"

# ...and the high ones are actually reached, or the check above proves nothing.
himax=$(echo "$psout" | tail -n +2 | awk '{if ($1+0 > m) m = $1+0} END {print m+0}')
[ "$himax" -gt 32767 ] && ok ||
	bad "ps saw a pid past the 16-bit boundary" "highest was $himax"

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
selp=$(echo "$psout" | tail -n +2 | awk 'NR==2 {print $1}')
if [ -n "$selp" ]; then
	sel=$("$PS" "$selp" 2>/dev/null | awk '{print $1}')
	check "ps <pid> selects exactly that process" "$selp" "$sel"
else bad "no pid to select"; fi

# The time column comes from the u-area, which is the half PIOCGETPR does not
# carry -- so a non-zero time anywhere proves the u-area read reached ps.
nz=$(echo "$psout" | tail -n +2 | awk '$4 != "0:00"' | wc -l | tr -d ' ')
[ "$nz" -gt 0 ] && ok || bad "some process has non-zero cpu time from the u-area"

echo "kmemu: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
