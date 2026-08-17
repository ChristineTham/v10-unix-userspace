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

ROOT=$(cd "$(dirname "$0")/../.." && pwd)		# the release tree, v8/
REPO=$(cd "$ROOT/.." && pwd)			# the repository above it
MAKE=${MAKE:-make}
cd "$ROOT" || exit 1

B=build/stage0
pass=0 fail=0
TMP=${TMPDIR:-/tmp}/depstest.$$
mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT

# Settle the tree first: every case assumes its target starts up to date.
#
# The v8sys test binary is named explicitly because the default target does not
# build it -- it is a test, not part of the world -- and two cases below assert
# its edges.  A `dep' whose target is missing is a failure rather than a skip,
# which is right, so it has to be here rather than left to whoever ran make
# last.  Before the sleep, or its mtime lands in the same second as the touches
# that follow and make 3.81 cannot tell them apart.
# p9clprobe and the sanitized server are named for the same reason: neither is
# part of the world, both are prerequisites of test-streams, and the cases below
# assert their edges.
if ! $MAKE >/dev/null 2>&1 || ! $MAKE "$ROOT/$B/v8sys/test" >/dev/null 2>&1 ||
   ! $MAKE "$ROOT/$B/v8sys/p9clprobe" >/dev/null 2>&1 ||
   ! $MAKE "$ROOT/$B/v8fsd/v8fsd-ubsan" >/dev/null 2>&1; then
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
#
# SEEN ONCE AND NOT EXPLAINED, 2026-08-11.  `make -j8' followed immediately by
# `make test' reported BOTH spell cases -- hash.h and huff.h against
# hashlook.o -- as "touching X did not make Y stale".  Running this suite alone
# seconds later: 357 passed, 0 failed.  The change under test was in
# shim/v8sys/syscall.c, which hashlook.o does not depend on.
#
# THE OBVIOUS EXPLANATION IS WRITTEN DOWN HERE BECAUSE IT IS WRONG, and it was
# asserted as fact in commit b73fa43's message before anyone tried to reproduce
# it.  The story was the whole-second mtime trap: `touch' below sets the input
# to NOW, make treats EQUAL mtimes as up to date, so an object compiled in that
# same second is legitimately not stale.  Plausible, and CLAUDE.md documents the
# granularity.  It does not survive measurement --
#
#   three deliberate attempts to force the collision (touch both headers, `make
#   -j8' to rebuild hashlook.o, then run this suite) came back 357/0 each time,
#   because ~186 cases run before the spell pair and seconds elapse; and
#
#   in the failing run the object should not have been rebuilt at all, so its
#   mtime was already old when the touch happened.
#
# What IS established: both failures share one TARGET, so the fault points at
# hashlook.o rather than at either header; there is no hashlook.d, so these
# edges are explicit in the Makefile and a stale auto-dependency file is not it;
# and dep()'s control half passed, so the target really was up to date first.
#
# If it recurs, capture `stat -f '%Sm %N' -t '%H:%M:%S'` on the object and both
# headers AT THAT MOMENT, plus `make -q -d` for the target.  Without those it is
# not decidable after the fact.  Do NOT "fix" the touch on the strength of the
# story above: that would be a change to a guard justified by a mechanism that
# failed to reproduce.
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
dep '/etc/group -> rootfs'     src/etc/group                rootfs/etc/group
dep '/etc/ttys -> rootfs'      src/etc/ttys                 rootfs/etc/ttys

# libm, which is a stub because V8's was one -- shim/libm/dummy.c has the
# account.  It is in the graph rather than made once and forgotten because the
# driver now RESOLVES -l against the rootfs (libpath() in src/cmd/cc.c): if this
# archive goes missing, -lm silently reaches the macOS SDK again, and the way
# that failed was a link error naming _errno and neither libm nor the jail.
dep 'libm stub -> object'      shim/libm/dummy.c               $B/libm/dummy.o
dep 'libm object -> archive'   $B/libm/dummy.o                 rootfs/usr/lib/libm.a

# libtermcap, the same shape as libm and for a sharper reason: it is a real
# library with three members, so a source that stops reaching the archive is a
# missing capability rather than a missing stub.  All three are named, because
# the archive rule lists them explicitly and a dropped name leaves a stale
# member behind -- the `ar r' trap the libv8c rule documents at length.
dep 'termcap.c -> object'      src/lib/libtermlib/termcap.c    $B/termlib/termcap.o
dep 'tgoto.c -> object'        src/lib/libtermlib/tgoto.c      $B/termlib/tgoto.o
dep 'tputs.c -> object'        src/lib/libtermlib/tputs.c      $B/termlib/tputs.o
dep 'termcap.o -> archive'     $B/termlib/termcap.o            $B/termlib/libtermcap.a
dep 'archive -> rootfs copy'   $B/termlib/libtermcap.a         rootfs/usr/lib/libtermcap.a

# A HARD LINK CANNOT BE TESTED BY A STALENESS PROBE, and the case written for
# it here has been deleted rather than left failing.
#
# libtermlib.a is `ln libtermcap.a libtermlib.a', which is upstream's own
# install.  So the obvious edge -- touch the prerequisite, require the target
# to go stale -- is not merely hard, it is IMPOSSIBLE: the two names are one
# inode (measured: same inode number, and touching either moves both mtimes),
# so the prerequisite IS the target and can never be newer than itself.
#
# Nor does it need the edge.  The rootfs copy is `cp', which opens the existing
# file and truncates rather than replacing it, so the link survives a rebuild
# and both names get the new bytes with no second action.  The only way the
# pair can break is if that cp becomes an install or a mv -- and what catches
# THAT is an inode comparison, not a timestamp one.  tests/wavea has it:
# `libtermlib.a is a hard link to libtermcap.a', compared by inode precisely
# because two identical copies would pass a cmp and would not be what V8
# shipped.

# ul is the library's only installed consumer, so it is the edge that says the
# archive is on a link line at all rather than merely built.
dep 'ul.c -> ul'               src/cmd/ul/ul.c                 $B/bin/ul
dep 'libtermcap -> ul'         $B/termlib/libtermcap.a         $B/bin/ul
dep 'ul -> rootfs copy'        $B/bin/ul                       rootfs/usr/bin/ul

# The termcap DATABASE is data, and nothing compiles it -- so if this edge is
# missing, an edited /etc/termcap simply never reaches the world and every
# terminal keeps its old capabilities with nothing to say so.
dep '/etc/termcap -> rootfs'   src/etc/termcap                 rootfs/etc/termcap

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
dep 'yacc -> rootfs /bin/yacc' $B/yacc/yacc                    rootfs/usr/bin/yacc
dep 'lex -> rootfs /bin/lex'   $B/lex/lex                      rootfs/usr/bin/lex
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
dep 'wave A command install'  $B/bin/cal      rootfs/usr/bin/cal

# Wave A2 batch 2b.  One of the five is here rather than all five: the generic
# $(V8BIN) pattern rule builds them all identically, so five cases would assert
# the same edge five times.  `stty' is the one worth naming because it is the
# only one of the five that lands in /bin rather than /usr/bin -- the
# destination is DERIVED from Bell Labs' tables, so a case that fixes the path
# is also asserting the derivation.
dep 'stty source -> /bin/stty' src/cmd/stty.c                 $B/bin/stty
dep 'stty install'             $B/bin/stty                    rootfs/bin/stty
# mc is the only one of the five whose source changed, and its change is an
# INCLUDE: `#include "/usr/jerq/include/jioctl.h"' became `<jioctl.h>', the
# same change ls.c and termcap.c already carry.  So the header has to reach it.
#
# THE OBVIOUS CASE IS WRONG AND WAS MEASURED BEFORE THAT WAS NOTICED.
# `dep rootfs/usr/include/jioctl.h -> $B/bin/mc' fails: the rootfs copy is a
# BUILD PRODUCT, and what the rule names is the stamp beside it, so touching
# the copy makes nothing stale.  The source-side edge -- third_party's
# jerq/include/jioctl.h -> the stamp -- is real and is deliberately NOT
# asserted here, for the reason the units/eign block above gives: dep() touches
# its input, and an interrupted run would leave a pristine third_party file
# with a rewritten mtime that git cannot show.
dep 'rootfs headers -> mc'     rootfs/usr/include/.stamp      $B/bin/mc

# --- Wave A2 batch 2c: four directory programs, four build idioms -----------
# expr is a grammar and nothing else -- one .y, one object, and the scanner is
# C in the third section rather than a lex file.  regexp.h is expr's own and is
# reached with -I, so it is a real edge and not a system header.
dep 'expr grammar -> tables'   src/cmd/expr/expr.y            $B/expr/y.tab.c
dep 'expr regexp.h -> object'  src/cmd/expr/regexp.h          $B/expr/expr.o
# m4 is grap's shape without a lexer.  Upstream says `m4.o m4ext.o m4macs.o :
# m4.h' and deliberately LEAVES m4y.o OUT; m4y.y does not include the header,
# so the nodep is what keeps this tree from widening a claim upstream did not
# make.  A dependency this port ADDS is a statement about the program.
dep 'm4 grammar -> tables'     src/cmd/m4/m4y.y               $B/m4/y.tab.c
dep 'm4.h -> m4ext.o'          src/cmd/m4/m4.h                $B/m4/m4ext.o
nodep 'm4.h is not m4y.o edge' src/cmd/m4/m4.h                $B/m4/m4y.o
# diff3 is the SPELL SHAPE: /usr/bin/diff3 is a shell script and the binary it
# execs is /usr/lib/diff3.  Both halves have to install or the command is a
# script calling something absent -- which fails at RUN time with `not found',
# not at build time, so nothing else would say so.
dep 'diff3 source -> binary'   src/cmd/diff3/diff3.c          $B/diff3/diff3.o
dep 'diff3 binary -> /usr/lib' $B/bin/diff3                   rootfs/usr/lib/diff3
dep 'diff3 script -> /usr/bin' src/cmd/diff3/diff3.sh         rootfs/usr/bin/diff3
# THE FIVE COMMANDS V8 SHIPPED AS SHELL SCRIPTS, diff3's shape without the
# compiled half.  Their sources sit in TWO directories on purpose: false has
# upstream source at usr/src/cmd/false.sh, the other four have no usr/src copy
# at all and what was imported is the shipped artefact, which tools/import.sh
# maps to src/bin.  Both spellings are asserted here so a future tidy-up that
# moved one would go red.
dep 'true script -> /bin'      src/bin/true                   rootfs/bin/true
dep 'false script -> /bin'     src/cmd/false.sh               rootfs/bin/false
dep 'nohup script -> /bin'     src/bin/nohup                  rootfs/bin/nohup
dep 'dirname script -> /usr/bin' src/bin/dirname              rootfs/usr/bin/dirname
dep 'whois script -> /usr/bin' src/bin/whois                  rootfs/usr/bin/whois
# ed/e and compress/uncompress/zcat are HARD LINKS and deliberately have NO
# case here, for the reason libtermlib's does not: the prerequisite and the
# target are ONE INODE, so `touch' moves both mtimes and the target can never
# be newer than itself.  They are asserted by inode in tests/wavea instead.
# pack: two programs from one directory, and pcat is a HARD LINK to unpack.
# The link is asserted by INODE in tests/wavea, not here -- tests/deps cannot
# do it, for ex/vi's reason: the prerequisite IS the target, so it can never be
# newer than itself.  What is assertable is that each program tracks its own
# source and neither tracks the other's.
dep 'pack source -> pack'      src/cmd/pack/pack.c            $B/pack/pack.o
dep 'unpack source -> unpack'  src/cmd/pack/unpack.c          $B/pack/unpack.o
nodep 'pack.c is not unpack'   src/cmd/pack/pack.c            $B/pack/unpack.o
# units and ptx open /usr/lib/units and /usr/lib/eign by absolute path at RUN
# time, so a missing install shows up as "no table" or "Cannot open  file
# /usr/lib/eign" rather than as a build failure.  Their rules are asserted by
# the delete-and-restore loop further down rather than with dep(), because
# dep() touches its INPUT and the input here lives in third_party/, which this
# tree treats as immutable -- an interrupted run would leave a pristine file
# with a rewritten mtime, and git would not show it.
if cmp -s "$ROOT/rootfs/usr/lib/units" \
          "$REPO/third_party/Research-Unix-v8/v8/usr/lib/units"; then
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

# --- awk: the tree's only TWO-STEP generator chain --------------------------
# Everything else here generates a source file from one input.  awk's proctab.c
# is written by a program this build first has to compile and run, and that
# program reads a header a DIFFERENT generator wrote:
#
#	awk.g.y --yacc--> y.tab.c + y.tab.h
#	maketab.c + y.tab.h --v8cc--> maketab --run--> proctab.c --v8cc--> proctab.o
#
# So a new token in the grammar has to reach proctab.o through four rules, and
# upstream's own makefile says in its first four lines that theirs does not do
# it ("This makefile is wrong -- it doesn't properly recompile everything when a
# new token is added to awk.g.y.  Watch out!").  The whole point of the block in
# our Makefile is that this chain exists; these cases are what says it is real
# rather than that the files happen to be present.
#
# The interesting one is the LAST: it walks the entire chain in a single case,
# which is the only assertion that would notice the middle of it being cut.
dep 'awk grammar -> tables'    src/cmd/awk/awk.g.y             $B/awk/y.tab.c
dep 'awk lexer -> scanner'     src/cmd/awk/awk.lx.l            $B/awk/lex.yy.c
dep 'awk header -> an object'  src/cmd/awk/awk.h               $B/awk/run.o
dep 'awk tables -> maketab'    $B/awk/y.tab.c                  $B/awk/maketab
dep 'maketab source -> maketab' src/cmd/awk/maketab.c          $B/awk/maketab
dep 'maketab -> proctab.c'     $B/awk/maketab                  $B/awk/proctab.c
dep 'proctab.c -> proctab.o'   $B/awk/proctab.c                $B/awk/proctab.o
dep 'awk grammar -> proctab.o' src/cmd/awk/awk.g.y             $B/awk/proctab.o
dep 'awk install'              $B/bin/awk                      rootfs/usr/bin/awk
# THE OBVIOUS NEGATIVE CASE HERE IS FALSE, and it was written and measured
# before that was noticed: `nodep maketab.c -> bin/awk' fails, because touching
# maketab.c legitimately DOES make awk stale -- maketab.c builds maketab, which
# writes proctab.c, which compiles into the link.  A build tool's SOURCE is a
# transitive prerequisite of the program even though its OBJECT is not linked,
# and "is it a component" and "is it a prerequisite" are different questions
# that a dependency test can only answer the second of.  The first is asserted
# where it is observable instead: tests/wavea checks that maketab is not
# installed, since a tool in $(ROOTFS) would be a component by any measure.

# --- Wave A2 batch 2d: seven programs and a library -------------------------
# hoc: a grammar plus four .c.  Upstream's dependency lines are
#	hoc.o code.o init.o symbol.o: hoc.h
#	code.o init.o symbol.o:       x.tab.h
# and math.o is deliberately in NEITHER -- it includes <math.h> and nothing of
# hoc's -- so the nodep is what keeps this tree from widening a claim upstream
# did not make.  Same discipline as m4y.o above.
dep 'hoc grammar -> tables'    src/cmd/hoc/hoc.y               $B/hoc/y.tab.c
dep 'hoc grammar -> object'    src/cmd/hoc/hoc.y               $B/hoc/hocgram.o
dep 'hoc.h -> code.o'          src/cmd/hoc/hoc.h               $B/hoc/code.o
nodep 'hoc.h is not math.o edge' src/cmd/hoc/hoc.h             $B/hoc/math.o
# The TOKEN NUMBERS have to reach the three objects that read y.tab.h, and this
# is the case that says so: an edit to the grammar restages the header and
# every one of them recompiles.  Upstream gets there through x.tab.h, a file
# nothing includes, whose mtime moves only when the CONTENT of y.tab.h changes
# -- content-addressed rebuild avoidance in 1984 make, spelled with `cmp -s'.
# This port reaches the same correctness through the y.tab.c edge and skips the
# optimisation; see the Makefile for why (no grouped targets in make 3.81).
dep 'hoc grammar -> init.o'    src/cmd/hoc/hoc.y               $B/hoc/init.o
dep 'hoc install'              $B/bin/hoc                      rootfs/usr/bin/hoc
# p: three objects, and upstream's one dependency line is `pad.o: pad.h'.
dep 'pad.h -> pad.o'           src/cmd/p/pad.h                 $B/p/pad.o
nodep 'pad.h is not p.o edge'  src/cmd/p/pad.h                 $B/p/p.o
dep 'spname -> p'              src/cmd/p/spname.c              $B/p/spname.o
dep 'p install'                $B/bin/p                        rootfs/usr/bin/p
# pp: A LEX FILE WITH NO GRAMMAR, which is an idiom nothing before 2d had --
# every other lexer here is half of a yacc/lex pair and depends on y.tab.c for
# its token numbers.  This scanner stands alone, so the ONLY generated input is
# lex.yy.c and pp.h is what the two objects share.
dep 'pp lexer -> scanner'      src/cmd/pp/scan.l               $B/pp/lex.yy.c
dep 'pp lexer -> object'       src/cmd/pp/scan.l               $B/pp/scan.o
dep 'pp.h -> scan.o'           src/cmd/pp/pp.h                 $B/pp/scan.o
dep 'pp.h -> pp.o'             src/cmd/pp/pp.h                 $B/pp/pp.o
dep 'dev.h -> pp.o'            src/cmd/pp/dev.h                $B/pp/pp.o
nodep 'dev.h is not scan.o edge' src/cmd/pp/dev.h              $B/pp/scan.o
dep 'pp install'               $B/bin/pp                       rootfs/usr/bin/pp

# struct -- batch 2e.  def.h is the one that matters and is asserted first:
# it holds `#define VERT long', so a stale object here is a graph BUILT at one
# cell width and READ at another, which is silent.  Upstream's makefile spells
# the same edge (`main.o $(0FILES.o) ... : def.h'), and the numbered headers
# reach only their own group -- asserted both ways, since a too-wide edge
# rebuilds the world on every touch and nothing would notice.
dep 'struct def.h -> 1.hash.o'  src/cmd/struct/def.h            $B/struct/1.hash.o
dep 'struct def.h -> 3.then.o'  src/cmd/struct/def.h            $B/struct/3.then.o
dep 'struct 2.def.h -> 2.dfs.o' src/cmd/struct/2.def.h          $B/struct/2.dfs.o
nodep '2.def.h is not a 1.* edge' src/cmd/struct/2.def.h        $B/struct/1.hash.o
dep 'struct -> structure'       src/cmd/struct/2.dfs.c          $B/bin/structure
dep 'structure install'         $B/bin/structure                rootfs/usr/lib/struct/structure
# beautify: the lex file with no yacc file beside it (pp's idiom), and the
# CHECKED-IN y.tab.h, which both lextab.o and tree.o include.  A stale y.tab.h
# is a token-number skew and those produce wrong parses rather than errors,
# so the edge is asserted rather than left to the comment in upstream's
# makefile that names it.
dep 'struct lexer -> scanner'   src/cmd/struct/lextab.l         $B/struct/lex.yy.c
dep 'struct lexer -> object'    src/cmd/struct/lextab.l         $B/struct/lextab.o
dep 'y.tab.h -> lextab.o'       src/cmd/struct/y.tab.h          $B/struct/lextab.o
dep 'y.tab.h -> tree.o'         src/cmd/struct/y.tab.h          $B/struct/tree.o
dep 'b.h -> beauty.o'           src/cmd/struct/b.h              $B/struct/beauty.o
dep 'libl.a -> beautify'        $B/libl/libl.a                  $B/bin/beautify
dep 'beautify install'          $B/bin/beautify                 rootfs/usr/lib/struct/beautify
# The COMMAND is the shell script, and it is a real prerequisite of the
# installed file -- the same shape as diff3.sh.
dep 'struct.sh -> command'      src/cmd/struct/struct.sh        rootfs/usr/bin/struct

# csh -- task #93.  Nineteen objects share three headers, and upstream's own
# makefile names only one of them (`csh: ${OBJS} sh.local.h'), so the other two
# edges are this port's addition and are exactly the ones worth asserting: a
# stale sh.proc.h is a struct-layout skew between sh.proc.o and sh.sem.o, and
# that header is where the 16-bit p_pid that hung the shell lived.  A build
# that half-recompiled it would give two objects different ideas of where
# p_flags sits.
dep 'csh sh.h -> sh.o'          src/cmd/csh/sh.h                build/stage0/csh/sh.o
dep 'csh sh.local.h -> sh.o'    src/cmd/csh/sh.local.h          build/stage0/csh/sh.o
dep 'csh sh.proc.h -> sh.proc.o' src/cmd/csh/sh.proc.h          build/stage0/csh/sh.proc.o
dep 'csh sh.proc.h -> sh.sem.o' src/cmd/csh/sh.proc.h           build/stage0/csh/sh.sem.o
dep 'csh sh.dir.h -> sh.dir.o'  src/cmd/csh/sh.dir.h            build/stage0/csh/sh.dir.o
dep 'csh sh.proc.c -> sh.proc.o' src/cmd/csh/sh.proc.c          build/stage0/csh/sh.proc.o
# The library chain, source -> archive -> program.  libjobs is one member and
# csh calls into it 88 times; the failure it guards against is not a link error
# but a SILENT one, because macOS ships System V sigset/sighold/sigrelse and a
# csh that lost the archive would resolve them from -lSystem and run on the
# host's signal semantics.
dep 'sigset.c -> libjobs.a'     src/lib/libjobs/sigset.c        build/stage0/libjobs/libjobs.a
dep 'libjobs.a -> bin/csh'      build/stage0/libjobs/libjobs.a  build/stage0/bin/csh
dep 'csh install'               build/stage0/bin/csh            rootfs/bin/csh
dep 'libjobs.a install'         build/stage0/libjobs/libjobs.a  rootfs/usr/lib/libjobs.a
# NEGATIVE CONTROL.  nfunc.c sits in the csh directory and is an alternate
# sh.func.c that upstream's OBJS does not name, so it must reach nothing.  It
# is also the file the wavea pid-width sweep deliberately does not scan, and
# these two cases are the same claim measured from opposite ends: no object, no
# prerequisite.
nodep 'nodep nfunc.c -> bin/csh' src/cmd/csh/nfunc.c            build/stage0/bin/csh

# libl -- the SECOND library import after libtermlib, and pp is its only
# consumer.  The chain that matters is source -> archive -> program: a lex
# program with no yywrap() of its own does not link without it, and the failure
# is an undefined symbol rather than anything subtler.
dep 'libl source -> archive'   src/lib/libl/yywrap.c           $B/libl/libl.a
dep 'libl reject -> archive'   src/lib/libl/reject.c           $B/libl/libl.a
dep 'libl archive -> pp'       $B/libl/libl.a                  $B/bin/pp
dep 'libl install'             $B/libl/libl.a                  rootfs/usr/lib/libl.a
# calendar: five artefacts, three of them binaries that install to /usr/lib and
# two of them shell scripts.  The objects are asserted at all only because the
# link rule is a STATIC pattern rule: written as a chained pattern rule
# (`$(BINDIR)/calendar%: .../calendar%.o') make classed each object as an
# INTERMEDIATE and deleted it after every build -- measured, and these cases
# would have had nothing to name.
dep 'calendar1 source'         src/cmd/calendar/calendar1.c    $B/calendar/calendar1.o
dep 'calendar2 source'         src/cmd/calendar/calendar2.c    $B/calendar/calendar2.o
dep 'calendar1 -> /usr/lib'    $B/bin/calendar1                rootfs/usr/lib/calendar1
dep 'calendar script'          src/cmd/calendar/calendar       rootfs/usr/bin/calendar
dep 'calendar3 script'         src/cmd/calendar/calendar3      rootfs/usr/lib/calendar3
nodep 'calendar1.c is not calendar2' src/cmd/calendar/calendar1.c $B/calendar/calendar2.o
# newgrp, showq and dmesg install OUTSIDE /usr/bin, which is derived from Bell
# Labs' own tables rather than spelled in our Makefile -- so these three cases
# are the only thing that would notice $(call v8dest,...) starting to answer
# /usr/bin for them.
dep 'newgrp -> /bin'           $B/bin/newgrp                   rootfs/bin/newgrp
dep 'showq -> /etc'            $B/bin/showq                    rootfs/etc/showq
dep 'dmesg -> /etc'            $B/bin/dmesg                    rootfs/etc/dmesg
# showq's four includes were ABSOLUTE ("/usr/sys/h/param.h") and are now
# <sys/...>.  That makes our patched sys/param.h a real build input, which it
# was not before, and this is the edge that says the change actually landed --
# an absolute include resolves outside the tree and would produce no edge at
# all.
dep 'sys/param.h -> showq.o'   src/include/sys/param.h         $B/showq/showq.o
# The other three -- sys/stream.h, sys/inode.h, sys/conf.h -- CANNOT be
# asserted here and the reason is worth writing down rather than discovering
# twice.  This port patches none of them, so they reach the rootfs straight
# from third_party/ via $(ROOTFS_INC), and there is no file under src/include/
# to name.  dep() would have to touch a pristine upstream file, which is the
# same objection the units/eign block above raises.  What is assertable is the
# stamp, and the stamp is a prerequisite of EVERY v8cc object through
# $(V8CC_DEPS), so it says nothing about showq in particular.  param.h alone
# carries the claim, and it is the only one of the four this port modifies.

# --- libplot: the ar-as-source-bundle idiom -------------------------------
# The sources live INSIDE tek.c.a and plot.c.a, so the bundle is the only
# prerequisite there is -- there are no per-member edges to assert, because
# there are no per-member files.  One rule per archive, which is upstream's own
# granularity; src/libplot/PORTING.md says why a stamp-and-wildcard scheme
# cannot work here ($(wildcard) is expanded when make READS the rule, and the
# members do not exist until the extraction has run).
dep 'plot bundle -> libplot.a'  src/libplot/libplot/plot.c.a  $B/libplot/libplot.a
dep 'tek bundle -> lib4014.a'   src/libplot/lib4014/tek.c.a   $B/lib4014/lib4014.a
dep 'libplot.a installs'        $B/libplot/libplot.a          rootfs/usr/lib/libplot.a
dep 'lib4014.a installs'        $B/lib4014/lib4014.a          rootfs/usr/lib/lib4014.a
# The two bundles are independent -- they are different devices, not layers.
nodep 'tek bundle is not libplot.a' src/libplot/lib4014/tek.c.a $B/libplot/libplot.a
# graph and prof name -lplot on their link lines and need NOTHING from it (see
# PORTING.md: their plot calls are macros in <iplot.h>).  The edge is asserted
# anyway, because what the archive is there for is to stop `-lplot' escaping to
# the host SDK -- libpath() resolves -lNAME against $V8ROOT/usr/lib first, so a
# missing archive is a silent escape rather than a link error.
dep 'libplot.a -> graph'        $B/libplot/libplot.a          $B/bin/graph
dep 'graph source -> graph'     src/cmd/graph/graph.c         $B/graph/graph.o
dep 'prof source -> prof'       src/cmd/prof/prof.c           $B/prof/prof.o
# tek is the ONE consumer that genuinely needs the device library: driver.c
# dispatches through a table of 28 real function pointers.  Drop lib4014 from
# its link and it fails to link, which is what this edge stands for.
dep 'lib4014.a -> tek'          $B/lib4014/lib4014.a          $B/bin/tek
dep 'driver source -> tek'      src/cmd/plot/driver.c         $B/plot/driver.o
nodep 'lib4014 is not a graph edge' src/libplot/lib4014/tek.c.a $B/graph/graph.o

# --- qed: Thompson's editor -------------------------------------------------
# Upstream's whole dependency statement is one line -- `$(FILES): vars.h' --
# and it is copied rather than widened, which is the same discipline m4y.o and
# hoc's math.o get.  vars.h is where the four signal-handler variables live, so
# the edge is load-bearing: widening them from int to long is a change every
# object has to see.
dep 'qed vars.h -> main.o'     src/cmd/qed/vars.h              $B/qed/main.o
dep 'qed vars.h -> getfile.o'  src/cmd/qed/vars.h              $B/qed/getfile.o
dep 'qed vars.h -> subs.o'     src/cmd/qed/vars.h              $B/qed/subs.o
dep 'qed source -> object'     src/cmd/qed/blkio.c             $B/qed/blkio.o
dep 'qed install'              $B/bin/qed                      rootfs/usr/bin/qed
# misc.c re-declares savint, which getfile.c DEFINES.  There is no header
# between them -- upstream's extern is inside error() -- so make cannot express
# the coupling and neither can this file.  The nodep records that it is a real
# relationship the build graph does not carry, which is why the PORT comment in
# misc.c is the only thing keeping the two in step.
nodep 'getfile.c is not a misc.o edge' src/cmd/qed/getfile.c   $B/qed/misc.o

# --- Phase 4: libkmemu, and the edges that keep it out of everything else ---
dep 'kmemu source -> archive'  shim/libkmemu/utmp.c            $B/kmemu/libkmemu.a
dep 'kmemu header -> archive'  shim/libkmemu/kmemu.h           $B/kmemu/libkmemu.a
dep 'kmemu -> who'             shim/libkmemu/synth.c           $B/bin/who
dep 'who source -> who'        src/cmd/who.c                   $B/bin/who
dep 'who -> installed who'     $B/bin/who                      rootfs/bin/who
dep 'df source -> df'          src/cmd/df/df.c                 $B/bin/df
dep 'kmemu -> df'              shim/libkmemu/mtab.c            $B/bin/df
dep 'df -> installed df'       $B/bin/df                       rootfs/bin/df
dep 'fstab -> libc'            src/libc/stdio/fstab.c          $B/libc/libv8c.a
dep 'load source -> load'      src/cmd/load/load.c             $B/bin/load
dep 'kmem.c -> load'           shim/libkmemu/kmem.c            $B/bin/load
dep 'load -> installed load'   $B/bin/load                     rootfs/usr/bin/load
dep 'w source -> w'            src/cmd/w/w.c                   $B/bin/w
dep 'kmem.c -> w'              shim/libkmemu/kmem.c            $B/bin/w
dep 'w -> installed w'         $B/bin/w                        rootfs/usr/bin/w
# uptime is a HARD LINK to w, and dep() cannot express it: touching w touches
# the SHARED INODE, so uptime's mtime moves with it and make correctly sees
# nothing to do.  That is the link working, not a missing edge -- two names on
# one inode cannot drift.  What can go wrong is the link being BROKEN and not
# remade, so that is what gets asserted, below, after the restore loop.

# --- the patched headers must reach the rootfs ------------------------------
# ROOTFS_INC_SRC used to glob $(SRC)/include/* only, which matches the `sys'
# DIRECTORY -- and a directory's mtime moves when an entry is created or
# removed, not when a file inside it is edited.  So src/include/sys/param.h
# was copied out once, when the directory first appeared, and never again.
#
# It was found by a mutation test that REFUSED TO FAIL: reverting the DIRSIZ
# patch and rebuilding still passed, because the rootfs kept the old copy.
# That is the whole argument for verifying a guard by breaking the thing.
dep 'src/include/sys -> rootfs headers' src/include/sys/param.h \
                                        rootfs/usr/include/.stamp
dep 'src/include/sys/dir.h too'         src/include/sys/dir.h \
                                        rootfs/usr/include/.stamp
dep 'src/include -> rootfs headers'     src/include/dir.h \
                                        rootfs/usr/include/.stamp
# proc.h joined them with ps: the four pid fields are int here, not short.
dep 'src/include/sys/proc.h too'        src/include/sys/proc.h \
                                        rootfs/usr/include/.stamp
# fstab.h carries FSNMLG, which df and libc's getfsent both compile against;
# mtab.h is patched to agree even though nothing includes it.
dep 'src/include/fstab.h too'           src/include/fstab.h \
                                        rootfs/usr/include/.stamp
dep 'src/include/mtab.h too'            src/include/mtab.h \
                                        rootfs/usr/include/.stamp
# ...and FSNMLG has to reach df and libc, not just the rootfs copy.
dep 'fstab.h -> df'                     src/include/fstab.h     $B/bin/df
dep 'fstab.h -> libc getfsent'          src/include/fstab.h     $B/libc/libv8c.a

# --- the commands upstream keeps in a directory of their own ----------------
# Eleven of them were imported and never built, because the completeness check
# in tests/wavea globbed src/cmd/*.c and a program at src/cmd/mv/mv.c matches
# nothing. Their rules are GENERATED by foreach/eval rather than written out --
# a static pattern rule cannot express src/cmd/%/%.c, since make substitutes
# the stem for only the FIRST % in a prerequisite -- so the graph they produce
# is worth asserting rather than assuming.
V8DIRBIN_NAMES="cp dc ed factor mkdir mv primes rmdir sed fmt tsort"
dep 'mv source -> object'      src/cmd/mv/mv.c          $B/mv/mv.o
dep 'mv object -> binary'      $B/mv/mv.o               $B/bin/mv
dep 'mv -> installed mv'       $B/bin/mv                rootfs/bin/mv
dep 'cp source -> object'      src/cmd/cp/cp.c          $B/cp/cp.o
dep 'cp -> installed cp'       $B/bin/cp                rootfs/bin/cp
# The three multi-object ones are the shape a single-source rule would get
# wrong: sed is sed0.o + sed1.o, and BOTH have to reach the binary.
dep 'sed0 -> object'           src/cmd/sed/sed0.c       $B/sed/sed0.o
dep 'sed1 -> object'           src/cmd/sed/sed1.c       $B/sed/sed1.o
dep 'sed0.o -> sed'            $B/sed/sed0.o            $B/bin/sed
dep 'sed1.o -> sed'            $B/sed/sed1.o            $B/bin/sed
dep 'fmt head.c -> object'     src/cmd/fmt/head.c       $B/fmt/head.o
dep 'fmt head.o -> fmt'        $B/fmt/head.o            $B/bin/fmt
dep 'tsort subs.c -> object'   src/cmd/tsort/subs.c     $B/tsort/subs.o
dep 'tsort subs.o -> tsort'    $B/tsort/subs.o          $B/bin/tsort
# ...and the objects must not land BESIDE the binary. $(BINDIR)/mv is the
# executable, so an object at $(BINDIR)/mv/mv.o makes one path a file and a
# directory at once; ld says "open() failed, errno=21 (Is a directory)", which
# reads as a linker fault and is a layout one. It happened on the way in.
# Structural rather than a dependency question -- there is no rule for the bad
# path, so `make -q' on it errors rather than answering.
badlayout=""
for p in $V8DIRBIN_NAMES; do
	[ -d "$B/bin/$p" ] && badlayout="$badlayout $p"
done
if [ -z "$badlayout" ]; then pass=$((pass+1))
else fail=$((fail+1))
     echo "FAIL objects landed under bin/, colliding with the binary:$badlayout"; fi

# --- section 8a step 3: ps, and the /dev it refuses to start without --------
# Ten objects from one directory, and upstream's whole dependency statement is
# `$(OBJ): ps.h' -- so ps.h is the edge that a *.c glob would miss.
dep 'ps source -> object'      src/cmd/ps/ps.c                 $B/ps/ps.o
dep 'ps.h -> every object'     src/cmd/ps/ps.h                 $B/ps/printp.o
dep 'ps.h -> another object'   src/cmd/ps/ps.h                 $B/ps/getargs.o
dep 'ps objects -> ps'         $B/ps/doselect.o                $B/bin/ps
dep 'ps -> installed ps'       $B/bin/ps                       rootfs/bin/ps
# ps reads /proc, which lives in libkmemu -- so procfs.c is a real edge to the
# binary, exactly as kmem.c is for load.
dep 'procfs.c -> ps'           shim/libkmemu/procfs.c          $B/bin/ps
# ...and the ioctl dispatch it arrives through.
dep 'vfs.h -> ps'              shim/v8sys/vfs.h                $B/bin/ps

# --- section 8a step 4: the image tools, and the headers behind them --------
# The graph is small per program; what makes it worth cases is that these are
# the first commands here whose INSTALL target is not rootfs/bin, that their
# rules are GENERATED by foreach/eval so a typo produces silence rather than an
# error, and that three newly patched headers have to reach the OBJECT.
# quot's source is cmd/quot/quot.c, not cmd/quot.c -- upstream gave it a
# makefile, so import.sh brought a directory.  The Makefile spells that out in
# $(imgsrc_quot) and this loop has to agree, which is the point of asserting it:
# a wrong path there makes the object rule name a file that does not exist, and
# an eval'd rule for a target nothing asks for is silent.
for p in mkfs icheck dcheck clri fsck ncheck quot restor dumpdir; do
	case $p in quot) src=src/cmd/quot/quot.c ;; *) src=src/cmd/$p.c ;; esac
	dep "$p source -> object"     $src           $B/$p/$p.o
	dep "$p object -> binary"     $B/$p/$p.o     $B/bin/$p
	dep "$p -> installed in etc"  $B/bin/$p      rootfs/etc/$p
done
# dump is six objects, so the loop above cannot state it: $(imgobjs) has to
# collect all six and each has to reach the link.  Asserted for every one
# rather than a sample, because the failure mode of a generated rule is a
# MISSING rule, and a missing rule for an object nothing else names is silent
# -- make just relinks whatever .o happens to be there.
for s in dumpmain dumptraverse dumptape dumpoptr dumpitime unctime; do
	dep "dump/$s.c -> its object"  src/cmd/dump/$s.c  $B/dump/$s.o
	dep "dump/$s.o -> the binary"  $B/dump/$s.o       $B/bin/dump
done
dep 'dump.h -> every dump object'  src/cmd/dump/dump.h  $B/dump/dumptape.o
dep 'dump -> installed in etc'     $B/bin/dump          rootfs/etc/dump
# The disk-format headers.  These carry the VAX widths -- dinode 64, filsys
# 4096, daddr_t 4 -- and an edge that stops here means mkfs keeps writing an
# image built from whatever it was compiled against last.  Both ends: the
# generated rootfs view AND the binary, because the DIRSIZ mutation that
# refused to fail (see the note above) did so by way of a stale rootfs copy.
dep 'sys/types.h -> rootfs headers' src/include/sys/types.h \
                                    rootfs/usr/include/.stamp
dep 'sys/ino.h too'                 src/include/sys/ino.h \
                                    rootfs/usr/include/.stamp
dep 'sys/filsys.h too'              src/include/sys/filsys.h \
                                    rootfs/usr/include/.stamp
# ...and to the OBJECT, which is the edge that bites.  Asserted against
# $B/bin/mkfs first, and a mutation dropping $(V8CC_DEPS) from the object rule
# did not fail: the LINK rule names $(V8DEPS), so touching a header still
# restamped the binary -- by relinking a stale object compiled against the old
# header.  A rebuilt mtime on the thing you look at is not a rebuilt anything.
dep 'sys/ino.h -> mkfs object'     src/include/sys/ino.h       $B/mkfs/mkfs.o
dep 'sys/filsys.h -> mkfs object'  src/include/sys/filsys.h    $B/mkfs/mkfs.o
dep 'sys/param.h -> mkfs object'   src/include/sys/param.h     $B/mkfs/mkfs.o
# ...and to the checkers, which read what mkfs wrote.  A checker compiled
# against a different idea of the format than the writer is the one arrangement
# that makes a validator worse than useless: it agrees with itself.
dep 'sys/ino.h -> icheck object'   src/include/sys/ino.h       $B/icheck/icheck.o
# THIS CASE USED TO READ "sys/fblk.h is upstream, so sys/filsys.h stands for
# it", and that was true and is the gap it was standing in for.
# rootfs/usr/include is third_party's pristine headers with ours copied over
# the top (Makefile:1867 then :1873), so a header nobody imported silently
# stays 1985's -- and struct fblk, an on-disk record sitting in the same image
# as dinode and filsys, was the one nobody had. It measured 716 anyway, because
# `int' is 32 here as it was on the VAX and daddr_t came from our patched
# types.h. Right by coincidence, and invisible: a reader auditing "the port's
# headers" would not have found it in src/include at all.
dep 'sys/fblk.h -> icheck object'  src/include/sys/fblk.h      $B/icheck/icheck.o
dep 'sys/filsys.h -> icheck object' src/include/sys/filsys.h   $B/icheck/icheck.o
dep 'sys/ino.h -> dcheck object'   src/include/sys/ino.h       $B/dcheck/dcheck.o
# fsck reads every one of them and is the only program in the tree that reaches
# sys/inode.h -- imported for this step, for IFMT/IFDIR/IFREG alone; struct
# inode itself is never named.  A header with exactly one consumer is the kind
# that quietly stops being a prerequisite, so it gets its own case.
dep 'sys/ino.h -> fsck object'     src/include/sys/ino.h       $B/fsck/fsck.o
dep 'sys/filsys.h -> fsck object'  src/include/sys/filsys.h    $B/fsck/fsck.o
dep 'sys/inode.h -> fsck object'   src/include/sys/inode.h     $B/fsck/fsck.o
# ...and sys/param.h, which is where -DDIRSIZ=14 has to win.  For fsck that is
# not only a format question: pass2() copies DIRSIZ bytes per path component
# into pathname[200], so at the host's 254 a single component overruns it.
dep 'sys/param.h -> fsck object'   src/include/sys/param.h     $B/fsck/fsck.o
# ncheck is the reader that fails SILENTLY on a wrong DIRSIZ -- no output at
# all, exit 0 -- so param.h reaching its object is the edge that keeps the flag
# meaning something.  tests/mkfs section 9 measures the failure; this keeps the
# rebuild honest.
dep 'sys/ino.h -> ncheck object'   src/include/sys/ino.h       $B/ncheck/ncheck.o
dep 'sys/param.h -> ncheck object' src/include/sys/param.h     $B/ncheck/ncheck.o
dep 'sys/dir.h -> ncheck object'   src/include/sys/dir.h       $B/ncheck/ncheck.o
# quot reads inodes and never a directory, so it has no DIRSIZ edge to assert
# and its case list is shorter BY MEASUREMENT rather than by omission -- see
# tests/mkfs, which compiles quot.c both ways and cmp's the objects.
dep 'sys/ino.h -> quot object'     src/include/sys/ino.h       $B/quot/quot.o
dep 'sys/filsys.h -> quot object'  src/include/sys/filsys.h    $B/quot/quot.o
# doprnt is where %.Ns lives, and ncheck is the first program in the tree to
# print an unterminated fixed-width field through it.
dep 'doprnt.c -> libc'         src/libc/stdio/doprnt.c         $B/libc/libv8c.a
# THE TAPE FORMAT, and it is a fourth header carrying VAX widths.  dumprestor.h
# narrows struct spcl's c_date and c_ddate to four bytes for the reason
# sys/ino.h narrows di_mtime -- spcl is written to tape as exactly BSIZE(0)
# bytes, so a widened field slides every field after it.  All three programs
# read it and all three must rebuild.
dep 'dumprestor.h -> dump'     src/include/dumprestor.h        $B/dump/dumptape.o
dep 'dumprestor.h -> restor'   src/include/dumprestor.h        $B/restor/restor.o
dep 'dumprestor.h -> dumpdir'  src/include/dumprestor.h        $B/dumpdir/dumpdir.o
# ...and it reaches the rootfs view too, which is the end the programs actually
# compile against.
dep 'dumprestor.h -> rootfs headers' src/include/dumprestor.h \
                                     rootfs/usr/include/.stamp
# getgrnam was missing from libv8c, so dump resolved it from -lSystem and read
# the MAC's /etc/group from inside the jail -- the getgrent/`ls -g' leak again,
# caught by tests/kmemu's nm -u sweep.  V8 has the source; it is imported now.
dep 'getgrnam.c -> libc'       src/libc/stdio/getgrnam.c       $B/libc/libv8c.a
# ...and sys/param.h to the three tape objects that DIRSIZ actually changes,
# measured per object rather than per program: restor.o, dumpdir.o and
# dump/dumptraverse.o differ with and without the flag; dump's other five are
# byte-identical.  dumptraverse is the one worth naming -- dsrch() is the only
# consumer and it runs only in pass II of an INCREMENTAL, where a wrong DIRSIZ
# silently drops every directory from the tape.
dep 'sys/param.h -> restor object'   src/include/sys/param.h  $B/restor/restor.o
dep 'sys/param.h -> dumpdir object'  src/include/sys/param.h  $B/dumpdir/dumpdir.o
dep 'sys/param.h -> dumptraverse'    src/include/sys/param.h  $B/dump/dumptraverse.o
# daddr_t's width reaches libc as well as the program, because ltol3 strides by
# it -- two patches that once disagreed about exactly this.
dep 'sys/types.h -> libc'      src/include/sys/types.h         $B/libc/libv8c.a
dep 'ltol3.c -> libc'          src/libc/gen/ltol3.c            $B/libc/libv8c.a
dep 'l3tol.c -> libc'          src/libc/gen/l3tol.c            $B/libc/libv8c.a
# The negative control: mkfs links the standard list and nothing else.  It is
# not a groveler -- it writes a filesystem rather than reading the host's.
nodep 'mkfs does not reach libkmemu' shim/libkmemu/procfs.c    $B/bin/mkfs

# --- /usr/src/cmd, staged so Bell Labs' Admin/Mk can run in the jail --------
# Two edges per file and both matter.  A stale Admin/Mk in the rootfs would run
# an old build description while the Makefile derived destinations from the new
# tables -- the exact drift $(ADMIN) was pointed at src/ to prevent -- and a
# stale staged .c would have Mk rebuild the version before the last patch,
# which tests/jail would then compare against the current binary and call a
# difference in the COMPILER.
dep 'Admin/Mk -> the jail copy'     src/cmd/Admin/Mk \
                                    rootfs/usr/src/cmd/Admin/Mk
dep 'Admin/dest -> the jail copy'   src/cmd/Admin/dest \
                                    rootfs/usr/src/cmd/Admin/dest
dep 'Admin/binfiles too'            src/cmd/Admin/binfiles \
                                    rootfs/usr/src/cmd/Admin/binfiles
dep 'cat.c -> the staged source'    src/cmd/cat.c \
                                    rootfs/usr/src/cmd/cat.c
# ...and the same source still reaches the binary our own rules build, which is
# what tests/jail compares Mk's output against.  One file, two consumers.
dep 'cat.c -> our own cat'          src/cmd/cat.c              $B/bin/cat
# Negative control: staging is a copy, not a compile.  If the .c ever grew a
# dependency on the compiler the whole tree would restage on every toolchain
# change, and a 50-file copy would hide inside the build noise.
nodep 'staging a source does not depend on the compiler' \
                                    $B/cc/v8cc                 rootfs/usr/src/cmd/cat.c

# --- section 8a step 2: the filesystem switch -------------------------------
# vfs.c holds the mount table and the passthrough type; syscall.c dispatches
# through it.  Both directions are edges, and the header is the contract.
dep 'vfs.c -> shim'        shim/v8sys/vfs.c      $B/v8sys/libv8sys.a
dep 'vfs.h -> syscall.o'   shim/v8sys/vfs.h      $B/v8sys/syscall.o
dep 'vfs.h -> vfs.o'       shim/v8sys/vfs.h      $B/v8sys/vfs.o

# ...AND THE FOURTH TYPE, WHICH WAS THE ONE EDGE MISSING WHEN §8a step 5f ADDED
# THREE SLOTS TO THAT STRUCT.  The other three implementations had a case each
# and p9cl.c did not, purely because it was written after the block above.
#
# What a stale p9cl.o against a new vfs.h produces is not a link error: it is a
# TABLE WITH THE WRONG NUMBER OF ENTRIES, so v8fs_p9.t_access is whatever field
# happened to sit at that offset in the older layout.  Measured, by accident, on
# exactly this shape -- a `git stash' baseline measurement left syscall.o and
# syscall.c with the same mtime to the second, make declared the object current,
# and the shipped binaries dispatched access() into t_stat.  Every source file
# read correctly; the only place the truth showed was the wire.
dep 'vfs.h -> p9cl.o'      shim/v8sys/vfs.h      $B/v8sys/p9cl.o
dep 'p9cl.c -> p9cl.o'     shim/v8sys/p9cl.c     $B/v8sys/p9cl.o
dep 'vfs.h -> client probe' shim/v8sys/vfs.h     $B/v8sys/p9clprobe
dep 'vfs.h -> v8sys test'  shim/v8sys/vfs.h     $B/v8sys/test

# --- section 8a step 3: /proc, the second filesystem type -------------------
dep 'procfs.c -> kmemu archive' shim/libkmemu/procfs.c  $B/kmemu/libkmemu.a
dep 'vfs.h -> procfs.o'         shim/v8sys/vfs.h        $B/kmemu/procfs.o
dep 'noprocfs.c -> shim'        shim/v8sys/noprocfs.c   $B/v8sys/libv8sys.a
# ...and /proc must NOT reach an ordinary command.  It answers from libproc, so
# folding it into libv8sys would put a libSystem import in all 58 binaries --
# the same reason libkmemu is a separate archive, and the reason noprocfs.c
# exists at all.
nodep '/proc does not reach cat' shim/libkmemu/procfs.c $B/bin/cat

# --- section 8a step 1: V8's kernel source, and the seam under it -----------
# Two dialects meet at this archive, so both edges are asserted.  stream.c is
# authentic K&R compiled with gnu89; machdep.c is ours, compiled with SHIMFLAGS.
dep 'stream.c -> kern archive'  src/sys/dev/stream.c        $B/kern/libv8kern.a
dep 'stream.h -> stream.o'      src/sys/h/stream.h          $B/kern/stream.o
dep 'sparam.h -> stream.o'      src/sys/research/sparam.h   $B/kern/stream.o
# The stand-in headers are a build input like any other, and they are reached by
# -I rather than by being next to the source -- which is exactly the kind of
# edge a dependency scanner misses and a hand-written rule forgets.
dep 'our param.h -> stream.o'   shim/kern/h/param.h         $B/kern/stream.o
dep 'our mtpr.h -> stream.o'    shim/kern/h/mtpr.h          $B/kern/stream.o
dep 'our conf.h -> stream.o'    shim/kern/h/conf.h          $B/kern/stream.o
dep 'machdep.c -> kern archive' shim/kern/dev/machdep.c     $B/kern/libv8kern.a
dep 'rawsys.h -> machdep.o'     shim/v8sys/rawsys.h         $B/kern/machdep.o

# --- and the syscall side, which brought eleven headers with it -------------
#
# MEASURED WHILE MUTATION-TESTING THESE, AND IT IS TRUE OF EVERY `dep' CASE IN
# THIS FILE.  Deleting src/sys/h/inode.h from the Makefile's explicit
# prerequisite list does NOT make the case below fail, because $(DEPFLAGS) is
# `-MMD -MP' and the compiler-generated build/stage0/kern/streamio.d carries
# the same edge.  Removing BOTH does fail it.
#
# So a `dep' case asserts the UNION of the two mechanisms, which is the right
# thing to assert -- it is what make actually knows -- but it means the case
# cannot tell you which one is carrying the edge.  The explicit list still
# earns its place: -MMD's file appears only alongside the .o it describes, so a
# rule written WITHOUT $(DEPFLAGS) has no depfile at all and the hand-written
# prerequisites are the only thing left.  That is the failure this catches, and
# it is why a mutation of the list alone looks like a passing test.
# streamio.c reaches ELEVEN headers, six of them Bell Labs' and five ours, and
# every one is reached by a quoted "../h/x.h" that resolves against a directory
# the source is not in.  That is the shape a dependency scanner gets wrong and
# a hand-written rule forgets, so all eleven are asserted -- the authentic ones
# because a re-import must recompile, and ours because they are where every
# machine fact lives.
dep 'streamio.c -> kern archive' src/sys/sys/streamio.c    $B/kern/libv8kern.a
dep 'stream.h -> streamio.o'     src/sys/h/stream.h        $B/kern/streamio.o
dep 'dir.h -> streamio.o'        src/sys/h/dir.h           $B/kern/streamio.o
dep 'inode.h -> streamio.o'      src/sys/h/inode.h         $B/kern/streamio.o
dep 'ioctl.h -> streamio.o'      src/sys/h/ioctl.h         $B/kern/streamio.o
dep 'ttyld.h -> streamio.o'      src/sys/h/ttyld.h         $B/kern/streamio.o
dep 'file.h -> streamio.o'       src/sys/h/file.h          $B/kern/streamio.o
dep 'inline.h -> streamio.o'     src/sys/h/inline.h        $B/kern/streamio.o
dep 'sparam.h -> streamio.o'     src/sys/research/sparam.h $B/kern/streamio.o
dep 'our param.h -> streamio.o'  shim/kern/h/param.h       $B/kern/streamio.o
dep 'our user.h -> streamio.o'   shim/kern/h/user.h        $B/kern/streamio.o
dep 'our proc.h -> streamio.o'   shim/kern/h/proc.h        $B/kern/streamio.o
# THIS CASE USED TO SAY `our buf.h' AND POINT AT shim/kern/h/buf.h, AND IT WAS
# GREEN WHILE AUDITING NOTHING.  streamio.c:4 has always said
# `#include "../h/buf.h"'; that used to resolve to ours because src/sys/h/ had
# no buf.h, and bio.c's import in §8a step 5 put the authentic one there --
# which wins, because a quoted include tries the includer's directory first.
# The make edge stayed real (the Makefile listed our file as a prerequisite) so
# nothing went red, while the header named in the case was no longer the header
# the compile opens.  Measured with `clang -M', which is the only instrument
# that can see it: the #include line is identical either way.  Our copy used
# neither of its two constants anywhere and is deleted.
dep 'authentic buf.h -> streamio.o' src/sys/h/buf.h        $B/kern/streamio.o
dep 'our conf.h -> streamio.o'   shim/kern/h/conf.h        $B/kern/streamio.o

# setjmp.h is the twelfth and it is not one of the eleven: it arrives through
# OUR param.h, because u_qsav has to be a jump buffer this machine can use and
# it has to be V8's rather than the host's.  An edge with no #include naming it
# in any authentic file, which is precisely why it is written down.
dep 'V8 setjmp.h -> streamio.o'  src/include/setjmp.h      $B/kern/streamio.o

# --- §8a step 5: the six imported filesystem files, and the seven new headers
#
# THE OBJECTS ARE IN A SUBDIRECTORY AND THAT IS AN ASSERTION, not tidiness.
# src/sys/sys/subr.c and shim/kern/sys/subr.c share a basename, so the flat
# $B/kern/%.o naming above would give them ONE object -- make would compile
# whichever rule it matched and the other file would silently not be in the
# archive.  These cases pin both paths, so a collapse back to flat naming
# breaks a test rather than losing a translation unit.
dep 'alloc.c -> its object'   src/sys/sys/alloc.c  $B/kern/v8fs/alloc.o
dep 'iget.c -> its object'    src/sys/sys/iget.c   $B/kern/v8fs/iget.o
dep 'nami.c -> its object'    src/sys/sys/nami.c   $B/kern/v8fs/nami.o
dep 'rdwri.c -> its object'   src/sys/sys/rdwri.c  $B/kern/v8fs/rdwri.o
dep 'imported subr.c -> its object' src/sys/sys/subr.c $B/kern/v8fs/subr.o
dep 'bio.c -> its object'     src/sys/dev/bio.c    $B/kern/v8fs/bio.o
dep 'v8fs.c -> its object'    shim/kern/sys/v8fs.c $B/kern/v8fs/v8fs.o
dep 'v8fs objects -> archive' src/sys/sys/nami.c   $B/kern/libv8kern.a

# §8a step 5c's own file.  main.c is OURS and it is in the v8fs GROUP rather
# than with slp/fio/subr/ioconf, because it needs -DKERNEL and -fcommon to see
# the tentative definitions it then defines strongly.  Its object path is what
# says which rule matched: $B/kern/main.o would mean the generic shim rule ran
# and the tables were compiled without KERNEL -- which links, and gives the
# kernel a null inode table.
dep 'main.c -> its object'    shim/kern/sys/main.c $B/kern/v8fs/main.o
dep 'main.o -> archive'       shim/kern/sys/main.c $B/kern/libv8kern.a
# main.c is the one file that includes the AUTHENTIC buf.h by full path, for
# struct buf and the B_ flags binit weaves the free lists with.
dep 'authentic buf.h -> main.o' src/sys/h/buf.h    $B/kern/v8fs/main.o
dep 'inode.h -> main.o'       src/sys/h/inode.h    $B/kern/v8fs/main.o
dep 'mount.h -> main.o'       src/sys/h/mount.h    $B/kern/v8fs/main.o
dep 'our filsys.h -> main.o'  shim/kern/h/filsys.h $B/kern/v8fs/main.o
# §8a step 5d.  vlimit.h is authentic, it is #defines only, and NOTHING HAD
# EVER INCLUDED IT although shim/kern/h/user.h:171 already named it in a
# comment as the authority for u_limit's indices.  v8k_uinit needs LIM_FSIZE
# and INFINITY from it; without them writei refuses every write to a regular
# file, with EMFILE.
#
# BOTH THESE CASES ARE BELT AND BRACES AND THAT IS WORTH SAYING.  Measured by
# deleting each include: the build FAILS -- `use of undeclared identifier
# INFINITY' and `call to undeclared function getfs' -- because the shim half is
# -std=gnu99 where the imported half is K&R.  So the compiler is the primary
# guard and these assert the MAKE edge, which is the half the compiler cannot
# see: a stale object built before the header changed.  The .d files clang
# -MMD writes name both, checked.
dep 'authentic vlimit.h -> main.o' src/sys/h/vlimit.h $B/kern/v8fs/main.o
# And v8fs.c gained filsys.h in the same step, for `struct filsys *getfs()' --
# the DECLARATION, not the struct.  Under -std=gnu99 an undeclared getfs is an
# error rather than a truncated pointer, which is the good direction and is
# what caught it; the edge is asserted so the include cannot be dropped by
# someone tidying, which would turn a compile error into a rebuild that
# silently did not happen.
dep 'our filsys.h -> v8fs.o'  shim/kern/h/filsys.h $B/kern/v8fs/v8fs.o
dep 'V8 filsys.h -> v8fs.o'   src/include/sys/filsys.h $B/kern/v8fs/v8fs.o

# The seven headers §8a step 5 added.  Every one is reached by a quoted
# "../h/x.h" that resolves against a directory the source is not in -- the
# shape a scanner gets wrong and a hand-written rule forgets.
#
# FOUR OF THE SEVEN EXIST ONLY BECAUSE AN AUTHENTIC HEADER INCLUDES THEM:
# src/sys/h/vm.h is Bell Labs' and its lines 7-10 name vmparam.h, vmmac.h,
# vmmeter.h and vmsystm.h.  Two of those are empty files.  An empty header
# still has to be a prerequisite, because the day it stops being empty the
# object must recompile -- and vmparam.h stopped being empty within a minute
# of being written (KLMAX, bio.c:553).
dep 'our pte.h -> bio.o'      shim/kern/h/pte.h      $B/kern/v8fs/bio.o
dep 'our vmmac.h -> bio.o'    shim/kern/h/vmmac.h    $B/kern/v8fs/bio.o
dep 'our vmparam.h -> bio.o'  shim/kern/h/vmparam.h  $B/kern/v8fs/bio.o
dep 'our vmmeter.h -> bio.o'  shim/kern/h/vmmeter.h  $B/kern/v8fs/bio.o
dep 'our vmsystm.h -> bio.o'  shim/kern/h/vmsystm.h  $B/kern/v8fs/bio.o
dep 'authentic vm.h -> bio.o' src/sys/h/vm.h         $B/kern/v8fs/bio.o
dep 'authentic seg.h -> bio.o' src/sys/h/seg.h       $B/kern/v8fs/bio.o

# systm.h reaches ALL SIX and is the one that carries time, lbolt and the
# device-switch counts -- so a re-import of it must recompile every one.
for o in alloc iget nami rdwri subr bio; do
	dep "systm.h -> $o.o" src/sys/h/systm.h $B/kern/v8fs/$o.o
done

# The three FORWARDING headers, which are one line each and point at the
# patched on-disk records in src/include/sys/.  §8a step 4a's whole lesson is
# that a record written to a disk gets exactly ONE declaration, so the edge
# that has to hold is from the record to the object, THROUGH the forwarder.
dep 'our filsys.h -> alloc.o'  shim/kern/h/filsys.h        $B/kern/v8fs/alloc.o
dep 'the real filsys.h -> alloc.o' src/include/sys/filsys.h $B/kern/v8fs/alloc.o
dep 'our ino.h -> iget.o'      shim/kern/h/ino.h           $B/kern/v8fs/iget.o
dep 'our fblk.h -> alloc.o'    shim/kern/h/fblk.h          $B/kern/v8fs/alloc.o
dep 'mount.h -> nami.o'        src/sys/h/mount.h           $B/kern/v8fs/nami.o
dep 'cmap.h -> rdwri.o'        src/sys/h/cmap.h            $B/kern/v8fs/rdwri.o
dep 'vlimit.h -> rdwri.o'      src/sys/h/vlimit.h          $B/kern/v8fs/rdwri.o
dep 'acct.h -> alloc.o'        src/sys/h/acct.h            $B/kern/v8fs/alloc.o
dep 'inline.h -> iget.o'       src/sys/h/inline.h          $B/kern/v8fs/iget.o

# param.h reaches v8fs.o, and this case was WRITTEN WRONG THE FIRST TIME in a
# way worth keeping: its label said "our hostok.h -> v8fs.o" while the path it
# checked was param.h.  v8fs.c does not include hostok.h at all -- measured,
# zero occurrences -- so the label described an edge that does not exist while
# the assertion tested one that does.  A green case with a lying label is worse
# than no case: it is a claim nothing audits, which is this suite's own subject.
dep 'our param.h -> v8fs.o'    shim/kern/h/param.h         $B/kern/v8fs/v8fs.o

# WHY THE HEADER CASES ABOVE SURVIVE A MUTATION OF THE MAKEFILE, which is not
# obvious and was found by running one.  Removing $(V8FS_H) from the bio.o rule
# changed nothing: 323 passed either way.  The reason is $(DEPFLAGS) -- clang's
# -MMD writes build/stage0/kern/v8fs/bio.d, and THAT supplies the header edges.
# So the explicit prerequisite list is belt-and-braces over the generated one.
#
# The cases are not vacuous -- they assert the edge EXISTS, which is what has
# to be true -- but they are testing the .d mechanism, not the rule's text.
# Both are worth having: a .d file is only written by a successful compile, so
# it cannot describe a header that was added since, and the explicit list is
# what covers the first build after an import.  The case below is the one that
# can only pass through the explicit list, because $(SRCTREE)-staged sources
# have no .d at all.

# --- ttyld.c, the tty line discipline, and the generated header it needs ----
#
# Five of its six includes are edges like streamio.c's.  The sixth is the one
# worth the case: ttyld.c:6 is `#include "tty.h"', the per-configuration header
# config(8) would have written, and the file that answers it is OURS --
# shim/kern/dev/tty.h, found through KERNFLAGS' -Ishim/kern/dev by the same
# fall-through that turns "../h/param.h" into a stand-in.  NTTY lives only
# there, so if that edge is missing, changing the number leaves a stale object
# holding a differently sized tty[] and nothing says so.
dep 'ttyld.c -> kern archive'    src/sys/dev/ttyld.c       $B/kern/libv8kern.a
dep 'stream.h -> ttyld.o'        src/sys/h/stream.h        $B/kern/ttyld.o
dep 'ioctl.h -> ttyld.o'         src/sys/h/ioctl.h         $B/kern/ttyld.o
dep 'ttyld.h -> ttyld.o'         src/sys/h/ttyld.h         $B/kern/ttyld.o
dep 'sparam.h -> ttyld.o'        src/sys/research/sparam.h $B/kern/ttyld.o
dep 'our param.h -> ttyld.o'     shim/kern/h/param.h       $B/kern/ttyld.o
dep 'our conf.h -> ttyld.o'      shim/kern/h/conf.h        $B/kern/ttyld.o
dep 'generated tty.h -> ttyld.o' shim/kern/dev/tty.h       $B/kern/ttyld.o

# partab.c includes NOTHING -- 51 lines of pure data -- so the archive edge is
# the only one there is, and its absence from the list above is not an omission.
dep 'partab.c -> kern archive'   src/sys/sys/partab.c      $B/kern/libv8kern.a

# The four files that supply the fifteen names.  tsleep is the one that decided
# whether this import could happen at all, so slp.c gets its own edge rather
# than standing behind the archive.
dep 'slp.c -> kern archive'      shim/kern/sys/slp.c       $B/kern/libv8kern.a
dep 'fio.c -> kern archive'      shim/kern/sys/fio.c       $B/kern/libv8kern.a
dep 'subr.c -> kern archive'     shim/kern/sys/subr.c      $B/kern/libv8kern.a
dep 'ioconf.c -> kern archive'   shim/kern/sys/ioconf.c    $B/kern/libv8kern.a
dep 'our user.h -> slp.o'        shim/kern/h/user.h        $B/kern/slp.o

# --- §8a step 5e: the image driver, the 9P codec and the server ---------------
#
# THE FIRST EDGE HERE IS A NEGATIVE ONE AND IT IS THE POINT.  imgdev.c is a
# block driver -- it does host I/O by definition -- so putting its object in
# libv8kern.a made the archive import _pread and _pwrite and broke the case
# asserting it imports only V8's own three.  A driver set is part of a
# CONFIGURATION rather than of the kernel library: config(8) chooses one on a
# real V8, v8k_bdconf stands in for config(8), and the object belongs on the
# link line of whatever is being configured.  This says so as an assertion.
#
# It also guards the way the mistake HID.  Removing the object from KERN_OBJ
# left the archive newer than every remaining prerequisite, so the `rm -f && ar
# rcs' rule never re-ran and nm -u still showed both names -- the fix looked
# like it had not worked.  A later re-addition would fail this case rather than
# the externals case, and this one names the reason.
nodep 'the kernel archive does NOT carry the image driver' \
                                 shim/kern/dev/imgdev.c    $B/kern/libv8kern.a
dep 'imgdev.h -> imgdev.o'       shim/kern/dev/imgdev.h    $B/kern/imgdev.o
dep 'our param.h -> imgdev.o'    shim/kern/h/param.h       $B/kern/imgdev.o
dep 'buf.h -> imgdev.o'          src/sys/h/buf.h           $B/kern/imgdev.o

# The codec is compiled TWICE from one source -- once into libv8sys.a for the
# client, once into the server -- so both edges are named.  shim/p9/p9.h says
# why one file rather than two, and the reason is the same one-table rule that
# keeps vfs.c from growing a second prefix list.
dep 'p9.c -> the shim object'    shim/p9/p9.c              $B/v8sys/p9.o
dep 'p9.h -> the shim object'    shim/p9/p9.h              $B/v8sys/p9.o
dep 'p9.o -> libv8sys'           shim/p9/p9.c              $B/v8sys/libv8sys.a
dep 'p9.c -> the server'         shim/p9/p9.c              $B/v8fsd/v8fsd
dep 'p9.h -> the server'         shim/p9/p9.h              $B/v8fsd/v8fsd
# ...and the host half of the transport seam, which the client does NOT get:
# libv8sys reaches the kernel through rawsys and may name no libc function.
dep 'p9io_libc.c -> the server'  shim/p9/p9io_libc.c       $B/v8fsd/v8fsd
dep 'p9io.c -> libv8sys'         shim/v8sys/p9io.c         $B/v8sys/libv8sys.a
dep 'v8fsd.c -> the server'      shim/v8fsd/v8fsd.c        $B/v8fsd/v8fsd
dep 'the driver -> the server'   shim/kern/dev/imgdev.c    $B/v8fsd/v8fsd
dep 'kern archive -> the server' shim/kern/sys/v8fs.c      $B/v8fsd/v8fsd

dep 'file.h -> fio.o'            src/sys/h/file.h          $B/kern/fio.o
dep 'rawsys.h -> fio.o'          shim/v8sys/rawsys.h       $B/kern/fio.o

# ...and the kernel archive must NOT be a prerequisite of an ordinary command.
# 85 KB of bss and a 60 KB page-touching qinit() belong to programs that open a
# stream, which today is none of them.  Same reasoning as libkmemu, different
# cost.
nodep 'streams do not reach cat'  src/sys/dev/stream.c  $B/bin/cat
nodep 'streams do not reach libv8sys' shim/kern/dev/machdep.c $B/v8sys/libv8sys.a

# THE POINT OF THE SEPARATE ARCHIVE.  libkmemu links host libc, so if it ever
# became a prerequisite of an ordinary command that command would start
# importing libSystem -- silently, since the link would still succeed. These
# negative controls are what make that a build-graph fact rather than a habit.
# (tests/kmemu asserts the same thing from the other end, on the symbol table.)
nodep 'kmemu does not reach cat'   shim/libkmemu/mtab.c   $B/bin/cat
nodep 'kmemu does not reach libc'  shim/libkmemu/utmp.c   $B/libc/gen/malloc.o
nodep 'kmemu does not reach shim'  shim/libkmemu/synth.c  $B/v8sys/syscall.o
# ...and the shim's own do-nothing half is NOT in libkmemu: it has to travel
# with libv8sys, or a program that does not link libkmemu fails to link at all.
dep 'nokmemu -> shim archive'  shim/v8sys/nokmemu.c            $B/v8sys/libv8sys.a
dep 'tz.c -> shim archive'     shim/v8sys/tz.c                 $B/v8sys/libv8sys.a
dep 'syscalls.def -> stubs'    shim/v8sys/syscalls.def         $B/v8sys/libv8stubs.a

# The signal trampoline is the one shim file that is ASSEMBLY, so SHIM_SRC's
# `*.c' wildcard cannot see it and both of these edges had to be written by
# hand.  Neither failure would look like a build problem: an archive without it
# leaves every signal handler installed with a null trampoline, and a test
# binary without it fails to link a suite whose whole point is to catch that.
dep 'sigtramp -> shim archive' shim/v8sys/sigtramp.s           $B/v8sys/libv8sys.a
dep 'sigtramp -> v8sys test'   shim/v8sys/sigtramp.s           $B/v8sys/test
dep 'signal.c -> v8sys test'   shim/v8sys/signal.c             $B/v8sys/test
dep 'sigtramp -> client probe' shim/v8sys/sigtramp.s           $B/v8sys/p9clprobe

# THE CLIENT PROBE COMPILES p9cl.c A SECOND TIME, and that is what these edges
# are about.  It links $(SHIM_SRC) straight into a host binary rather than using
# libv8sys.a, so a change to the client is compiled twice into two artefacts --
# and a probe built from a stale copy would test the code that was there
# yesterday while the shipped binaries tested today's.  That is not a build
# failure; it is a green suite measuring two different programs.
dep 'p9cl.c -> client probe'   shim/v8sys/p9cl.c               $B/v8sys/p9clprobe
dep 'the codec -> client probe' shim/p9/p9.c                   $B/v8sys/p9clprobe
dep 'the probe -> its binary'  tests/streams/p9clprobe.c       $B/v8sys/p9clprobe
# ...and the probe is NOT in the shipped archive, which is the other half: it
# has a main() and it is a test.
nodep 'the probe is not shipped' tests/streams/p9clprobe.c     $B/v8sys/libv8sys.a

# THE SANITIZED SERVER MUST TRACK THE REAL ONE.  It exists to catch undefined
# behaviour no behavioural case can see, so a stale copy is the worst possible
# failure: it would report the OLD source clean and say nothing.
dep 'v8fsd.c -> ubsan server'  shim/v8fsd/v8fsd.c              $B/v8fsd/v8fsd-ubsan
dep 'the codec -> ubsan server' shim/p9/p9.c                   $B/v8fsd/v8fsd-ubsan
# ...and it is the SHIM's, not startup code: crt0.o must not acquire it.
nodep 'sigtramp is not crt0'   shim/v8sys/sigtramp.s           $B/crt0.o

# The libc files added when tests/kmemu found them resolving from libSystem
# instead.  A missing one does not break the build, so only the dependency edge
# says they are being compiled at all.  sleep is the fifth and the odd one: it
# was not missing by oversight but held back, because it is alarm + a handler +
# pause() and no V8 program in this port could catch a signal until
# shim/v8sys/sigtramp.s existed.
dep 'atof -> libc'             src/libc/gen/atof.c             $B/libc/libv8c.a
dep 'tolower -> libc'          src/libc/gen/tolower.c          $B/libc/libv8c.a
dep 'getgrent -> libc'         src/libc/stdio/getgrent.c       $B/libc/libv8c.a
dep 'getpwuid -> libc'         src/libc/stdio/getpwuid.c       $B/libc/libv8c.a
dep 'sleep -> libc'            src/libc/gen/sleep.c            $B/libc/libv8c.a

# --- negative controls: the suite must be able to say "no" ------------------
nodep 'spell does not reach sh'   src/cmd/spell/huff.h   $B/sh/main.o
nodep 'tbl does not reach eqn'    src/cmd/tbl/t..c       $B/eqn/main.o
nodep 'refer does not reach libc' src/cmd/refer/refer..c $B/libc/gen/malloc.o
nodep 'grap does not reach pic'   src/cmd/grap/grap.h    $B/pic/main.o

# --- deleted rootfs data files must come back -------------------------------
# A directory stamp recorded "the install ran", which is not the question:
# deleting one installed table left the stamp alone, so make did not restore it.
#
# The five /dev entries at the end are the same question asked of a STATIC
# PATTERN rule.  $(ROOTFS_DEVFD) is 128 targets from one rule, and the easy
# spellings of that -- a directory target, or `$(ROOTFS_DEVFD):' with a recipe
# -- would either restore nothing (the stamp problem again) or be a multi-target
# rule racing under -j.  Deleting one node has to put back that node.
for f in rootfs/usr/lib/term/tab.37 rootfs/usr/lib/font/dev202/DESC.out \
         rootfs/usr/lib/font/dev202/R.out rootfs/lib/libv8c.a \
         rootfs/usr/lib/grap.defines rootfs/usr/lib/units rootfs/usr/lib/eign \
         rootfs/usr/bin/cal rootfs/bin/who rootfs/bin/df rootfs/usr/bin/load \
         rootfs/usr/bin/w rootfs/usr/bin/uptime rootfs/bin/ps \
         rootfs/etc/mkfs rootfs/etc/icheck rootfs/etc/dcheck \
         rootfs/etc/clri rootfs/etc/fsck rootfs/etc/ncheck rootfs/etc/quot \
         rootfs/etc/dump rootfs/etc/restor rootfs/etc/dumpdir \
         rootfs/usr/src/cmd/Admin/Mk rootfs/usr/src/cmd/cat.c \
         rootfs/dev/fd/0 rootfs/dev/fd/3 rootfs/dev/fd/127 \
         rootfs/dev/tty rootfs/dev/stdin; do
	rm -f "$ROOT/$f"
	$MAKE >/dev/null 2>&1
	if [ -f "$ROOT/$f" ]; then
		pass=$((pass+1))
	else
		fail=$((fail+1)); echo "FAIL deleting $f did not restore it"
	fi
done

# --- ...and each font recipe must write only its OWN target -----------------
# Not a dependency question, which is why it hid here: the edges were all
# correct. The recipe was `cp $(BUILD)/troff/dev202/*.out $(@D)/', so under -j
# make ran twelve instances of it and each wrote all twelve files. macOS cp
# clones through a rename, so one job could stat a name another had moved
# aside: "cp: .../BI.out: No such file or directory", three runs in six. CI lost
# the race before this repo did.
#
# Asked of the recipe rather than of an outcome, because the outcome is
# intermittent and a test that fails half the time is worse than none.
rm -f "$ROOT/rootfs/usr/lib/font/dev202/BI.out"
fontcmd=$($MAKE -n "$ROOT/rootfs/usr/lib/font/dev202/BI.out" 2>/dev/null | grep '^.*cp ')
case "$fontcmd" in
*'*.out'*) fail=$((fail+1))
           echo "FAIL the font install recipe copies the whole set, not its target"
           echo "  $fontcmd" ;;
*BI.out*)  pass=$((pass+1)) ;;
*)         fail=$((fail+1))
           echo "FAIL could not read the font install recipe"; echo "  $fontcmd" ;;
esac
$MAKE >/dev/null 2>&1

# ...and w/uptime must come back as ONE INODE, not two files.  Deleting w breaks
# the link, so the rule has to remake it; if it did not, both names would exist
# -- passing the loop above -- while quietly being separate binaries that the
# next rebuild could leave at different vintages.  Restoration is not the
# invariant here; identity is.
for victim in rootfs/usr/bin/w rootfs/usr/bin/uptime; do
	rm -f "$ROOT/$victim"
	$MAKE >/dev/null 2>&1
	wi=$(stat -f %i "$ROOT/rootfs/usr/bin/w" 2>/dev/null)
	ui=$(stat -f %i "$ROOT/rootfs/usr/bin/uptime" 2>/dev/null)
	if [ -n "$wi" ] && [ "$wi" = "$ui" ]; then
		pass=$((pass+1))
	else
		fail=$((fail+1))
		echo "FAIL after deleting $victim, w and uptime are not one inode ($wi vs $ui)"
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

# --- the repo root holds nothing but the known files ------------------------
# Debugging lands at the root, because a reproduction is quicker to write there
# than to place properly.  313 KB of it got COMMITTED -- fourteen tracked files
# including four 73 KB Mach-O binaries (gw, ls1, pwd2, so) and eight .s files --
# and none of it was referenced by the Makefile, the suites, CI or the hooks.
#
# .gitignore cannot be the guard.  It now anchors /*.c, /*.h and /*.s at the
# root, but an EXTENSIONLESS binary is indistinguishable by pattern from a
# legitimate file, and those were four of the fourteen.  So the guard is an
# allowlist: anything at the root that is not named here is a finding.
#
# Directories are not checked -- they are the structure.  Only regular files.
# BOTH ROOTS, because there are two now and scratch lands in either.  $REPO is
# the repository (prose, the dispatching Makefile, third_party, tools); $ROOT is
# this release's tree, and a reproduction written while debugging v8 lands
# there.  Checking only one would leave the other exactly as unguarded as the
# whole repo was before this case existed.
strays=""
for d in "$REPO" "$ROOT"; do
	case "$d" in
	"$REPO") allowed=" .gitignore ARTICLE.md CLAUDE.md Makefile PLAN.md README.md " ;;
	*)       allowed=" Makefile " ;;
	esac
	for f in $(cd "$d" && ls -p | grep -v '/$' | tr '\n' ' '); do
		case "$allowed" in
		*" $f "*) ;;
		*) strays="$strays ${d##*/}/$f" ;;
		esac
	done
done
if [ -z "$strays" ]; then
	pass=$((pass+1))
else
	fail=$((fail+1))
	echo "FAIL a tree root has files that are not in the allowlist:$strays"
	echo "  scratch belongs in the session scratchpad, not the repo."
	echo "  if one of these is real, add it to \$allowed in this case."
fi

echo "deps: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
