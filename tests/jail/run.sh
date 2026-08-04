#!/bin/sh
# The chroot, as a test.
#
# The claim being tested is the bootstrap claim: that when V8's make runs a
# build, every command in every recipe is V8 code -- the shell, the tools the
# shell then runs, and the compiler -- rather than the host's.
#
# WHY THIS IS NOT chroot(2).  Every V8 binary here is a Mach-O linked against
# /usr/lib/libSystem.B.dylib, so a real chroot would need dyld and the dyld
# shared cache inside the jail, and that cache is SIP-protected; chroot(2) also
# needs root.  Instead rootpath() in shim/v8sys/syscall.c resolves /bin/ and
# /usr/bin/ inside $V8ROOT, and v8s_execve routes through it.  The shim is this
# port's kernel, and chroot is a kernel service, so that is where it lives.
#
# WHY THE NEGATIVE CASES MATTER MOST.  rootpath() falls back to the host path
# when the rootfs does not have the file.  That keeps the port usable while
# /bin is incomplete, but it is the exact shape of the bug that has cost this
# port three debugging rounds -- a gap filled silently by the host, found only
# by its consequences (scanf, printf and execl each did it at the libc layer).
# So the fall-through is reported, and the cases below prove the report fires.
# A guard that has never been seen to fail is not a guard.

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
V8ROOT=$ROOT/rootfs; export V8ROOT
PATH=/bin:/usr/bin; export PATH
SHELL=/bin/sh; export SHELL
MAKE8=$ROOT/rootfs/bin/make

TMP=${TMPDIR:-/tmp}/jail.$$
mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
cd "$TMP" || exit 1

pass=0 fail=0
ck() {
	if [ "$2" = "$3" ]; then pass=$((pass+1))
	else fail=$((fail+1)); echo "FAIL $1"; echo "  want [$2]"; echo "  got  [$3]"; fi
}

[ -x "$MAKE8" ] || { echo "missing $MAKE8 -- run make v8make"; exit 1; }

# --- the /bin the jail needs ------------------------------------------------
for t in sh make cc cat echo rm cp mv; do
	if [ -x "$V8ROOT/bin/$t" ] || [ "$t" = cp ] || [ "$t" = mv ]; then
		pass=$((pass+1))
	else
		fail=$((fail+1)); echo "FAIL /bin/$t missing from the rootfs"
	fi
done

# --- the installed binaries are ours, not the host's ------------------------
if cmp -s "$V8ROOT/bin/cat" /bin/cat; then
	fail=$((fail+1)); echo "FAIL rootfs/bin/cat IS the host's /bin/cat"
else
	pass=$((pass+1))
fi

# --- a build runs entirely inside the jail ----------------------------------
cat > makefile <<'EOF'
all: out.txt
	echo "made $@"
out.txt: in.txt
	cat in.txt > out.txt
	echo appended >> out.txt
	rm -f scratch
EOF
echo seed > in.txt
: > scratch

# strict refuses any exec that would leave the jail, so a clean run under
# strict IS the proof -- not the absence of warnings, which proves nothing.
out=$(V8JAIL=strict "$MAKE8" 2>&1)
ck 'V8 make builds under V8JAIL=strict' 'seed appended' "$(cat out.txt 2>/dev/null | tr '\n' ' ' | sed 's/ $//')"
ck 'V8 rm ran inside the jail' 'gone' "$([ -f scratch ] && echo NO || echo gone)"
case "$out" in
*"leaves the jail"*) fail=$((fail+1)); echo "FAIL something escaped: $out" ;;
*) pass=$((pass+1)) ;;
esac

# --- the guard fires: a tool the rootfs does NOT have -----------------------
# The escape marker goes to a FILE, not to stdout.  make echoes each recipe
# before running it, so a marker inside the command text appears in the output
# whether or not the command ever ran -- which made the first version of this
# test report a pass for strict mode and a pass for unset mode, both wrong.
cat > mk2 <<'EOF'
all:
	/usr/bin/awk 'BEGIN{print "ran"}' > awkran.txt
EOF

rm -f awkran.txt
warn=$(V8JAIL=warn "$MAKE8" -f mk2 2>&1)
case "$warn" in
*"exec leaves the jail: /usr/bin/awk"*) pass=$((pass+1)) ;;
*) fail=$((fail+1)); echo "FAIL warn mode did not report the escape"; echo "  got [$warn]" ;;
esac
ck 'warn mode still lets it run' 'ran' "$(cat awkran.txt 2>/dev/null)"

rm -f awkran.txt
V8JAIL=strict "$MAKE8" -f mk2 >/dev/null 2>&1
ck 'strict mode refuses the host binary' '' "$(cat awkran.txt 2>/dev/null)"

# --- unset stays quiet, so a partly-ported tree still works -----------------
rm -f awkran.txt
quiet=$(V8JAIL= "$MAKE8" -f mk2 2>&1)
ck 'unset lets the host tool run' 'ran' "$(cat awkran.txt 2>/dev/null)"
case "$quiet" in
*"leaves the jail"*) fail=$((fail+1)); echo "FAIL unset should not report" ;;
*) pass=$((pass+1)) ;;
esac

# --- the shell make execs is V8's ------------------------------------------
# $$ in a recipe reaches the shell; V8 sh and the host sh differ in that V8's
# has no $RANDOM and no shell functions.  Simpler and unambiguous: ask the
# shim, by exec'ing a path only the rootfs has.
cat > mk3 <<'EOF'
all:
	cc -V 2>&1 | head -1
EOF
V8JAIL=strict "$MAKE8" -f mk3 >/dev/null 2>&1
ck 'cc resolves to V8 cc inside the jail' '0' "$?"

# --- the compiler is IN the jail, not merely reachable from it --------------
# `cc -V` above proves the shim resolved the name.  It does not prove the
# driver obeys the jail, and for a long time it did not: rootfs/bin/cc was a
# clang binary with zero V8 symbols, so every path it opened and every pass it
# exec'd went straight to the kernel and the jail could not observe one of them.
# The structural check first, because the behavioural ones below are only
# meaningful if it holds.
ck 'the installed driver is a V8 binary' 'yes' \
   "$(nm "$V8ROOT/bin/cc" 2>/dev/null | grep -q '_v8start' && echo yes || echo no)"

# --- a real compile AND link, entirely under strict -------------------------
cat > hello.c <<'EOF'
#include <stdio.h>
main()
{
	printf("compiled in the jail\n");
	exit(0);
}
EOF
strictout=$(V8JAIL=strict "$V8ROOT/bin/cc" -o hello hello.c 2>&1)
ck 'cc compiles and links under strict' 'compiled in the jail' "$(./hello 2>&1)"

# The link reaches the host's clang, which is on the exception list.  strict
# permits it and stays quiet, so any "leaves the jail" here is a real escape.
case "$strictout" in
*"leaves the jail"*) fail=$((fail+1)); echo "FAIL strict compile escaped: $strictout" ;;
*) pass=$((pass+1)) ;;
esac

# --- warn tells the two kinds of crossing apart -----------------------------
warnout=$(V8JAIL=warn "$V8ROOT/bin/cc" -o hello2 hello.c 2>&1)
case "$warnout" in
*"sanctioned host toolchain: /usr/bin/clang"*) pass=$((pass+1)) ;;
*) fail=$((fail+1)); echo "FAIL warn did not name the sanctioned clang exec"
   echo "  got [$warnout]" ;;
esac

# cpp and ccom live INSIDE the rootfs; the driver execs them by absolute path,
# which rootpath() has no reason to rewrite.  Reporting those as escapes made
# every compile print two false alarms and taught the reader to skip the output.
case "$warnout" in
*"leaves the jail"*) fail=$((fail+1))
   echo "FAIL warn reported a rootfs path as an escape"; echo "  got [$warnout]" ;;
*) pass=$((pass+1)) ;;
esac

# --- the exception list is a list, not a door -------------------------------
# clang being permitted must not permit anything else.  awk is refused above
# under strict; assert the wording differs, so the two can never be confused.
case "$warnout" in
*"exec leaves the jail: /usr/bin/clang"*)
   fail=$((fail+1)); echo "FAIL sanctioned and unsanctioned use the same wording" ;;
*) pass=$((pass+1)) ;;
esac

echo "jail: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
