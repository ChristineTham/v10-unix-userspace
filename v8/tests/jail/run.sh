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
# cp and mv used to be excused here, because they had been imported into
# src/cmd/cp/ and src/cmd/mv/ and never built -- and an excused case is a case
# that has stopped asking. They are built now, so the exemption is gone and
# `mv' in particular is load-bearing: four upstream makefiles in the rung-5
# sweep below run `mv y.tab.c foo.c' and died on "Cannot load mv" without it.
#
# Asked of the jail's PATH rather than of /bin, and that is the right question
# rather than a weakening: V8 make execs /bin/sh and it is SH that searches, so
# what matters is that the name resolves the way a recipe would resolve it.  It
# also has to be asked that way now -- `sed' is a /usr/bin command in V8 and
# moved there when the install layout was taken from upstream's own tables.
for t in sh make cc cat echo rm cp mv sed mkdir rmdir; do
	found=
	for d in bin usr/bin; do
		[ -x "$V8ROOT/$d/$t" ] && { found=$d; break; }
	done
	if [ -n "$found" ]; then
		pass=$((pass+1))
	else
		fail=$((fail+1)); echo "FAIL $t is on neither /bin nor /usr/bin in the rootfs"
	fi
done

# --- the installed binaries are ours, not the host's ------------------------
if cmp -s "$V8ROOT/bin/cat" /bin/cat; then
	fail=$((fail+1)); echo "FAIL rootfs/bin/cat IS the host's /bin/cat"
else
	pass=$((pass+1))
fi

# --- CREATION LANDS INSIDE THE JAIL, NOT ON THE MAC -------------------------
# rootpath() decides by asking whether the rootfs has the path -- which is the
# right rule for a reader and is unanswerable for a name that does not exist
# yet.  So for a long time nothing being CREATED could be resolved, and
# creat("/etc/x") inside the jail went to the Mac's /etc.
#
# It failed there, because every V8 directory on macOS is root-owned, and it
# failed with EACCES -- which reads as a permissions problem rather than as a
# missing jail.  On a host directory that happened to be writable it would not
# have failed at all.
#
# The tell was the asymmetry: open("/etc/group", 0) read the jail's copy while
# creat("/etc/group") reached for the Mac's.  The same name meaning two
# different worlds depending on which syscall asked.  So both directions are
# checked here, and the read case is what makes the write case meaningful.
cat > mk.c <<'EOF'
#include <stdio.h>
main()
{
	extern int errno;
	int f;
	f = creat("/etc/v8jailprobe", 0644);
	printf("creat %d %d\n", f >= 0, f < 0 ? errno : 0);
	if (f >= 0) { write(f, "in-jail\n", 8); close(f); }
	f = open("/etc/v8jailprobe", 0);
	printf("reopen %d\n", f >= 0);
	if (f >= 0) close(f);
	if (mkdir("/usr/lib/v8jaildir", 0755) == 0) printf("mkdir 1\n");
	else printf("mkdir 0 %d\n", errno);
	exit(0);
}
EOF
rm -rf "$V8ROOT/etc/v8jailprobe" "$V8ROOT/usr/lib/v8jaildir"
"$V8ROOT/bin/cc" -o mk mk.c >/dev/null 2>&1
mkout=$(./mk 2>&1)
ck 'creat inside a jailed directory succeeds'   'creat 1 0' "$(echo "$mkout" | sed -n 1p)"
ck '...and the file is readable afterwards'     'reopen 1'  "$(echo "$mkout" | sed -n 2p)"
ck 'mkdir inside a jailed directory succeeds'   'mkdir 1'   "$(echo "$mkout" | sed -n 3p)"

# WHERE it landed is the actual assertion.  Both of the above would also pass
# on a host that let the write through, which is the outcome being ruled out.
[ -f "$V8ROOT/etc/v8jailprobe" ] && pass=$((pass+1)) ||
	{ fail=$((fail+1)); echo "FAIL the created file is not in the rootfs"; }
[ -d "$V8ROOT/usr/lib/v8jaildir" ] && pass=$((pass+1)) ||
	{ fail=$((fail+1)); echo "FAIL the created directory is not in the rootfs"; }
[ -e /etc/v8jailprobe ] && { fail=$((fail+1)); echo "FAIL it escaped to the Mac's /etc"; } ||
	pass=$((pass+1))

# ...AND THE SAME QUESTION OF mknod, which is how mkdir(1) makes a directory.
#
# The probe above calls mkdir(2) and always passed, because v8s_mkdir resolves.
# v8s_mknod did NOT -- it handed the raw path straight to SYS_mkdir. It was the
# one creating syscall the V8P_MAKE conversion missed, and it was missed for a
# reason worth keeping: mkdir(1) is the only caller of mknod in the entire tree,
# and mkdir(1) was among eleven commands imported and never built. An
# unreachable syscall cannot be seen to be wrong.
#
# mkdir(1) shows both halves of the split in one program: its access() asked the
# JAIL and its mknod asked the HOST. It failed closed only because every jailed
# prefix happens to be SIP-protected here; on a writable one it would have made
# the directory outside the jail and reported success. So the assertion is where
# the directory LANDS, tested through mkdir(1) because that is the only path to
# the syscall.
rm -rf "$V8ROOT/usr/lib/v8mknodprobe"
"$V8ROOT/bin/mkdir" /usr/lib/v8mknodprobe >/dev/null 2>&1
ck 'mkdir(1) creates inside the jail' 'yes' \
   "$([ -d "$V8ROOT/usr/lib/v8mknodprobe" ] && echo yes || echo no)"
[ -e /usr/lib/v8mknodprobe ] &&
	{ fail=$((fail+1)); echo "FAIL mkdir(1) escaped to the Mac's /usr/lib"; } ||
	pass=$((pass+1))
# ...and rmdir(1) takes it apart again, which is three unlink(2)s in V7 and
# proves the two resolve the same path rather than merely both succeeding.
"$V8ROOT/bin/rmdir" /usr/lib/v8mknodprobe >/dev/null 2>&1
ck 'rmdir(1) removes it from the jail' 'gone' \
   "$([ -d "$V8ROOT/usr/lib/v8mknodprobe" ] && echo NO || echo gone)"
rm -rf "$V8ROOT/usr/lib/v8mknodprobe"

# link took NEITHER of its names through rootpath, so `ln /bin/cat x' linked the
# MAC's /bin/cat -- a file the V8 world cannot even see with open(2).  Compared
# by size, because the two /bin/cat are different programs.
"$V8ROOT/bin/ln" /bin/cat lnprobe 2>/dev/null
if [ -f lnprobe ]; then
	ck 'link resolves its existing name inside the jail' \
	   "$(wc -c < "$V8ROOT/bin/cat")" "$(wc -c < lnprobe)"
else
	fail=$((fail+1)); echo "FAIL ln /bin/cat produced nothing"
fi
rm -f lnprobe
rm -rf "$V8ROOT/etc/v8jailprobe" "$V8ROOT/usr/lib/v8jaildir"

# ...and the union's READ rule is untouched by all of that: a path the rootfs
# does not have still falls through to the host.  This is the case that would
# fail if mkpath's parent rule had been applied to readers too.
cat > rd.c <<'EOF'
#include <stdio.h>
main()
{
	int f = open("/usr/lib/dyld", 0);	/* the Mac has it; the rootfs does not */
	printf("hostread %d\n", f >= 0);
	if (f >= 0) close(f);
	exit(0);
}
EOF
"$V8ROOT/bin/cc" -o rd rd.c >/dev/null 2>&1
ck 'a path the rootfs lacks still reads through to the host' \
   'hostread 1' "$(./rd 2>&1)"

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

# --- as(1) IS sanctioned, and only because an authentic makefile reaches it --
# PLAN.md S1 has sanctioned as/ld/ar/strip/nm from the start, but hosttools[]
# in shim/v8sys/syscall.c named only clang, on the reasoning that cc(1) reaches
# the rest THROUGH clang.  That held for every recipe this port had run, and
# stopped holding when sh's `:fix' -- an upstream build helper -- invoked $AS
# by name.  The same shape as v8s_mknod passing its path unresolved: a rule
# nothing exercises cannot be seen to be incomplete.
#
# Both directions, because the value of an exception list is that it excludes.
cat > jas.c <<'EOF'
main(c, v) char **v; { execl("/usr/bin/as", "as", "--version", 0); _exit(3); }
EOF
cat > jnm.c <<'EOF'
main(c, v) char **v; { execl("/usr/bin/nm", "nm", "--version", 0); _exit(3); }
EOF
"$V8ROOT"/bin/cc -o jas jas.c 2>/dev/null
"$V8ROOT"/bin/cc -o jnm jnm.c 2>/dev/null
asout=$( V8JAIL=strict ./jas 2>&1 >/dev/null )
case "$asout" in
*"leaves the jail"*) fail=$((fail+1)); echo "FAIL as refused under strict: $asout" ;;
*) pass=$((pass+1)) ;;
esac
# nm is on PLAN's prose list but NOT in hosttools[], because nothing execs it.
# Asserting the refusal keeps the array honest about what it actually permits:
# if a later change adds nm, this case says so rather than letting the list
# drift into "everything the prose mentions".
nmout=$( V8JAIL=strict ./jnm 2>&1 >/dev/null )
case "$nmout" in
*"leaves the jail"*) pass=$((pass+1)) ;;
*) fail=$((fail+1)); echo "FAIL nm is not in hosttools[] but was permitted" ;;
esac

# --- WHERE :fix STOPS, and it is arm64 that stops it ------------------------
# sh's makefile runs `:fix msg' and `:fix ctype'.  Both compile to assembly and
# rewrite .data to .text so the tables land in shared read-only text -- the VAX
# optimisation.  ctype.c is a character table and relocates nothing, so it
# assembles and links.  msg.c holds `struct sysnod commands[]', a table of
# pointers; an initialised pointer in __TEXT is a text relocation, and arm64
# executables are position-independent by force (`-no_pie ignored for arm64'),
# so it can never be resolved at link time the way a.out resolved it.
#
# Asserting the boundary rather than leaving sh unmentioned: if a future change
# makes this link, that is a real change in what the port can claim, and it
# should have to come past a failing test to say so.
mkdir -p fixp && cp "$ROOT"/src/cmd/sh/msg.c "$ROOT"/src/cmd/sh/*.h fixp/ 2>/dev/null
( cd fixp && "$V8ROOT"/bin/cc -S -c msg.c ) 2>/dev/null
( cd fixp && "$V8ROOT"/bin/ed - msg.s >/dev/null 2>&1 <<'EOF'
g/^[ 	]*\.data/s/data/text/
w
q
EOF
)
ck ":fix's ed rewrite moves sh's tables into text" yes \
   "$(grep -q '^	\.text' fixp/msg.s 2>/dev/null && echo yes || echo no)"
( cd fixp && as -o msg.o msg.s ) 2>/dev/null
ck "the rewritten assembly still assembles" yes \
   "$([ -f fixp/msg.o ] && echo yes || echo no)"
# A real link, not a bare one: ld reports undefined symbols and stops before it
# ever looks at relocations, so linking msg.o alone fails for the wrong reason
# and the case would pass on a machine where the rewrite worked fine.
echo 'main(){ return 0; }' > fixp/mstub.c
( cd fixp && "$V8ROOT"/bin/cc -c mstub.c ) 2>/dev/null
relout=$( clang -nostdlib -e _v8start -o fixp/msgx "$V8ROOT"/lib/crt0.o \
          fixp/msg.o fixp/mstub.o "$V8ROOT"/lib/libv8c.a \
          "$V8ROOT"/lib/libv8stubs.a "$V8ROOT"/lib/libv8sys.a -lSystem 2>&1 )
# THE CLAIM IS "THIS LINK CANNOT SUCCEED", so that is what is tested.  Keying
# the pass on the substring `text-reloc' would test Apple's diagnostic wording
# instead, and they have rewritten these once already (ld-prime).  A re-wording
# would turn a correct refusal into a failure whose message actively misdirects
# -- it would say the arm64 PIE rule had changed when nothing had.
#
# The wording is still printed when it is absent, because it is the evidence
# that the refusal is the one meant rather than some unrelated link error.
if [ -f fixp/msgx ]; then
	fail=$((fail+1))
	echo "FAIL sh's pointer table linked in __TEXT -- arm64 PIE rule changed?"
else
	pass=$((pass+1))
	case "$relout" in
	*text-reloc*) ;;
	*) echo "  (note: link refused, but not for a text relocation: $(echo "$relout" | head -1))" ;;
	esac
fi
rm -rf jas.c jnm.c jas jnm fixp

# --- RUNG 4: V8 make rebuilds the C compiler, inside the jail ---------------
# The ladder's whole point, and the first time every part of it is V8's at once:
# V8's make reads the makefile, V8's sh runs the recipes, V8's cc drives V8's
# cpp and V8's ccom, and the result is a working compiler -- all under
# V8JAIL=strict, so the only thing permitted to leave is the documented
# as/ld exception.
#
# The makefile is deliberately plain 1985 make: no pattern rules, no automatic
# variables beyond $@, no GNU anything. If it needs a feature V8's make does not
# have, that is a finding, not something to work around.
A64=$ROOT/compiler/ccom-arm64
MI=$ROOT/src/cmd/ccom/common
cp "$ROOT/src/cmd/ccom/vax/y.debug.sv" y.debug

cat > mkccom <<EOF
CC = cc
INC = -I$A64 -I$MI -I.
OBJ = xdefs.o scan.o pftn.o trees.o optim.o reader.o common1.o pjw.o \\
      lookup.o catch2.o t2print.o cgram.o \\
      local.o local2.o emit.o printx.o gencode.o dbstubs.o

ccom: \$(OBJ)
	\$(CC) -o ccom \$(OBJ)

xdefs.o: $MI/xdefs.c
	\$(CC) \$(INC) -DYYDEBUG -c -o xdefs.o $MI/xdefs.c
scan.o: $MI/scan.c
	\$(CC) \$(INC) -DYYDEBUG -c -o scan.o $MI/scan.c
pftn.o: $MI/pftn.c
	\$(CC) \$(INC) -DYYDEBUG -c -o pftn.o $MI/pftn.c
trees.o: $MI/trees.c
	\$(CC) \$(INC) -DYYDEBUG -c -o trees.o $MI/trees.c
optim.o: $MI/optim.c
	\$(CC) \$(INC) -DYYDEBUG -c -o optim.o $MI/optim.c
reader.o: $MI/reader.c
	\$(CC) \$(INC) -DYYDEBUG -c -o reader.o $MI/reader.c
common1.o: $MI/common1.c
	\$(CC) \$(INC) -DYYDEBUG -c -o common1.o $MI/common1.c
pjw.o: $MI/pjw.c
	\$(CC) \$(INC) -DYYDEBUG -c -o pjw.o $MI/pjw.c
lookup.o: $MI/lookup.c
	\$(CC) \$(INC) -DYYDEBUG -c -o lookup.o $MI/lookup.c
catch2.o: $MI/catch2.c
	\$(CC) \$(INC) -DYYDEBUG -c -o catch2.o $MI/catch2.c
t2print.o: $MI/t2print.c
	\$(CC) \$(INC) -DYYDEBUG -c -o t2print.o $MI/t2print.c
cgram.o: $MI/cgram.c
	\$(CC) \$(INC) -DYYDEBUG -c -o cgram.o $MI/cgram.c
local.o: $A64/local.c
	\$(CC) \$(INC) -c -o local.o $A64/local.c
local2.o: $A64/local2.c
	\$(CC) \$(INC) -c -o local2.o $A64/local2.c
emit.o: $A64/emit.c
	\$(CC) \$(INC) -c -o emit.o $A64/emit.c
printx.o: $A64/printx.c
	\$(CC) \$(INC) -c -o printx.o $A64/printx.c
gencode.o: $A64/gencode.c
	\$(CC) \$(INC) -c -o gencode.o $A64/gencode.c
dbstubs.o: $A64/dbstubs.c
	\$(CC) \$(INC) -c -o dbstubs.o $A64/dbstubs.c
EOF

out=$(V8JAIL=strict "$MAKE8" -f mkccom 2>&1)
ck 'V8 make rebuilds ccom under strict' yes "$([ -x ./ccom ] && echo yes || echo no)"

# Nothing unsanctioned may have escaped.  clang for the link is on the list and
# is silent under strict; anything else prints "leaves the jail".
case "$out" in
*"leaves the jail"*) fail=$((fail+1)); echo "FAIL rung 4 escaped: $out" ;;
*) pass=$((pass+1)) ;;
esac

# And the compiler it produced actually compiles.
"$V8ROOT/lib/cpp" "$ROOT/src/cmd/cat.c" "-I$V8ROOT/usr/include" > r4.i 2>/dev/null
./ccom r4.i r4.s >/dev/null 2>&1
ck 'the ccom V8 make built compiles a real file' yes \
   "$([ "$(wc -c < r4.s 2>/dev/null || echo 0)" -gt 1000 ] && echo yes || echo no)"

# NOT compared against $V8ROOT/lib/ccom.  That one is still the clang-built
# stage-0 compiler, and this one was built by v8cc, so they differ by exactly
# the generation PLAN.md S4c describes -- two instructions, and correctly so.
# Comparing them would fail for the right reason, which makes it a bad test.
#
# What must hold is that the makefile is REPRODUCIBLE: run it again from clean
# and it produces a compiler that generates the same code. A build driven by a
# makefile that does not is not a build.
mkdir -p again && cp y.debug again/
( cd again && V8JAIL=strict "$MAKE8" -f ../mkccom >/dev/null 2>&1 )
./again/ccom r4.i r4b.s >/dev/null 2>&1
ck 'a second V8 make build generates identical code' yes \
   "$(cmp -s r4.s r4b.s && echo yes || echo no)"

# make must settle: a second run does no work.
again=$(V8JAIL=strict "$MAKE8" -f mkccom 2>&1)
case "$again" in
*"-c -o"*) fail=$((fail+1)); echo "FAIL rung 4 does not settle: $again" ;;
*) pass=$((pass+1)) ;;
esac

# --- RUNG 5: a program built by its OWN authentic makefile ------------------
# lex's makefile is upstream V8, unmodified, and it is the one that matters:
# line 11 declares `lmain.o: lmain.c ldefs.c once.c`, the dependency whose
# absence caused this port's worst bug -- a 2x heap overrun that presented as
# "calloc returns 0" and cost a full session to re-derive. The knowledge was in
# the tree the whole time, in a file the build ignored.
#
# So this runs V8's make on V8's makefile with V8's cc and V8's yacc, in a
# directory containing nothing but V8's sources, under strict.
ck 'V8 yacc is in the jail' yes "$([ -x "$V8ROOT/usr/bin/yacc" ] && echo yes || echo no)"

mkdir -p r5 && cp "$ROOT"/src/cmd/lex/*.c "$ROOT"/src/cmd/lex/*.y \
    "$ROOT"/src/cmd/lex/[Mm]akefile r5/ 2>/dev/null
cp "$ROOT"/src/cmd/lex/ldefs.c "$ROOT"/src/cmd/lex/once.c r5/ 2>/dev/null
r5out=$( cd r5 && V8JAIL=strict "$MAKE8" 2>&1 )
ck "lex builds from V8's own makefile" yes "$([ -x r5/lex ] && echo yes || echo no)"
case "$r5out" in
*"leaves the jail"*) fail=$((fail+1)); echo "FAIL rung 5 escaped: $r5out" ;;
*) pass=$((pass+1)) ;;
esac

# It has to WORK, not just link.  A scanner that matches one token and prints it
# exercises yacc's tables, ncform, and the generated lex.yy.c together.
if [ -x r5/lex ]; then
	cat > r5/t.l <<'LEOF'
%%
[0-9]+	printf("NUM(%s)", yytext);
.	;
%%
LEOF
	( cd r5 && V8JAIL=strict ./lex t.l >/dev/null 2>&1 )
	ck 'and the lex it built generates a scanner' yes \
	   "$([ -s r5/lex.yy.c ] && echo yes || echo no)"
else
	fail=$((fail+1)); echo "FAIL rung 5: no lex to run"
fi

# --- PHASE 6b: pwd tells the truth about the jail ---------------------------
# "/" means $V8ROOT.  getwd() does not ask the kernel for a path -- V8 has no
# such syscall -- it stats "/" for the root's dev/ino and walks ".." up until it
# matches, building the name from directory entries.  With "/" meaning the
# host's root that walk ran straight past $V8ROOT, and pwd printed the Mac's
# path for a directory the V8 world calls /bin.
ck 'pwd inside the jail reports a V8 path' '/bin' \
   "$(cd "$V8ROOT/bin" && "$V8ROOT/bin/sh" -c pwd 2>&1)"
ck 'and at the root itself' '/' \
   "$(cd "$V8ROOT" && "$V8ROOT/bin/sh" -c pwd 2>&1)"
ck 'a nested V8 directory too' '/usr/lib' \
   "$(cd "$V8ROOT/usr/lib" && "$V8ROOT/bin/sh" -c pwd 2>&1)"

# --- PHASE 6c: /etc is not decoration ---------------------------------------
# getpw.c, getlogin.c and ttyslot.c read it, and without a passwd `ls -l` prints
# a bare numeric uid for every file.  group, fstab, hosts and ttys are genuine
# V8 files imported with provenance; passwd is synthesized at build time because
# it has to name the REAL user -- V8's own names 1985 Bell Labs staff, and
# getpwuid() would not find whoever is running this.
for f in group fstab hosts ttys passwd; do
	ck "/etc/$f is installed" yes "$([ -s "$V8ROOT/etc/$f" ] && echo yes || echo no)"
done
ck 'a V8 program reads V8 /etc, not the host s' 'other::1' \
   "$("$V8ROOT/bin/cat" /etc/group 2>&1 | head -1)"
ck 'the synthesized passwd names the real user' yes \
   "$(grep -q "^$(id -un):" "$V8ROOT/etc/passwd" && echo yes || echo no)"
# The functional point: a name, not a number.
case "$("$V8ROOT/bin/ls" -l "$V8ROOT/etc/group" 2>&1)" in
*"$(id -un)"*) pass=$((pass+1)) ;;
*) fail=$((fail+1)); echo "FAIL ls -l shows a bare uid, not a name" ;;
esac

# --- -l RESOLVES INSIDE THE ROOTFS, and that is a jail question -------------
# Until libpath() existed, every -l went straight to clang and therefore to the
# macOS SDK.  Nothing noticed, because our own build rules pass archives by
# absolute path and never use -l at all -- so the hole was reachable only from
# an authentic makefile, and eleven of them say -lm.
#
# pic and grap in the rung-5 sweep cover this only indirectly: they assert a
# link SUCCEEDS.  These two assert WHICH library answered, in both directions.
cat > lm.c <<'EOF'
#include <stdio.h>
main() { extern double sqrt(); printf("%.4f\n", sqrt(2.0)); exit(0); }
EOF
# The stub is upstream's own: v8/usr/lib/libm.a is 216 bytes and defines
# nothing, because V8's math is in libc.  So a correct -lm contributes no
# symbols and the answer must come from libv8c.
#
# WHY THE VALUE IS CHECKED AND NOT JUST THE EXIT STATUS.  Deleting the stub and
# re-running is the mutation, and it does not fail the way it looks like it
# should: this program still links, and prints 0.0000.  Falling through to the
# SDK's libm puts a real sqrt() ahead of V8's, and v8cc passes doubles in x0-x7
# while AAPCS64 reads them from d0-d7 -- so the argument is read from the wrong
# register and the answer is silently wrong.  pic fails LOUDLY on the same
# mutation (it references errno, which then resolves to libSystem's indirect
# symbol and cannot be relocated), so one missing archive produces a link error
# for one program and a wrong number for another.  The value is the assertion
# that catches the quiet half.
ck 'the rootfs has V8s libm stub' yes \
   "$([ -f "$V8ROOT/usr/lib/libm.a" ] && echo yes || echo no)"
"$V8ROOT"/bin/cc -o lm lm.c -lm 2>/dev/null
ck 'cc -lm links, and V8s own math answers' '1.4142' "$(./lm 2>&1)"
# ...and the fall-through is REPORTED rather than silent, because a gap filled
# quietly by the host is the shape this port has paid for three times.
lmout=$( V8JAIL=strict "$V8ROOT"/bin/cc -o lmx lm.c -lnosuchlib 2>&1 )
case "$lmout" in
*"is not in the rootfs"*) pass=$((pass+1)) ;;
*) fail=$((fail+1)); echo "FAIL -l fall-through was silent under V8JAIL: $lmout" ;;
esac
# Unset stays quiet, exactly as the exec report does, so a partly-ported tree
# is not noisy.
lmq=$( "$V8ROOT"/bin/cc -o lmx lm.c -lnosuchlib 2>&1 )
case "$lmq" in
*"is not in the rootfs"*)
   fail=$((fail+1)); echo "FAIL -l reported with V8JAIL unset" ;;
*) pass=$((pass+1)) ;;
esac

# -lc IS THE SAME HOLE WITH A LOADED GUN, and nothing we build uses it -- which
# is exactly why it needs a case.  28 upstream makefiles say -lc, and the ported
# set only grows.  Before libpath() handled it, this program linked, exited 0,
# and printed
#
#	printf works: 1830099536 (null)
#
# because -lc reaching clang puts libSystem ahead of libv8c: v8cc passes every
# argument positionally in x0-x7, Apple's ARM64 ABI passes variadic arguments on
# the stack.  Same bug as scanf, printf and execl, which this port has paid for
# three times.  A variadic call is checked rather than a scalar one for that
# reason -- a non-variadic function would have come back right and proved
# nothing.
cat > lc.c <<'EOF'
#include <stdio.h>
main() { printf("v=%d s=%s\n", 42, "str"); exit(0); }
EOF
"$V8ROOT"/bin/cc -o lc lc.c -lc 2>/dev/null
ck 'cc -lc does not reach the host libc' 'v=42 s=str' "$(./lc 2>&1)"
# ...and quietly, because -lc names a library the driver already links last.
# Resolving it to libv8c.a instead works and makes ld warn about a duplicate on
# every such link; a warning nobody can act on is how a real one gets skimmed.
lcw=$( "$V8ROOT"/bin/cc -o lc lc.c -lc 2>&1 )
case "$lcw" in
*duplicate*) fail=$((fail+1)); echo "FAIL cc -lc warns about a duplicate: $lcw" ;;
*) pass=$((pass+1)) ;;
esac
rm -f lm.c lm lmx lc.c lc

# --- PHASE 6d: the launcher, and the world it drops you into ----------------
# V8's own v8.c is twelve lines: chroot, chdir, drop privilege, exec the shell.
# Ours keeps chdir and umask, and drops the other two on purpose -- the chroot
# already happened (every binary links libv8sys), and dropping to uid 3 on a
# Mac would break every file operation to imitate a multi-user system PLAN.md
# S1 lists as a non-goal.
ck 'the launcher is installed' yes "$([ -x "$V8ROOT/usr/bin/v8" ] && echo yes || echo no)"
ck 'it lands you at the root of the V8 world' '/' \
   "$(echo pwd | "$V8ROOT/usr/bin/v8" 2>&1 | head -1)"

# The directories THEMSELVES, not just their contents.  v8dirs entries carry a
# trailing slash so "/binary" is not mistaken for "/bin/", and that also meant
# `ls /etc` listed the Mac's while `cat /etc/group` read V8's -- one path naming
# two different worlds depending on a trailing character.
# This asked whether the first line was `fstab', and that was a VALUE where a
# relation was meant -- true only for as long as nothing installed into /etc
# sorted before it.  Adding mkfs, icheck and dcheck (section 8a step 4) put
# `dcheck' at the top and turned it red, which is the right outcome from the
# wrong test: nothing about the jail had changed.
#
# The relation the case exists to state is that `ls /etc' inside the jail lists
# the ROOTFS's /etc and not the Mac's, so it is asserted against the rootfs
# itself.  It cannot drift as things are installed, and it says strictly more:
# not merely that one V8 file shows up, but that nothing else does.  LC_ALL=C on
# both sides, since the two listings come from two different ls implementations.
ck 'ls /etc shows V8 s etc, not the host s' \
   "$(LC_ALL=C ls -1 "$V8ROOT/etc" | LC_ALL=C sort | tr '\n' ' ')" \
   "$(echo 'ls /etc' | "$V8ROOT/usr/bin/v8" 2>&1 | LC_ALL=C sort | tr '\n' ' ')"
case "$(echo 'ls /bin' | "$V8ROOT/usr/bin/v8" 2>&1)" in
*cc*) pass=$((pass+1)) ;;
*) fail=$((fail+1)); echo "FAIL ls /bin does not show the V8 /bin" ;;
esac

# --- RUNG 5, generalised: four makefiles, four different idioms -------------
# lex above proves the mechanism.  It does not prove the mechanism copes with
# what upstream's makefiles actually contain, and they are not uniform:
#
#	sed     a recipe globbing *.o, and target and recipe on ONE line
#	fmt     macro definition and $(OBJ) expansion
#	tsort   .SUFFIXES and a .c.o suffix rule -- no explicit object rules
#
# If V8's make lacks something one of them needs, that is a finding about the
# make we ported, and better found here than by a program failing to build.
#	tbl     a t?.o glob, three flags at once (-i -s -O), and a 22-target
#	        dependency line on t..c -- an #included NON-HEADER, the class
#	        CLAUDE.md calls invisible to every scanner.  If V8 make gets
#	        that line right, the knowledge the build system had to be told
#	        was in the tree all along.
#	yacc    $(CC) rather than a literal, a y?.o glob, and the same shape of
#	        dependency on dextern and files.
#	sh is NOT here, and the reason is architectural rather than clerical.
#	It was recorded for a while as wanting "a generated source, msg.o",
#	which was simply wrong -- msg.c is checked in.  What the makefile does
#	with it is run `sh ./:fix msg', and :fix is a VAX SHARED-TEXT helper:
#	compile to assembly, rewrite .data to .text with ed, reassemble, so
#	every shell process maps one copy of the tables.  msg.c holds
#	`struct sysnod commands[]', a table of POINTERS, and an initialised
#	pointer in __TEXT needs a load-time fixup -- a text relocation, which
#	ld refuses.  Nor can it be waived: -no_pie is IGNORED for arm64, so
#	position independence is mandatory and the fixup can never be resolved
#	statically the way a.out resolved it at link time.  :fix ctype works
#	(a char table relocates nothing); :fix msg cannot, here, ever.
#	So sh sits with cpp in the category below, not in this sweep.
#	spell   FOUR programs from one makefile, and `spellprog` rather than
#	        `all`, because all regenerates the word lists from the american
#	        and british dictionaries -- a different claim, and a slow one.
#	man     the minimal case -- one .c, one rule -- kept because a sweep
#	        that only covers elaborate makefiles proves nothing about plain
#	        ones, and most of upstream's are plain.
#	troff   FOURTEEN objects from a twenty-two-file directory -- the largest
#	        link here beside eqn's twenty-two.  Scale is
#	        its own idiom: it is the case where a toolchain that is subtly
#	        wrong about include paths or object naming stops getting lucky.
#	refer   four programs and an #included non-header (refer..c), the same
#	        invisible-to-every-scanner class as tbl's t..c.
#	ps      `ps: & $(OBJ)' -- V8 make's `&', which no other makefile here
#	        uses -- plus `$(OBJ): ps.h' as the whole dependency statement.
#	load    two lines, and a GROVELER: upstream's makefile knows nothing of
#	w	libkmemu, which this port invented, so what it builds is a real
#	        program that cannot answer. Rung 5 is a claim about the build
#	        DESCRIPTION being Bell Labs', not about the binary being the
#	        installed one, and these two are where that distinction is
#	        visible rather than academic.
#	make    V8 make building ITSELF from its own makefile, which is the
#	        only entry here that closes a loop.  Its dependency statement
#	        is `$(OBJECTS): defs' -- defs being another #included
#	        non-header -- and for a while this sweep did not copy it, so
#	        make died on "Don't know how to make defs" and the blocker was
#	        recorded as a generated ident.c.  ident.c is checked in and
#	        always was.  The sweep was the bug, not the port.
#	eqn     builds `a.out', not `eqn'; upstream's makefile declares no
#	        target of its own name.  Asked for the wrong one, make fell
#	        through to the built-in .c -> executable rule, linked eqn.o
#	        alone and failed on every symbol in the other 21 objects --
#	        which read exactly like a broken link and was a wrong question.
#	pic     -lm, and the whole reason it now works: see shim/libm/dummy.c.
#	grap    Upstream's libm.a is a 216-byte archive holding one 62-byte
#	        object that defines nothing, because V8's math is in libc. The
#	        driver used to hand -lm to clang, which answered with the SDK's
#	        libm -- a libSystem re-export ahead of libv8c on the link line
#	        -- and _errno resolved to an indirect symbol with no address.
#	quot    THE FIRST IMAGE TOOL TO GET HERE, and it is here by a measured
#	        no-op rather than an exemption.  The other nine in $(IMGBIN) are
#	        compiled -DDIRSIZ=14 and their own build descriptions pass no
#	        -D, so rung 5 would build a different program; quot reads
#	        inodes and never a directory, so its object is byte-identical
#	        either way.  tests/mkfs proves the identity by cmp'ing two
#	        compiles; the case below proves the consequence, by asking the
#	        rung-5 binary and the installed one the same question about a
#	        real filesystem.  `quot: quot.o' with no rule for the object,
#	        so V8 make's built-in .c.o is doing the work, as in tsort.
for spec in "sed sed" "fmt fmt" "tsort tsort" "tbl tbl" "yacc yacc" \
            "spell spellprog" "man man" "troff troff" "refer refer" \
            "ps ps" "load load" "w w" \
            "make make" "eqn a.out" "pic pic" "grap grap" "quot quot"; do
	set -- $spec
	prog=$1 target=$2
	d=r5_$prog
	mkdir -p $d
	cp "$ROOT"/src/cmd/$prog/*.c "$ROOT"/src/cmd/$prog/*.h "$d"/ 2>/dev/null
	# grammars and lexers: make, eqn, pic and grap each generate a .c the
	# makefile never mentions, through V8 make's built-in .y and .l rules.
	cp "$ROOT"/src/cmd/$prog/*.y "$ROOT"/src/cmd/$prog/*.l "$d"/ 2>/dev/null
	# the #included non-headers, which are not *.c and not *.h
	cp "$ROOT"/src/cmd/$prog/t..c "$ROOT"/src/cmd/$prog/dextern \
	   "$ROOT"/src/cmd/$prog/files "$ROOT"/src/cmd/$prog/refer..c \
	   "$ROOT"/src/cmd/$prog/defs \
	   "$d"/ 2>/dev/null
	# sh's makefile runs `sh ./:fix ctype`.  A build helper whose name
	# begins with a colon matches no glob at all -- the same invisibility
	# as the #included non-headers, one layer further out.
	cp "$ROOT"/src/cmd/$prog/:fix "$d"/ 2>/dev/null
	cp "$ROOT"/src/cmd/$prog/[Mm]akefile "$d"/ 2>/dev/null
	out=$( cd $d && V8JAIL=strict "$MAKE8" $target 2>&1 )
	ck "$prog builds $target from its own V8 makefile" yes \
	   "$([ -x $d/$target ] && echo yes || echo no)"
	case "$out" in
	*"leaves the jail"*) fail=$((fail+1)); echo "FAIL $prog escaped: $out" ;;
	*) pass=$((pass+1)) ;;
	esac
done

# AND THE ONE ABOVE THAT CAN BE ASKED A QUESTION.  For the other sixteen, rung 5
# ends at "it built"; load and w go further and say `No mem', because upstream's
# makefile knows nothing of libkmemu.  quot is the opposite case and the first
# of its kind here: the build description is complete, the program needs no
# invented library, and the flag its group carries is a measured no-op for it.
# So the rung-5 binary and the one our Makefile installed can be handed the same
# image and must agree -- which is the positive counterpart to the `who' pair
# further down, where the difference IS the point.
if [ -x r5_quot/quot ]; then
	printf '/dev/null\n200 32\nd--777 0 0\n$\n' > r5q.proto
	"$V8ROOT/etc/mkfs" r5q.img r5q.proto >/dev/null 2>&1
	a=$("$V8ROOT/etc/quot" -f r5q.img 2>&1)
	b=$(./r5_quot/quot   -f r5q.img 2>&1)
	if [ -n "$a" ] && [ "$a" = "$b" ]; then pass=$((pass+1))
	else fail=$((fail+1)); echo "FAIL the rung-5 quot answers differently"
	     echo "  ours [$a]"; echo "  r5   [$b]"; fi
else
	fail=$((fail+1)); echo "FAIL rung 5: no quot to run"
fi

# --- #! scripts are interpreted INSIDE the jail ------------------------------
# The kernel resolves a shebang against the real filesystem before the shim sees
# it, so every shell script in the world used to run under the Mac's sh --
# looking at the Mac's directories, running the Mac's commands, never calling
# rootpath() once.  v8s_execve now reads the interpreter line itself.
cat > sb <<'SBEOF'
#!/bin/sh
pwd
SBEOF
chmod 755 sb
ck 'a #! script runs under V8 sh, so pwd is a V8 path' '/' \
   "$(cd "$V8ROOT" && "$V8ROOT/bin/sh" -c "$TMP/sb" 2>&1 | head -1)"

# man through its OWN #! line -- the case that found this.
case "$(echo 'man cat' | "$V8ROOT/usr/bin/v8" 2>&1)" in
*"Eighth Edition"*) pass=$((pass+1)) ;;
*) fail=$((fail+1)); echo "FAIL man via its shebang did not render" ;;
esac

# ---------------------------------------------------------------------------
# RUNG 5 FOR THE HALF OF cmd/ THAT HAS NO MAKEFILE: Bell Labs' Admin/Mk.
#
# Everything above about rung 5 is "V8's make reads an upstream makefile", and
# it is demonstrated on eighteen programs.  MORE THAN HALF OF cmd/ HAS NO
# MAKEFILE -- those are bare *.c files, and their build description is
# Admin/Mk, a shell script.  For each name it does
#
#	eval D=`Admin/dest $B`;  cc $CFLAGS -o $B $B.c
#	install $B $D/$B         # strip $1 && cp $1 $2
#	rm -f $B.o $B
#
# so a run exercises V8's sh (functions, backquotes, eval, case, set -p), two
# nested V8 shell scripts with no #! line, V8's cc driving V8's cpp and ccom,
# and upstream's own install-destination tables -- and then has to leave the
# directory as it found it.
#
# THE WHOLE ROOTFS IS COPIED FIRST, and that is not tidiness: install() writes
# to /bin and /usr/bin, so a run against the real rootfs would replace the
# binaries every later suite uses with ones this test built.  14 MB, 0.3s.
#
# The other reason to copy: the programs can then be DELETED before the run, so
# their reappearance is evidence.  A test that rebuilds over an existing file
# proves only that make ran.
# ---------------------------------------------------------------------------
RFS=$TMP/rfs
cp -a "$V8ROOT" "$RFS" || { echo "FAIL could not copy the rootfs"; fail=$((fail+1)); }
MKNAMES=$(ls "$RFS"/usr/src/cmd/*.c 2>/dev/null | sed 's|.*/||')
MKCOUNT=$(echo $MKNAMES | wc -w | tr -d ' ')
# If the staging rule ever stops running, every case below would pass vacuously
# on an empty list.  That is the failure tests/cpp already had once.
if [ "$MKCOUNT" -ge 40 ]; then pass=$((pass+1))
else fail=$((fail+1)); echo "FAIL only $MKCOUNT sources staged at /usr/src/cmd -- run make srctree"; fi

# AND NO IMAGE TOOL MAY BE ON THAT LIST, which is a structural guard rather than
# a preference.  Mk compiles a bare cmd/*.c with `cc $CFLAGS -o $B $B.c' and no
# -D of any kind -- correct on a machine whose param.h says DIRSIZ 14, and wrong
# here, where it says 254.  What that costs is measured in tests/mkfs section 9
# and it is silent in BOTH directions: an Mk-built mkfs writes 256-byte records
# that every checker pronounces clean, and an Mk-built ncheck reads a good image
# and prints nothing at all, exit status 0.  $(V8BIN) and $(IMGBIN) are two
# lists in one Makefile and nothing else would notice a name moving between
# them, so the disjointness is asserted here where the consequence lands.
#
# AND IT CATCHES A STALE ROOTFS AS WELL AS A WRONG LIST, which is not a
# weakness: $(SRCTREE) staging is ADDITIVE.  make copies a source in when the
# name is in $(V8BIN) and never removes it when the name leaves, so a tree that
# once had ncheck there keeps serving it to Mk.  Found by this very case, on a
# run after a mutation had put ncheck in $(V8BIN) and taken it out again --
# CLAUDE.md's third shape, a property of what ran before rather than of the
# machine.  The fix is to delete the file (or `make clean'), not to relax this.
overlap=
for n in mkfs icheck dcheck clri fsck ncheck quot restor dumpdir dump; do
	case " $(echo $MKNAMES) " in *" $n.c "*) overlap="$overlap $n" ;; esac
done
if [ -z "$overlap" ]; then pass=$((pass+1))
else
	fail=$((fail+1))
	echo "FAIL \$(IMGBIN) program staged for Admin/Mk:$overlap"
	echo "  Mk passes no -D, so it would build these at the host's DIRSIZ 254."
	echo "  Either the name is in \$(V8BIN), or rootfs/usr/src/cmd holds a"
	echo "  stale copy from when it was -- staging never unstages.  rm it."
fi

# Where our Makefile put each one, before anything is deleted.
for n in $(echo "$MKNAMES" | sed 's|\.c$||'); do
	(cd "$RFS" && ls bin/$n usr/bin/$n etc/$n lib/$n 2>/dev/null | head -1)
done > "$TMP/mk.before"
ck 'every staged source has an installed binary to compare against' \
   "$MKCOUNT" "$(grep -c . "$TMP/mk.before")"

# DELETE A SUBSET, NOT ALL OF THEM, and the reason is worth keeping: Mk runs
# inside the world it is rebuilding.  `Admin/lookline' IS grep, the loop echoes,
# each program ends `rm -f $B.o $B', and every name is put through basename --
# so deleting all fifty removes the tools the script needs on its second
# iteration, and the run dies in a way that reads like a compiler failure.  (It
# did.)  Rebuilding those four OVER THEMSELVES is fine and happens anyway; only
# removing them first is not.
MKDEL='cat rev sort sum wc who cal ascii od tr uniq comm'
grep -E "/($(echo $MKDEL | tr ' ' '|'))\$" "$TMP/mk.before" > "$TMP/mk.del"
ck 'the delete list resolves to installed paths' \
   "$(echo $MKDEL | wc -w | tr -d ' ')" "$(grep -c . "$TMP/mk.del")"
(cd "$RFS" && while read -r p; do rm -f "$p"; done < "$TMP/mk.del")
ck 'they are gone before Mk runs' '0' \
   "$(cd "$RFS" && while read -r p; do [ -e "$p" ] && echo x; done < "$TMP/mk.del" | grep -c .)"

V8ROOT="$RFS" V8JAIL=strict "$RFS/bin/sh" "$RFS/usr/src/cmd/Admin/Mk" $MKNAMES \
    > "$TMP/mk.log" 2>&1
ck 'Mk reports one header per program' "$MKCOUNT" "$(grep -c '========' "$TMP/mk.log")"

# Under strict, a sanctioned tool is silent and an escape is refused loudly, so
# any v8sys line at all is a failure.
ck 'nothing left the jail' '' "$(grep '^v8sys' "$TMP/mk.log")"

# The claim.  Not "a binary appeared" but "the binary upstream's own tables name
# appeared", which is what makes this a rung-5 case rather than a build check.
ck 'every program is back, at the path Admin/dest names' "$MKCOUNT" \
   "$(cd "$RFS" && while read -r p; do [ -x "$p" ] && echo x; done < "$TMP/mk.before" | grep -c .)"

# TWO INDEPENDENT DERIVATIONS OF THE SAME ANSWER.  $(call v8dest,...) in the
# Makefile reads Bell Labs' tables at build time; Admin/dest reads them at run
# time, in V8's sh, with an arm for ulibfiles the Makefile deliberately omits.
# Comparing them is the only thing that would notice the omission mattering.
for n in $(echo "$MKNAMES" | sed 's|\.c$||'); do
	(cd "$RFS" && ls bin/$n usr/bin/$n etc/$n lib/$n 2>/dev/null | head -1)
done > "$TMP/mk.after"
if cmp -s "$TMP/mk.before" "$TMP/mk.after"; then pass=$((pass+1))
else fail=$((fail+1)); echo "FAIL Admin/dest and \$(call v8dest,...) disagree:"
     diff "$TMP/mk.before" "$TMP/mk.after" | head -6; fi

# Mk ends each program with `rm -f $B.o $B', so the source directory has to
# come out as it went in.  A build that litters /usr/src/cmd would be caught
# here and nowhere else.
ck 'Mk cleans up after itself' '' \
   "$(ls "$RFS/usr/src/cmd" | grep -vE '\.c$|^Admin$' | tr '\n' ' ' | sed 's/ $//')"

# And they RUN, with the same answers as the ones our Makefile built.  The paths
# are LOOKED UP rather than written down -- writing `bin/wc' when it is really
# usr/bin/wc produced four failures that read as build errors and were a typo,
# which is the same trap $(call v8dest,...) exists to avoid at build time.  `who'
# is here because it is the one that needs libkmemu: a driver that quietly
# dropped a library would show as a wrong ANSWER here rather than at the link.
printf 'delta\nalpha\ncharlie\n' > "$TMP/mkin"
mkpath() { grep -E "/$1\$" "$TMP/mk.before" | head -1; }
for c in "cat $TMP/mkin" "rev $TMP/mkin" "sort $TMP/mkin" "sum $TMP/mkin" \
         "wc -l $TMP/mkin"; do
	n=${c%% *}; args=${c#$n}
	p=$(mkpath "$n")
	if [ -z "$p" ]; then fail=$((fail+1)); echo "FAIL cannot locate $n"; continue; fi
	a=$(V8ROOT="$V8ROOT" sh -c "\"$V8ROOT/$p\" $args" </dev/null 2>&1)
	b=$(V8ROOT="$RFS"    sh -c "\"$RFS/$p\" $args"    </dev/null 2>&1)
	if [ "$a" = "$b" ] && [ -n "$a" ]; then pass=$((pass+1))
	else fail=$((fail+1)); echo "FAIL Mk-built $p differs"; echo "  ours [$a]"; echo "  Mk   [$b]"; fi
done

# WHO IS THE GROVELER, AND IT MUST *NOT* AGREE.  This is the third instance of
# the distinction PLAN.md draws for `load' and `w': upstream's build description
# knows nothing about libkmemu, which this port invented, so what rung 5 builds
# is a real program that cannot answer.  `w' says `No mem'; a Mk-built `who'
# says `cannot open /etc/utmp'.
#
# The mechanism is worth naming because it is a decision rather than a gap.
# libkmemu manufactures /etc/utmp when a reader opens it, and it reaches the
# link through the Makefile's groveler rules -- NOT through the cc driver's
# default library list, which is deliberate: putting it there would make every
# V8 binary import libSystem for facts it never asks about.  `nm -u' on the
# Mk-built who is empty where ours has _setutxent/_getutxent/_endutxent.
#
# AND THIS CASE STARTED LIFE ASSERTING THE OPPOSITE, passed here, and failed on
# a GitHub runner -- correctly.  /etc/utmp is created lazily by the first who
# that runs, so on a machine where any earlier run had made one, `cp -a' carried
# it into the copy and the Mk-built who read a file libkmemu had left behind.
# On a fresh runner there was none.  Green locally was an artifact of a stale
# file, and the runner was right; hence the rm below, so the case does not
# depend on what ran before it.
rm -f "$RFS/etc/utmp"
whop=$(mkpath who)
ck 'a Mk-built groveler runs and reports that it cannot answer' \
   'who: cannot open /etc/utmp' \
   "$(V8ROOT="$RFS" "$RFS/$whop" </dev/null 2>&1)"
# ...while the one our rules built does answer, on the same host, seconds apart.
# The pair is the assertion: a difference in the BUILD DESCRIPTION, not in the
# machine, and not in the compiler.
ourwho=$(V8ROOT="$V8ROOT" "$V8ROOT/$whop" </dev/null 2>&1)
case $ourwho in
*"cannot open"*|"") fail=$((fail+1))
	echo "FAIL our own who cannot answer either -- libkmemu is not linked" ;;
*) pass=$((pass+1)) ;;
esac

# What it reaches, named rather than merely counted as zero above.  strip is on
# this list because Mk's install() is `strip $1 && cp $1 $2': without it the &&
# short-circuits and NOTHING INSTALLS, which reads as a build failure rather
# than as a jail decision.  clang appears twice per program, for the assemble
# and the link.
V8ROOT="$RFS" V8JAIL=warn "$RFS/bin/sh" "$RFS/usr/src/cmd/Admin/Mk" cat.c \
    > "$TMP/mkwarn.log" 2>&1
ck 'the only host tools Mk reaches are the documented ones' \
   '/usr/bin/clang /usr/bin/strip' \
   "$(grep '^v8sys: sanctioned' "$TMP/mkwarn.log" | sed 's/.*toolchain: //;s/ .*//' \
      | sort -u | tr '\n' ' ' | sed 's/ $//')"

echo "jail: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
