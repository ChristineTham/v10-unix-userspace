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
if ! $MAKE >/dev/null 2>&1 || ! $MAKE "$ROOT/$B/v8sys/test" >/dev/null 2>&1; then
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

# libm, which is a stub because V8's was one -- shim/libm/dummy.c has the
# account.  It is in the graph rather than made once and forgotten because the
# driver now RESOLVES -l against the rootfs (libpath() in src/cmd/cc.c): if this
# archive goes missing, -lm silently reaches the macOS SDK again, and the way
# that failed was a link error naming _errno and neither libm nor the jail.
dep 'libm stub -> object'      shim/libm/dummy.c               $B/libm/dummy.o
dep 'libm object -> archive'   $B/libm/dummy.o                 rootfs/usr/lib/libm.a

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
