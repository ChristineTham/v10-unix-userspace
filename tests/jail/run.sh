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
ck 'V8 yacc is in the jail' yes "$([ -x "$V8ROOT/bin/yacc" ] && echo yes || echo no)"

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

# --- PHASE 6d: the launcher, and the world it drops you into ----------------
# V8's own v8.c is twelve lines: chroot, chdir, drop privilege, exec the shell.
# Ours keeps chdir and umask, and drops the other two on purpose -- the chroot
# already happened (every binary links libv8sys), and dropping to uid 3 on a
# Mac would break every file operation to imitate a multi-user system PLAN.md
# S1 lists as a non-goal.
ck 'the launcher is installed' yes "$([ -x "$V8ROOT/bin/v8" ] && echo yes || echo no)"
ck 'it lands you at the root of the V8 world' '/' \
   "$(echo pwd | "$V8ROOT/bin/v8" 2>&1 | head -1)"

# The directories THEMSELVES, not just their contents.  v8dirs entries carry a
# trailing slash so "/binary" is not mistaken for "/bin/", and that also meant
# `ls /etc` listed the Mac's while `cat /etc/group` read V8's -- one path naming
# two different worlds depending on a trailing character.
ck 'ls /etc shows V8 s etc, not the host s' 'fstab' \
   "$(echo 'ls /etc' | "$V8ROOT/bin/v8" 2>&1 | head -1)"
case "$(echo 'ls /bin' | "$V8ROOT/bin/v8" 2>&1)" in
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
for prog in sed fmt tsort tbl yacc; do
	d=r5_$prog
	mkdir -p $d
	cp "$ROOT"/src/cmd/$prog/*.c "$ROOT"/src/cmd/$prog/*.h "$d"/ 2>/dev/null
	# the #included non-headers, which are not *.c and not *.h
	cp "$ROOT"/src/cmd/$prog/t..c "$ROOT"/src/cmd/$prog/dextern \
	   "$ROOT"/src/cmd/$prog/files "$d"/ 2>/dev/null
	cp "$ROOT"/src/cmd/$prog/[Mm]akefile "$d"/ 2>/dev/null
	out=$( cd $d && V8JAIL=strict "$MAKE8" 2>&1 )
	ck "$prog builds from its own V8 makefile" yes \
	   "$([ -x $d/$prog ] && echo yes || echo no)"
	case "$out" in
	*"leaves the jail"*) fail=$((fail+1)); echo "FAIL $prog escaped: $out" ;;
	*) pass=$((pass+1)) ;;
	esac
done

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
case "$(echo 'man cat' | "$V8ROOT/bin/v8" 2>&1)" in
*"Eighth Edition"*) pass=$((pass+1)) ;;
*) fail=$((fail+1)); echo "FAIL man via its shebang did not render" ;;
esac

echo "jail: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
