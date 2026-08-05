#!/bin/sh
# The build graph, as a test.
#
# Four times in this port a bug was really a stale object file, and each cost
# a full debugging round because a stale object does not look like a build
# problem -- it looks like the code being wrong.  The worst was lex: `once.c`
# widened left[] and right[] to long, `parser.y` allocates them with
# sizeof(*left), and a y.tab.o built before the widening allocated 1700*4 bytes
# for arrays written as 8-byte longs.  That is a 2x heap overrun through the
# next block's malloc header, and it presented as "calloc returns 0".
#
# So the dependency graph is not a convenience here.  It is load-bearing, and
# it gets tested like anything else load-bearing.
#
# HOW THIS TESTS WITHOUT REBUILDING ANYTHING
#
# `make -q TARGET` answers "would this be remade?" without running a recipe:
# exit 0 means up to date, non-zero means stale.  Each case asserts the target
# is up to date, touches the input, asserts it went stale, then restores the
# input's ORIGINAL mtime with `cp -p` so the tree is left exactly as found.
# Nothing is compiled, so the whole suite runs in about a second.
#
# THE ONE-SECOND SLEEP IS NOT A FLAKE GUARD
#
# GNU make 3.81 -- which is what macOS ships, and what this builds under --
# compares mtimes at WHOLE-SECOND granularity, even though APFS records
# nanoseconds.  Measured: two files 17ms apart compare equal to make.  So a
# `touch` in the same second as the build that produced the object is invisible,
# and every case here would pass vacuously.  The sleep puts the touch into a
# later second than the settling build.
#
# (That granularity is also a real hazard for a human: edit a header within the
# same second a build finished and make will not notice.  Nothing in the rules
# can fix it -- it is make's comparison, not our graph.)

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
MAKE=${MAKE:-make}
cd "$ROOT" || exit 1

B=build/stage0
pass=0 fail=0
TMP=${TMPDIR:-/tmp}/depstest.$$
mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT

# Settle the tree first: every case assumes its target starts up to date.
if ! $MAKE >/dev/null 2>&1; then
	echo "FAIL cannot settle the build -- run make and fix that first"
	exit 1
fi
sleep 1

# needs_remake TARGET -> true when make says the target is stale
needs_remake() {
	$MAKE -q "$ROOT/$1" >/dev/null 2>&1
	[ $? -ne 0 ]
}

# dep DESCRIPTION INPUT TARGET
#   asserts TARGET is up to date, that touching INPUT makes it stale, and
#   restores INPUT's original mtime.
dep() {
	desc=$1 input=$2 target=$3

	if [ ! -f "$input" ]; then
		fail=$((fail+1)); echo "FAIL $desc: no such input $input"; return
	fi
	if [ ! -f "$target" ]; then
		fail=$((fail+1)); echo "FAIL $desc: no such target $target"; return
	fi
	# Control: without the touch, the target must be up to date.  Without this
	# a rule that is ALWAYS stale would pass every case for the wrong reason --
	# which is exactly what the phony `v8yacc` prerequisite used to do.
	if needs_remake "$target"; then
		fail=$((fail+1)); echo "FAIL $desc: $target was already stale before the touch"
		return
	fi

	cp -p "$input" "$TMP/save" || { fail=$((fail+1)); echo "FAIL $desc: cannot save"; return; }
	touch "$input"
	if needs_remake "$target"; then
		pass=$((pass+1))
	else
		fail=$((fail+1))
		echo "FAIL $desc"
		echo "  touching $input did not make $target stale"
	fi
	cp -p "$TMP/save" "$input"
}

# nodep DESCRIPTION INPUT TARGET -- the negative control.  Proves the suite can
# tell the difference, rather than reporting "stale" for everything.
nodep() {
	desc=$1 input=$2 target=$3
	cp -p "$input" "$TMP/save" || { fail=$((fail+1)); echo "FAIL $desc: cannot save"; return; }
	touch "$input"
	if needs_remake "$target"; then
		fail=$((fail+1))
		echo "FAIL $desc"
		echo "  touching $input should NOT have made $target stale"
	else
		pass=$((pass+1))
	fi
	cp -p "$TMP/save" "$input"
}

# --- a variable used above its definition expands to NOTHING -----------------
# This has now bitten three times, each time silently:
#   $(ROOTFS) in a target name       -> the rule built /lib/...
#   $(A64BUILD) in a test prereq     -> test-v8ccom depended on /v8ccom
#   $(V8DEPS) in the /bin rules      -> 38 binaries with no library dependency,
#                                       linked correctly and never relinked
# make expands a prerequisite when it READS the rule, exactly as it does a
# target name, so a variable defined further down the file is empty there.
# Nothing in the build fails; the dependency just is not there.
#
# Remembering to run this check is what failed twice, so it is a test now.
undef=$($MAKE -n --warn-undefined-variables 2>&1 | grep -c 'warning: undefined variable' || true)
if [ "$undef" -eq 0 ]; then
	pass=$((pass+1))
else
	fail=$((fail+1))
	echo "FAIL $undef undefined-variable warnings -- a variable is used above its definition"
	$MAKE -n --warn-undefined-variables 2>&1 | grep 'warning: undefined variable' | sort -u | head -5
fi

# --- the toolchain: a change here invalidates every object in the tree -------
# cc.c is the one that was actually broken.  The driver was not a prerequisite
# of anything it compiled, so this session's change to its link line rebuilt
# nothing; had the change affected code generation, every object would have
# been silently stale.
dep 'cc driver -> sh'          src/cmd/cc.c                    $B/sh/main.o
dep 'cc driver -> libc'        src/cmd/cc.c                    $B/libc/gen/malloc.o

# --- the driver is now a V8 binary, which gives it real prerequisites -------
# It used to be one clang command with one input.  It is now compiled by the
# seed and LINKED against crt0 + libv8c + libv8sys, so it has the same exposure
# every other V8 program has -- and that is precisely the exposure that left 38
# /bin binaries stale when $(V8DEPS) expanded to nothing.  Same shape, so the
# same cases.
dep 'cc.c -> seed driver'      src/cmd/cc.c                    $B/cc/cc-seed
dep 'cc.c -> driver object'    src/cmd/cc.c                    $B/cc/cc.o
dep 'driver object -> v8cc'    $B/cc/cc.o                      $B/cc/v8cc
dep 'libc -> v8cc'             $B/libc/libv8c.a                $B/cc/v8cc
dep 'shim -> v8cc'             $B/v8sys/libv8sys.a             $B/cc/v8cc
dep 'crt0 -> v8cc'             $B/crt0.o                       $B/cc/v8cc
dep 'seed -> libc'             $B/cc/cc-seed                   $B/libc/gen/malloc.o

# The cycle-break, asserted rather than assumed.  libv8c is compiled by the
# SEED driver precisely so it does not depend on the installed one -- the
# installed one is linked against libv8c.  If someone "tidies" the libc rules
# back onto $(V8CCRUN), make reports a dependency loop and drops the
# prerequisite, and this is the case that notices.
nodep 'libc does not need installed cc' rootfs/bin/cc          $B/libc/gen/malloc.o
dep 'ccom backend -> libc'     compiler/ccom-arm64/gencode.c   $B/libc/gen/malloc.o
dep 'ccom macdefs -> sh'       compiler/ccom-arm64/macdefs.h   $B/sh/main.o
dep 'cpp -> libc'              src/cmd/cpp/cpp.c               $B/libc/gen/malloc.o
# The V8 system headers, reached through $V8ROOT/usr/include rather than by any
# -I, so nothing on a command line reveals them.
dep 'V8 headers -> sh'         src/include/setjmp.h            $B/sh/main.o
dep 'V8 headers -> troff'      src/include/setjmp.h            $B/troff/n1.o

# --- #included .c files: invisible to a header scanner AND to a *.c glob -----
# The lex case is the bug this whole suite exists for.
dep 'lex once.c -> y.tab.o'    src/cmd/lex/once.c              $B/lex/y.tab.o
dep 'lex ldefs.c -> lmain.o'   src/cmd/lex/ldefs.c             $B/lex/lmain.o
dep 'refer refer..c'           src/cmd/refer/refer..c          $B/refer/refer1.o
dep 'tbl t..c'                 src/cmd/tbl/t..c                $B/tbl/t1.o
# yacc's dextern is included by y1..y4 and itself includes "files".
dep 'yacc dextern'             src/cmd/yacc/dextern            $B/yacc/y1.o
dep 'yacc files'               src/cmd/yacc/files              $B/yacc/y1.o

# --- ordinary headers, per program ------------------------------------------
dep 'spell hash.h'             src/cmd/spell/hash.h            $B/spell/hashlook.o
dep 'spell huff.h'             src/cmd/spell/huff.h            $B/spell/hashlook.o
dep 'pic pic.h'                src/cmd/pic/pic.h               $B/pic/main.o
dep 'eqn e.h'                  src/cmd/eqn/e.h                 $B/eqn/main.o
dep 'sh defs.h'                src/cmd/sh/defs.h               $B/sh/main.o
dep 'troff tdef.h'             src/cmd/troff/tdef.h            $B/troff/n1.o
dep 'nroff tdef.h'             src/cmd/troff/tdef.h            $B/nroff/n1.o

# --- generators: a new yacc or lex means regenerated tables ------------------
dep 'grammar -> pic tables'    src/cmd/pic/picy.y              $B/pic/y.tab.c
dep 'lexer -> pic scanner'     src/cmd/pic/picl.l              $B/pic/lex.yy.c
dep 'grammar -> eqn tables'    src/cmd/eqn/eqn.y               $B/eqn/y.tab.c

# --- the rootfs the driver actually links against ---------------------------
# Staleness incident #4: `make libv8c` refreshed build/ but not the copy under
# rootfs/lib that cc links, so spell's sscanf kept reaching the host's.
dep 'libv8c -> rootfs copy'    $B/libc/libv8c.a                rootfs/lib/libv8c.a
dep 'crt0 -> rootfs copy'      $B/crt0.o                       rootfs/lib/crt0.o
dep 'cc -> rootfs copy'        $B/cc/v8cc                      rootfs/bin/cc
dep 'ccom -> rootfs copy'      $B/ccom-arm64/v8ccom            rootfs/lib/ccom
dep 'yaccpar -> rootfs copy'   src/cmd/yacc/yaccpar            rootfs/usr/lib/yaccpar
dep 'ncform -> rootfs copy'    src/cmd/lex/ncform              rootfs/usr/lib/lex/ncform
dep '/etc/group -> rootfs'     src/v8/etc/group                rootfs/etc/group
dep '/etc/ttys -> rootfs'      src/v8/etc/ttys                 rootfs/etc/ttys

# --- /bin: the jail's contents must track the shim --------------------------
# These are the rules that had no library dependency at all.  Editing the shim
# left every /bin binary stale, which is worse than usual: the jail is what the
# shim implements, so a stale /bin means the chroot silently is not one.
dep 'shim -> /bin/cat'         $B/v8sys/libv8sys.a             $B/bin/cat
dep 'shim -> /bin/rm'          $B/v8sys/libv8sys.a             $B/bin/rm
dep 'shim -> make'             $B/v8sys/libv8sys.a             $B/make/make
dep 'libc -> /bin/cat'         $B/libc/libv8c.a                $B/bin/cat
dep 'cat.c -> /bin/cat'        src/cmd/cat.c                   $B/bin/cat
dep '/bin/cat -> rootfs copy'  $B/bin/cat                      rootfs/bin/cat
dep 'sh -> rootfs /bin/sh'     $B/sh/sh                        rootfs/bin/sh
# yacc and lex belong in /bin because a program built from its own V8 makefile
# runs `yacc parser.y`.  Until they were installed the jail refused it outright,
# which is the guard working: they had been built and used by this Makefile for
# months without ever being reachable inside the world they belong to.
dep 'yacc -> rootfs /bin/yacc' $B/yacc/yacc                    rootfs/bin/yacc
dep 'lex -> rootfs /bin/lex'   $B/lex/lex                      rootfs/bin/lex
dep 'make -> rootfs /bin/make' $B/make/make                    rootfs/bin/make
# make's `defs` is a header under a name that is not .h -- the fourth file of
# that shape in this tree, after lex's once.c, tbl's t..c and refer's refer..c.
dep 'make defs'                src/cmd/make/defs               $B/make/main.o
dep 'make gram.y'              src/cmd/make/gram.y             $B/make/gram.c

# --- make's built-in yacc/lex rules must stay dead ---------------------------
# GNU make ships `%.c: %.y` whose recipe ends `mv -f y.tab.c $@`.  cgram.y sits
# next to the CHECKED-IN cgram.c that the ccom rules use instead of running
# yacc, so the built-in rule can overwrite authentic source: `make -B` on
# anything reaching cgram.o rewrote it in place with a different yaccpar and
# absolute paths in its `# line` directives.  Caught by `git diff`, nothing else.
#
# Asserted with -n, so nothing is written even if the guard has been removed.
for t in $B/ccom-arm64/cgram.o $B/ccom/cgram.o; do
	if $MAKE -n -B "$ROOT/$t" 2>/dev/null | grep -q 'mv -f y\.tab\.c'; then
		fail=$((fail+1))
		echo "FAIL make's built-in yacc rule can still write into src/ (via $t)"
	else
		pass=$((pass+1))
	fi
done
# The control: the file it would have clobbered is the one we care about, and it
# must be present and checked in rather than generated.
if [ -f "$ROOT/src/cmd/ccom/common/cgram.c" ]; then
	pass=$((pass+1))
else
	fail=$((fail+1)); echo "FAIL cgram.c is missing -- it is checked-in source, not a build product"
fi

# --- Wave A: installed commands and the data two of them read ----------------
dep 'wave A command source'   src/cmd/cal.c   $B/bin/cal
dep 'wave A command install'  $B/bin/cal      rootfs/bin/cal
# units and ptx open /usr/lib/units and /usr/lib/eign by absolute path at RUN
# time, so a missing install shows up as "no table" or "Cannot open  file
# /usr/lib/eign" rather than as a build failure.  Their rules are asserted by
# the delete-and-restore loop further down rather than with dep(), because
# dep() touches its INPUT and the input here lives in third_party/, which this
# tree treats as immutable -- an interrupted run would leave a pristine file
# with a rewritten mtime, and git would not show it.
if cmp -s "$ROOT/rootfs/usr/lib/units" \
          "$ROOT/third_party/Research-Unix-v8/v8/usr/lib/units"; then
	pass=$((pass+1))
else
	fail=$((fail+1)); echo "FAIL installed units table differs from upstream's"
fi

# --- grap, which joined the default build with struct-by-value ---------------
dep 'grap grap.h'              src/cmd/grap/grap.h             $B/grap/plot.o
dep 'grap grammar'             src/cmd/grap/grap.y             $B/grap/y.tab.c
dep 'grap lexer'               src/cmd/grap/grapl.l            $B/grap/lex.yy.c
dep 'grap install'             $B/grap/grap                    rootfs/usr/bin/grap
# grap.defines is RUNTIME data, so it must install -- and must NOT be a build
# input.  An earlier version of the Makefile made every grap object depend on
# it, calling it an #included non-header.  It is not one; the pair below is what
# says so, and the nodep is the half that would have caught the mistake.
dep 'grap defines install'     src/cmd/grap/grap.defines       rootfs/usr/lib/grap.defines
nodep 'grap.defines is not a build input' src/cmd/grap/grap.defines $B/grap/plot.o

# --- negative controls: the suite must be able to say "no" ------------------
nodep 'spell does not reach sh'   src/cmd/spell/huff.h   $B/sh/main.o
nodep 'tbl does not reach eqn'    src/cmd/tbl/t..c       $B/eqn/main.o
nodep 'refer does not reach libc' src/cmd/refer/refer..c $B/libc/gen/malloc.o
nodep 'grap does not reach pic'   src/cmd/grap/grap.h    $B/pic/main.o

# --- deleted rootfs data files must come back -------------------------------
# A directory stamp recorded "the install ran", which is not the question:
# deleting one installed table left the stamp alone, so make did not restore it.
for f in rootfs/usr/lib/term/tab.37 rootfs/usr/lib/font/dev202/DESC.out \
         rootfs/usr/lib/font/dev202/R.out rootfs/lib/libv8c.a \
         rootfs/usr/lib/grap.defines rootfs/usr/lib/units rootfs/usr/lib/eign \
         rootfs/bin/cal; do
	rm -f "$ROOT/$f"
	$MAKE >/dev/null 2>&1
	if [ -f "$ROOT/$f" ]; then
		pass=$((pass+1))
	else
		fail=$((fail+1)); echo "FAIL deleting $f did not restore it"
	fi
done

# --- and the build must settle ----------------------------------------------
# A build that never reaches idle hides real work in the noise, and tempts the
# shortcuts that caused the staleness bugs in the first place.  Before this
# change a `make` with nothing touched recompiled 39 objects, every time.
$MAKE >/dev/null 2>&1
busy=$($MAKE 2>&1 | grep -cE '\-c \-o ')
if [ "$busy" -eq 0 ]; then
	pass=$((pass+1))
else
	fail=$((fail+1))
	echo "FAIL build does not settle: $busy objects recompiled with nothing changed"
fi

echo "deps: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
