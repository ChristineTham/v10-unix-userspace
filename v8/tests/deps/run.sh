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
dep 'load -> installed load'   $B/bin/load                     rootfs/bin/load
dep 'w source -> w'            src/cmd/w/w.c                   $B/bin/w
dep 'kmem.c -> w'              shim/libkmemu/kmem.c            $B/bin/w
dep 'w -> installed w'         $B/bin/w                        rootfs/bin/w
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
for f in rootfs/usr/lib/term/tab.37 rootfs/usr/lib/font/dev202/DESC.out \
         rootfs/usr/lib/font/dev202/R.out rootfs/lib/libv8c.a \
         rootfs/usr/lib/grap.defines rootfs/usr/lib/units rootfs/usr/lib/eign \
         rootfs/bin/cal rootfs/bin/who rootfs/bin/df rootfs/bin/load \
         rootfs/bin/w rootfs/bin/uptime rootfs/bin/ps; do
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
for victim in rootfs/bin/w rootfs/bin/uptime; do
	rm -f "$ROOT/$victim"
	$MAKE >/dev/null 2>&1
	wi=$(stat -f %i "$ROOT/rootfs/bin/w" 2>/dev/null)
	ui=$(stat -f %i "$ROOT/rootfs/bin/uptime" 2>/dev/null)
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
