# Research Unix V8 -> macOS/ARM64.  See ../PLAN.md.
#
# Stage 0 builds the V8 toolchain with the *host* compiler, just far enough to
# get a working v8cc.  Nothing built here is authentic-by-construction; it is
# scaffolding.  Once the ARM64 backend lands (Phase 1b) the world is rebuilt by
# v8cc itself, and the stage-0 binaries are only kept to prove the fixpoint.

# TWO ROOTS, because two things are being versioned differently.
#
# ROOT is THIS release's tree -- v8/ -- and it is derived from where this
# makefile sits rather than from the caller's directory, so moving the tree
# moves every path in it and nothing here had to change when v8/ appeared.
# V9 and V10 get their own sibling of it.
#
# REPO is the repository, and holds what is NOT per-release: third_party/ (the
# vendored upstreams, already versioned inside themselves), tools/, and the
# prose.  The distinction is the whole reason for the split -- see ../PLAN.md
# and ../CLAUDE.md's layout section.
ROOT    := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
REPO    := $(abspath $(ROOT)..)/
BUILD   := $(ROOT)build/stage0
SRC     := $(ROOT)src
# Defined up here, not beside the rootfs target: make expands variables in a
# TARGET name as soon as it reads the rule, so any rule building something
# under the rootfs must come after this line.
ROOTFS  := $(ROOT)rootfs

HOSTCC  ?= clang

# ---------------------------------------------------------------------------
# The rootfs, named as FILES.  Recipes are at the bottom; the names are here
# because make expands a target name when it reads the rule.
#
# Everything v8cc needs in order to compile anything.  This is the list that
# used to be spelled `| rootfs` -- an ORDER-ONLY prerequisite on a PHONY
# target, which is two mistakes that cancel into looking fine:
#
#   order-only means "exist before me", not "I depend on you", so editing
#   cc.c or a header in src/include rebuilt NOTHING that used them; and
#   phony means always out of date, so every rule naming it as a normal
#   prerequisite -- the yacc and lex generator rules -- rebuilt every time.
#
# Measured before changing, on the tree as it stood: a `make` with nothing
# touched recompiled 39 objects (all of eqn, all of pic, lex's y.tab.o), and
# `touch src/cmd/cc.c` then recompiled exactly the same 39.  The difference the
# driver made was zero.  The two are one bug: the churn is what made a build
# slow enough that hand-copying y.tab.o felt reasonable, and that stale
# y.tab.o is the lex bug in src/cmd/lex/PORTING.md.
# ---------------------------------------------------------------------------
V8INCSRC       := $(REPO)third_party/Research-Unix-v8/v8/usr/include
JERQINC        := $(REPO)third_party/Research-Unix-v8/jerq/include

ROOTFS_CPP     := $(ROOTFS)/lib/cpp
ROOTFS_CCOM    := $(ROOTFS)/lib/ccom
ROOTFS_CC      := $(ROOTFS)/bin/cc
ROOTFS_INC     := $(ROOTFS)/usr/include/.stamp
ROOTFS_YACCPAR := $(ROOTFS)/usr/lib/yaccpar
ROOTFS_NCFORM  := $(ROOTFS)/usr/lib/lex/ncform
# /etc is not decoration: getpw.c, getlogin.c and ttyslot.c read it, and
# `ls -l` prints bare uids without a passwd.  group, fstab, hosts and ttys are
# genuine V8 files imported with provenance like any other source; passwd is
# the one that cannot be, because it has to name the real user (see below).
ROOTFS_ETC     := $(patsubst $(SRC)/etc/%,$(ROOTFS)/etc/%, \
                    $(wildcard $(SRC)/etc/[a-z]*)) $(ROOTFS)/etc/passwd

# /dev is not decoration either: ps(1) REFUSES TO START without it.  ps.c:21-28
# getdirs /dev, /dev/dk and /dev/pt and opens /dev/drum, calling error() on any
# failure -- Killian's /dev reorganisation put disks under dk and pseudo-
# terminals under pt, and V8's ps was written to that layout.
#
# All three are EMPTY, and that is the true answer rather than a placeholder to
# get past a check: the jail exposes no disk devices and no ptys, and this world
# has no swap, so a zero-length drum is a drum with nothing on it.  Nothing ever
# reads it -- ps consults the drum only for a process without SLOAD, and every
# process here has it (shim/libkmemu/procfs.c).
#
# An empty directory is not empty to a reader: the shim's snapshot carries `.'
# and `..', so fstat reports 512 bytes and getdir's malloc gets a count of 2
# rather than 0.  Measured, because getdir would return null on malloc(0) and ps
# would call error() -- the failure this list exists to prevent.
#
# /dev/fd is the fourth thing in /dev and it is NOT empty, which is the whole
# difference from the three above.  V8 shipped one node per file-table slot --
# proto-dev:199-329 lists 0 through 127, and NOFILE is 128 (h/param.h:19), so
# the node set IS the file table -- plus stdin, stdout, stderr and tty hard
# linked onto minors 0-3 (link count 2 on exactly those eight names).
#
# They are EMPTY REGULAR FILES standing in for character devices, the way
# /dev/drum above stands in for the swap device, and NOTHING EVER OPENS ONE:
# shim/v8sys/vfs.c claims all five paths for the /dev/fd type before the
# passthrough /dev/ row is reached, so an open is a dup and a stat is
# synthesized.  What these files provide is the NAME, because `ls /dev/fd' has
# to read a real directory.
ROOTFS_DEVFD   := $(addprefix $(ROOTFS)/dev/fd/,$(shell seq 0 127))
# `null' is here for a different reason from the other four and was MISSING
# until the crash probe's containment check found it.  V8 shipped it --
# proto-dev:25 is `crw-rw-rw- 1 root man 3, 2 ... null' -- so its absence was a
# gap rather than a decision, and a jailed program doing creat("/dev/null")
# made one at run time, because creat keys on the parent and $V8ROOT/dev
# exists.  That is the same fall-through tar used for /dev/rmt1, and it showed
# up the same way: as a NEW PATH on a fresh CI checkout, where this tree had
# had one for so long that nothing noticed.
#
# THIS PARAGRAPH USED TO END "materialising it changes no behaviour -- the file
# a program would have created is now already there".  The observation is true
# and the inference was wrong, which is this tree's most repeated shape.  On a
# fresh checkout, BEFORE anything created it, /dev/null had no rootfs copy, the
# union rule found nothing, and the path fell through to the HOST's device --
# so reads were EOF and writes were discarded, correctly.  Materialising it did
# not preserve that; it moved the breakage from "after the first write" to
# "always", and made the guard that had found it blind, since a path already in
# the list can never be reported as new.
#
# So the file stays -- `ls /dev' has to list what V8 listed -- and it is now
# NEVER THE OBJECT: shim/v8sys/vfs.c has an exact /dev/null row before the
# passthrough /dev/ row, whose t_path hands the name back unresolved so the
# host's character device answers.  Same arrangement as the five rows above,
# for the same reason, reached one step later.
ROOTFS_DEVSTD  := $(addprefix $(ROOTFS)/dev/,tty stdin stdout stderr null)
ROOTFS_DEV     := $(ROOTFS)/dev/dk $(ROOTFS)/dev/pt $(ROOTFS)/dev/drum \
                  $(ROOTFS_DEVFD) $(ROOTFS_DEVSTD)

# One target per installed file, NOT a directory stamp.  A stamp records "the
# install ran", which is not the question make needs answered -- deleting
# rootfs/usr/lib/term/tab.37 left the stamp in place, so `make` did not put it
# back.  Measured, not assumed.  These lists name every file that lands in the
# rootfs, so each one is restored on its own.
ROOTFS_TERM := $(patsubst $(SRC)/cmd/troff/term/tab.%,$(ROOTFS)/usr/lib/term/tab.%, \
                 $(wildcard $(SRC)/cmd/troff/term/tab.*))
# makedev emits DESC.out plus one .out per font named on DESC's `fonts` line.
DEV202_FONTS := $(shell sed -n 's/^fonts[ 	][0-9]*[ 	]*//p' $(SRC)/cmd/troff/dev202/DESC)
ROOTFS_FONT  := $(patsubst %,$(ROOTFS)/usr/lib/font/dev202/%.out,DESC $(DEV202_FONTS))

# The prerequisite set of every object compiled by v8cc.  Normal, not
# order-only: a change to any of these invalidates every object in the tree.
V8CC_DEPS = $(ROOTFS_CC) $(ROOTFS_CPP) $(ROOTFS_CCOM) $(ROOTFS_INC)

# The seed driver, and what IT needs.
#
# There is exactly one cycle in this build and this is how it is cut.  The
# installed driver is now a V8 binary, so it has to be LINKED against libv8c --
# and libv8c has to be COMPILED by a driver.  cc-seed is the same cc.c built by
# clang: it compiles libv8c, libv8c links the real driver, and the real driver
# compiles everything else.  Note the absence of $(ROOTFS_CC) below; that is
# the whole point of the variable.
#
# The seed is never installed and never linked into anything.  It is rung 0 of
# the ladder in PLAN.md S4a, and rung 4 is where V8 make deletes the need for it.
CCSEED     := $(BUILD)/cc/cc-seed
SEED_DEPS   = $(CCSEED) $(ROOTFS_CPP) $(ROOTFS_CCOM) $(ROOTFS_INC)

# The two generators, as the FILES they are.  Depending on the phony `v8yacc`
# and `v8lex` is what rebuilt pic and eqn on every single make.
YACC := $(BUILD)/yacc/yacc
LEX  := $(BUILD)/lex/lex

# The libraries installed for the driver to link, and the ARM64 compiler's
# build directory.  Up here for the same reason as ROOTFS: make expands a
# variable in a PREREQUISITE as soon as it reads the rule, exactly as it does
# in a target name.  Defined below their first use, $(A64BUILD)/v8ccom
# expanded to `/v8ccom` and `make test` failed with "No rule to make target".
A64BUILD    := $(BUILD)/ccom-arm64
ROOTFS_LIBS := $(ROOTFS)/lib/crt0.o $(ROOTFS)/lib/libv8c.a \
               $(ROOTFS)/lib/libv8stubs.a $(ROOTFS)/lib/libv8sys.a \
               $(ROOTFS)/usr/lib/libm.a $(ROOTFS)/usr/lib/libtermcap.a \
               $(ROOTFS)/usr/lib/libtermlib.a

# The V8 link line, hoisted for the same reason and after the same bug bit a
# THIRD time.  V8DEPS was defined beside the pic rules, and the /bin and make
# rules added later sit above that, so `$(V8DEPS)` in their prerequisites
# expanded to nothing.  The recipes still linked correctly -- a recipe is
# expanded when it RUNS, by which time the whole makefile has been read -- so
# the binaries were right and simply never relinked when libv8sys.a changed.
# Editing the shim then left 38 stale binaries in the rootfs and no sign of it.
#
# Archive order matters: libv8stubs.a is one object per syscall so a program
# defining its own rmdir still wins, and -lSystem is last because the shim is
# the one place the two worlds are meant to meet.
V8DEPS    = $(BUILD)/crt0.o $(BUILD)/libc/libv8c.a $(BUILD)/v8sys/libv8stubs.a \
            $(BUILD)/v8sys/libv8sys.a
V8LIBS    = $(BUILD)/libc/libv8c.a $(BUILD)/v8sys/libv8stubs.a \
            $(BUILD)/v8sys/libv8sys.a -lSystem
V8LDFLAGS = -nostdlib -e _v8start

# Automatic header dependencies.
#
# Not a nicety here.  This port is driven by headers -- macdefs.h alone fixes
# the whole target model (type widths, register numbering, which hooks exist) --
# and without these, editing one left every pass-1 object stale while the build
# reported success.  That is precisely the shape of the cpp COFF-bias bug, where
# two halves of one program disagreed about a constant, so the build system is
# not allowed to reintroduce it.
DEPFLAGS = -MMD -MP

# Dialect flags: what a 1985 K&R program needs from a 2026 compiler.
#   gnu89                     -- K&R definitions, no C99/C23 strictness
#   fcommon                   -- tentative definitions merge across TUs
#   fsigned-char              -- V8 assumes signed char (see cpp's COFF tables);
#                                Apple ARM64 already agrees, Linux ARM64 does not
#   Wno-return-mismatch       -- bare `return;` in implicit-int functions is fine
#   Wno-implicit-*            -- K&R: no prototypes, implicit int
#   Wno-deprecated-non-proto  -- K&R definitions themselves
KRFLAGS = -std=gnu89 -fcommon -fsigned-char -O1 \
          -Wno-comment -Wno-return-mismatch -Wno-implicit-int \
          -Wno-implicit-function-declaration -Wno-deprecated-non-prototype \
          -Wno-parentheses -Wno-unused-value \
          -Wno-incompatible-library-redeclaration

# 1978 yacc spelled actions '={ ... }'; modern yacc/bison wants '{ ... }'.
# This is a host-tool incompatibility, not a defect, so the .y files stay
# pristine and the fixup lives here.  V8's own yacc (once built) needs no fixup.
YACCFIX = sed 's/={/{/g'

# V8 libc internals that host libc lacks.  Scaffolding; gone in Phase 2b.
STAGE0_COMPAT = $(REPO)tools/stage0-compat.c

# stubs.c and onestub.c are EXCLUDED here on purpose.  Between them they define
# open/read/write with their V8 names, which collide with the host functions the
# rest of the shim calls -- see shim/NOTES.md.  They are compiled only into
# libv8stubs.a, which the V8 world links with -nostdlib, never into anything
# that also uses host libc.  onestub.c additionally needs -DV8_NAME and friends
# and does not compile without them.
SHIM_SRC = $(filter-out $(ROOT)shim/v8sys/stubs.c $(ROOT)shim/v8sys/onestub.c, \
                        $(wildcard $(ROOT)shim/v8sys/*.c))

# The signal trampoline is assembly, so the *.c wildcard above cannot see it and
# it has to be named.  Defined HERE rather than beside SHIM_OBJ because the
# v8sys test rule below names it as a prerequisite, and a variable defined lower
# down expands to nothing there -- the mistake this file has made three times.
#
# It belongs in libv8sys.a and not beside crt0.o: the kernel enters it on every
# signal delivery, so it is part of the shim rather than of program startup, and
# putting it in the archive means signal.c's reference to it is what pulls it in.
SHIM_ASM = $(ROOT)shim/v8sys/sigtramp.s

# THE SHIM'S HEADERS, AND THE TWO RULES THAT COMPILE $(SHIM_SRC) DIRECTLY HAD
# NONE OF THEM AS PREREQUISITES.  Every .o under $(BUILD)/v8sys gets its header
# edges from $(DEPFLAGS), but $(BUILD)/v8sys/test and $(BUILD)/v8sys/p9clprobe
# link the SOURCES into a host binary in one step and never produce a .d, so
# their edges have to be written out.
#
# WHAT A MISSING ONE COSTS IS NOT A BUILD FAILURE.  §8a step 5f added three
# slots to `struct v8fstyp' in vfs.h; a probe built from a stale copy of that
# header has a TABLE WITH THE WRONG NUMBER OF ENTRIES, so t_access is whatever
# field sat at that offset before.  It links, it runs, and it dispatches into
# the wrong function -- measured by accident on exactly this shape, where a
# `git stash' baseline left one object stale and access() went to t_stat.
#
# Named rather than wildcarded, unlike SHIM_SRC, because a header appearing in
# shim/v8sys is a smaller event than a .c appearing there and the list is four
# long.  Defined HERE for SHIM_ASM's reason: the first rule that uses it is
# below, and a variable defined lower down expands to nothing.
SHIM_HDR = $(ROOT)shim/v8sys/v8sys.h $(ROOT)shim/v8sys/vfs.h \
           $(ROOT)shim/v8sys/rawsys.h $(ROOT)shim/p9/p9.h \
           $(ROOT)shim/v8id.h

# The 9P codec, and it is OUTSIDE shim/v8sys on purpose.  It is compiled twice
# -- once into libv8sys.a for the client, once into the v8fs server, which is a
# host binary and cannot link libv8sys -- so it belongs to neither.  Naming it
# here rather than letting the wildcard above find it is the same reason the
# wildcard is warned about beside nokmemu.o: a file in shim/v8sys joins every
# V8 binary silently, and this one's second consumer would not be visible at
# all.  Defined above first use for the reason SHIM_ASM gives.
P9_SRC = $(ROOT)shim/p9/p9.c
# ...and the host half of its transport seam, which the client does NOT get:
# libv8sys reaches the kernel through rawsys and may name no libc function, so
# shim/v8sys/p9io.c is its p9_io_read.  This one is shared by the server and by
# tests/streams/p9probe.c, which are both ordinary host binaries.
P9_HOSTIO = $(ROOT)shim/p9/p9io_libc.c

# CANCEL GNU MAKE'S BUILT-IN yacc AND lex RULES.  A pattern rule with no recipe
# deletes the built-in one.
#
# This is not tidiness.  GNU make ships `%.c: %.y` with the recipe
#
#	$(YACC) $<  &&  mv -f y.tab.c $@
#
# and `src/cmd/ccom/common/cgram.y` sits next to the CHECKED-IN `cgram.c` that
# the ccom rules below deliberately use instead of running yacc.  So the built-in
# rule can WRITE INTO AUTHENTIC SOURCE: `make -B` on anything that reaches
# cgram.o regenerates cgram.c in place, with a different yaccpar (short vs int
# in the parser skeleton, `>` vs `>=` on the stack-overflow check) and absolute
# paths baked into its `# line` directives.  Observed doing exactly that in this
# tree; `git diff` was the only thing that noticed.
#
# Every generated parser and scanner here has an explicit rule that writes into
# $(BUILD), so nothing wants the built-ins.  tests/deps asserts they stay dead.
%.c: %.y
%.c: %.l

.PHONY: install man grap wavec-install spell-install all stage0 test-deps test-jail test-selfhost v8bin v8make libv8kern v8fsd v8fsd-ubsan cpp ccom-pass1 ccom-vax v8ccom v8cc rootfs rootfs-libs libv8sys libv8c crt0 sh nroff troff tbl v8yacc v8lex pic spell refer eqn devtables test test-cpp test-v8ccom test-v8cc test-v8sys test-freestanding test-libv8c test-wavea test-waveb test-sh test-wavec test-kmemu test-streams test-hooks test-mkfs mkfs imgbin srctree clean distclean
all: stage0
# libv8c belongs here.  Without it a plain `make` rebuilt the compiler but left
# libv8c.a compiled by the PREVIOUS one, so a back-end fix looked like it had
# not worked -- which cost a full debugging round on the indirect-call bug.
stage0: cpp v8ccom v8cc libv8sys libv8kern v8fsd crt0 rootfs libv8c rootfs-libs sh nroff troff tbl v8yacc v8lex pic spell refer eqn v8make v8bin man wavec-install spell-install $(ROOTFS)/bin/[ $(ROOTFS)/bin/sh $(ROOTFS)/bin/make $(ROOTFS)/usr/bin/yacc $(ROOTFS)/usr/bin/lex srctree

# A test target's prerequisites are the files its script actually opens.  Four
# of these ran `rootfs/bin/cc` while depending only on rootfs-libs, and got the
# driver for free because the library install rules said `| rootfs`.  Naming
# $(V8CC_DEPS) is what keeps that true now the order-only prerequisite is gone.
test: test-deps test-jail test-selfhost test-cpp test-v8ccom test-v8cc test-v8sys test-freestanding test-libv8c test-wavea test-waveb test-sh test-wavec test-kmemu test-streams test-mkfs test-hooks
	@mkdir -p $(BUILD) && : > $(BUILD)/.tests-passed
# THE STAMP IS THE GREEN RUN, and it costs nothing to be sure of: make runs a
# target's recipe only when every prerequisite has succeeded, so this line is
# unreachable unless all seventeen suites passed.  Nothing in the build reads it
# -- it is for .claude/hooks/ci-green.sh, which refuses `git push' when a source
# file is newer than the stamp.  Deliberately NOT a real file target with
# prerequisites: "have the tests been run since I last edited" is a question
# about wall-clock history, not about a dependency graph, and make would answer
# the wrong one by rebuilding it.
# First, because it tests the thing every other suite's result depends on: that
# what was built is what the sources say.  Four bugs in this port were a stale
# object rather than wrong code.  It settles the build itself, so it takes no
# prerequisites; $(MAKE) is passed through so the nested invocation shares this
# one's jobserver.
test-deps:
	@MAKE="$(MAKE)" $(ROOT)tests/deps/run.sh
# The chroot: that a build driven by V8 make runs entirely on V8 binaries.
# Depends on the whole jail, since that is what it tests.
test-jail: v8make v8bin $(ROOTFS)/bin/sh $(ROOTFS)/bin/make $(ROOTFS)/usr/bin/yacc $(ROOTFS)/usr/bin/lex
	@$(ROOT)tests/jail/run.sh
# The self-host boundary: every translation unit of cpp and ccom compiles under
# v8cc and links freestanding.  Needs the driver, its passes, the headers, the
# libraries it links, and V8's yacc for cpp's grammar.
test-selfhost: $(V8CC_DEPS) $(ROOTFS_LIBS) $(YACC)
	@$(ROOT)tests/selfhost/run.sh
test-cpp: $(BUILD)/cpp/cpp
	@$(ROOT)tests/cpp/run.sh $(BUILD)/cpp/cpp
test-v8ccom: $(A64BUILD)/v8ccom $(BUILD)/cpp/cpp
	@$(ROOT)tests/v8ccom/run.sh $(A64BUILD)/v8ccom
test-v8cc: $(V8CC_DEPS) $(ROOTFS_LIBS)
	@$(ROOT)tests/v8cc/run.sh
# V8ROOT IS SET HERE, AND IT HAD NEVER BEEN.  This rule does not pass
# $(SHIMFLAGS), so the test binary carries no -DV8ROOT_DEFAULT, so v8root()
# returned 0 and -- in CLAUDE.md's own words about the shim -- "when unset it
# silently falls back to the host filesystem".  The suite that tests the shim's
# path resolution had therefore been running with the JAIL OFF for its whole
# life: 164 cases, none of them able to see a syscall that resolved nothing.
# Two that resolve nothing were found by a sweep instead.
#
# The environment variable rather than the flag, because that is how every
# other suite says it and because v8root() checks it first anyway.
test-v8sys: $(BUILD)/v8sys/test
	@V8ROOT=$(ROOTFS) $(BUILD)/v8sys/test
test-freestanding: $(V8CC_DEPS) $(ROOTFS_LIBS)
	@$(ROOT)tests/freestanding/run.sh
test-libv8c: $(V8CC_DEPS) $(ROOTFS_LIBS)
	@$(ROOT)tests/libv8c/run.sh
test-wavea: $(V8CC_DEPS) $(ROOTFS_LIBS)
	@$(ROOT)tests/wavea/run.sh
test-waveb: $(V8CC_DEPS) $(ROOTFS_LIBS)
	@$(ROOT)tests/waveb/run.sh
test-sh: $(BUILD)/sh/sh
	@$(ROOT)tests/sh/run.sh
# nroff opens /usr/lib/term/tab.37 and troff the dev202 font tables, both
# resolved inside $V8ROOT.  They are inputs to the suite as much as the
# binaries are, so the suite depends on them.
test-wavec: nroff troff tbl eqn pic spell $(ROOTFS_TERM) $(ROOTFS_FONT)
	@$(ROOT)tests/wavec/run.sh
# Every binary in the rootfs is swept for libc imports, so the suite depends on
# all of them being built and installed, not just on who.
test-kmemu: v8bin $(V8CC_DEPS) $(ROOTFS_LIBS)
	@$(ROOT)tests/kmemu/run.sh
# V8's stream machinery.  libv8c is a prerequisite because one case asserts that
# the compiler-emitted memcpy in allocq() resolves to V8's own rather than
# Apple's, and that is a fact about libv8c's symbol table.
# imgbin SINCE §8a STEP 5c, because the suite now needs an actual filesystem to
# read: tests/streams/fsprobe.c drives namei/iget/bmap/readi over an image that
# mkfs(8) writes here, and a probe that built its own image would be comparing
# this port against itself.  It is the phony rather than $(BINDIR)/mkfs for the
# reason test-mkfs gives above -- BINDIR is defined hundreds of lines below and
# would expand to nothing here.
# v8fsd-ubsan IS THE PHONY, NOT $(V8FSD_UBSAN), for the reason test-mkfs gives
# above and check-makefile.sh caught within the second: that variable is defined
# 700 lines below here, so naming it would expand to nothing and the dependency
# silently would not exist.  $(BUILD)/v8sys/p9clprobe is spelled out because
# $(BUILD) is defined at the top.
test-streams: libv8kern libv8c imgbin v8fsd v8fsd-ubsan $(BUILD)/kern/imgdev.o \
	     $(BUILD)/v8sys/p9clprobe
	@$(ROOT)tests/streams/run.sh
# The on-disk formats, and the image mkfs writes from them.  It asserts widths
# from the V8 side -- a _Static_assert in the shim would see only clang, and
# these structures are read by v8cc, by clang, and in principle by a VAX.
#
# The prerequisite is the phony `imgbin' rather than $(BINDIR)/mkfs for the
# reason check-makefile.sh gave when this said the latter: BINDIR is defined
# 600 lines below here, so it expanded to nothing and the dependency silently
# was not there.  test-jail and test-kmemu already name v8make and v8bin the
# same way.
test-mkfs: imgbin $(V8CC_DEPS) $(ROOTFS_LIBS)
	@$(ROOT)tests/mkfs/run.sh
# The .claude hooks.  No build prerequisites -- they are shell and python, and
# the point is that they are code and rot like code.
test-hooks:
	@$(ROOT)tests/hooks/run.sh

# $(SHIM_ASM) is on this line for the signal-delivery cases: without the
# trampoline linked in, v8s_signal installs a handler the kernel cannot enter,
# which is the whole fault those cases exist to catch.
#
# $(P9_SRC) because the framing cases call p9_send and p9_recv, which live in
# the shared codec rather than in shim/v8sys -- $(SHIM_SRC) brings only p9io.c,
# the transport half of that seam.  Both halves or neither: this suite is the
# first thing that exercises them against a real socket.
$(BUILD)/v8sys/test: $(ROOT)tests/v8sys/test.c $(SHIM_SRC) $(P9_SRC) $(SHIM_ASM) \
                    $(SHIM_HDR)
	@mkdir -p $(BUILD)/v8sys
	$(HOSTCC) -std=gnu99 -Wall -Wno-unused-variable \
	    -fno-stack-protector -fno-stack-check -o $@ $(filter %.c %.s,$^)

# The v8fs CLIENT probe -- the same link line, and it is HERE rather than built
# inside tests/streams/run.sh the way that suite's other four probes are.  They
# compile against libv8kern, which is one archive the script can name; this one
# needs $(SHIM_SRC), and a second copy of that wildcard in a shell script is the
# two-lists-that-must-agree shape kmem.c's one-table rule exists to refuse.
#
# It links the SHIM and not the kernel, which is not an omission: §8a step 5e
# measured 56 symbol collisions between libv8kern and a program's own objects,
# 25 of them silent.  The kernel is in the other process -- that is the design.
#
# tests/streams runs it against a v8fsd it has already started, to reach the
# three paths no V8 program in the tree performs: fstat on a directory
# descriptor, lseek in all three whences, and dup sharing one offset.
$(BUILD)/v8sys/p9clprobe: $(ROOT)tests/streams/p9clprobe.c $(SHIM_SRC) $(P9_SRC) \
                          $(SHIM_ASM) $(SHIM_HDR)
	@mkdir -p $(BUILD)/v8sys
	$(HOSTCC) -std=gnu99 -Wall -Wno-unused-variable \
	    -fno-stack-protector -fno-stack-check -o $@ $(filter %.c %.s,$^)

# ---------------------------------------------------------------------------
# cpp -- the C preprocessor (Reiser, 1978).  First pass of the compiler.
#
# -Darm64=1 selects, in cpp.c, both the signed-char table bias (COFF 128) and
# the machine macro cpp predefines to the programs it preprocesses.  Building
# with the upstream -Dvax=1 would make our compiler announce itself as a VAX.
# ---------------------------------------------------------------------------
CPPSRC   = $(SRC)/cmd/cpp
CPPFLAGS_V8 = -Dunix=1 -Darm64=1 -DFLEXNAMES -DMTIME

cpp: $(BUILD)/cpp/cpp
$(BUILD)/cpp/cpp: $(CPPSRC)/cpp.c $(CPPSRC)/cpy.y $(CPPSRC)/yylex.c
	@mkdir -p $(BUILD)/cpp
	cp $(CPPSRC)/yylex.c $(BUILD)/cpp/
	$(YACCFIX) $(CPPSRC)/cpy.y > $(BUILD)/cpp/cpy.y
	cd $(BUILD)/cpp && yacc cpy.y && mv -f y.tab.c cpy.c
	$(HOSTCC) $(KRFLAGS) $(CPPFLAGS_V8) -c $(CPPSRC)/cpp.c -o $(BUILD)/cpp/cpp.o
	$(HOSTCC) $(KRFLAGS) $(CPPFLAGS_V8) -I$(BUILD)/cpp -c $(BUILD)/cpp/cpy.c -o $(BUILD)/cpp/cpy.o
	$(HOSTCC) $(KRFLAGS) -o $@ $(BUILD)/cpp/cpp.o $(BUILD)/cpp/cpy.o $(STAGE0_COMPAT)
	@echo "built $@"

# ---------------------------------------------------------------------------
# ccom -- the compiler proper, built here still targeting the VAX.
#
# Useless as a compiler on this machine: it emits VAX assembly.  It is built
# anyway because it is the reference instrument for Phase 1b -- it proves the
# machine-independent pass 1 works on ARM64, and lets us watch what the real
# backend does with a given parse tree while writing the ARM64 one.
#
# Note what is NOT here: match.o, allo.o, table.o, cost.o, cgen.o.  V8 replaced
# pcc's table-driven pass 2 with the hand-written recursive generator in
# gencode.c/genaux.c, and vax/local.c defines a stub codgen() "so pcc2 stuff
# doesn't get loaded".  Those files survive in the tree only for lint.
# ---------------------------------------------------------------------------
CCOM     = $(SRC)/cmd/ccom
CCOM_M   = $(CCOM)/common
CCOM_V   = $(CCOM)/vax
CCOM_INC = -I$(CCOM_V) -I$(CCOM_M)

CCOM_MI  = xdefs scan pftn trees optim reader common1 pjw lookup catch2 t2print
CCOM_MD  = local local2 debug memcpy gencode genaux printx lcatch2

CCOM_OBJ = $(patsubst %,$(BUILD)/ccom/%.o,$(CCOM_MI) $(CCOM_MD) cgram)

# The linkable set: everything except the two files that emit VAX instructions.
# gencode.c and genaux.c do not compile under clang, for one 1985 reason: doit()
# takes a 4-byte `ret` struct by value and every caller passes literal 0, punning
# an all-zero struct as an int.  Legal when K&R had no prototypes and the struct
# was exactly int-sized; clang now sees the definition and refuses.  We are
# replacing both files in Phase 1b, so they are not worth patching -- 22 edits to
# code scheduled for deletion.
CCOM_P1  = $(patsubst %,$(BUILD)/ccom/%.o,$(CCOM_MI) local local2 debug memcpy printx lcatch2 cgram)

ccom-pass1: $(BUILD)/ccom/ccom-pass1
$(BUILD)/ccom/ccom-pass1: $(CCOM_P1) $(ROOT)compiler/ccom-arm64/gencode.c
	$(HOSTCC) $(KRFLAGS) $(DEPFLAGS) $(CCOM_INC) -c $(ROOT)compiler/ccom-arm64/gencode.c \
		-o $(BUILD)/ccom/gencode-arm64.o
	$(HOSTCC) $(KRFLAGS) -o $@ $(CCOM_P1) $(BUILD)/ccom/gencode-arm64.o
	@echo "built $@ (pass 1 + ARM64 backend stub)"

# The complete VAX compiler.  Expected to fail on gencode.c/genaux.c as above;
# kept as a target so the failure stays visible rather than forgotten.
ccom-vax: $(CCOM_OBJ)
	$(HOSTCC) $(KRFLAGS) -o $(BUILD)/ccom/ccom-vax $(CCOM_OBJ)

$(BUILD)/ccom/%.o: $(CCOM_M)/%.c
	@mkdir -p $(BUILD)/ccom
	$(HOSTCC) $(KRFLAGS) $(DEPFLAGS) $(CCOM_INC) -DVAX -c $< -o $@

$(BUILD)/ccom/%.o: $(CCOM_V)/%.c
	@mkdir -p $(BUILD)/ccom
	$(HOSTCC) $(KRFLAGS) $(DEPFLAGS) $(CCOM_INC) -DVAX -c $< -o $@

# cgram.c is the checked-in yacc output, so the 1978 grammar needs no yacc run.
$(BUILD)/ccom/cgram.o: $(CCOM_M)/cgram.c
	@mkdir -p $(BUILD)/ccom
	cp $(CCOM_V)/y.debug.sv $(BUILD)/ccom/y.debug
	$(HOSTCC) $(KRFLAGS) $(DEPFLAGS) $(CCOM_INC) -I$(BUILD)/ccom -DVAX -DYYDEBUG -c $< -o $@

# ---------------------------------------------------------------------------
# v8ccom -- the real thing: V8's pass 1 with our ARM64 back end.
# ---------------------------------------------------------------------------
A64      = $(ROOT)compiler/ccom-arm64
A64INC   = -I$(A64) -I$(CCOM_M)		# A64BUILD is defined at the top

A64_MI   = $(patsubst %,$(A64BUILD)/%.o,$(CCOM_MI) cgram)
A64_MD   = $(patsubst %,$(A64BUILD)/%.o,local local2 emit printx gencode dbstubs)

v8ccom: $(A64BUILD)/v8ccom
$(A64BUILD)/v8ccom: $(A64_MI) $(A64_MD)
	$(HOSTCC) $(KRFLAGS) -o $@ $(A64_MI) $(A64_MD)
	@echo "built $@"

$(A64BUILD)/%.o: $(CCOM_M)/%.c
	@mkdir -p $(A64BUILD)
	$(HOSTCC) $(KRFLAGS) $(DEPFLAGS) $(A64INC) -c $< -o $@

$(A64BUILD)/%.o: $(A64)/%.c
	@mkdir -p $(A64BUILD)
	$(HOSTCC) $(KRFLAGS) $(DEPFLAGS) $(A64INC) -c $< -o $@

$(A64BUILD)/cgram.o: $(CCOM_M)/cgram.c
	@mkdir -p $(A64BUILD)
	cp $(CCOM_V)/y.debug.sv $(A64BUILD)/y.debug
	$(HOSTCC) $(KRFLAGS) $(DEPFLAGS) $(A64INC) -I$(A64BUILD) -DYYDEBUG -c $< -o $@

# ---------------------------------------------------------------------------
# libv8sys -- the shim standing in for the VAX kernel.  Modern C, clang-built:
# it is the seam, not authentic V8 code.
# ---------------------------------------------------------------------------
SHIM_OBJ = $(patsubst $(ROOT)shim/v8sys/%.c,$(BUILD)/v8sys/%.o,$(SHIM_SRC)) \
           $(patsubst $(ROOT)shim/p9/%.c,$(BUILD)/v8sys/%.o,$(P9_SRC)) \
           $(patsubst $(ROOT)shim/v8sys/%.s,$(BUILD)/v8sys/%.o,$(SHIM_ASM))

# crt0 and the V8-named stub layer: the two pieces a freestanding V8 program
# needs on top of the shim.  The V8-named layer is built here, and ONLY here,
# because open/read/write collide with the host's -- see shim/NOTES.md.
crt0: $(BUILD)/crt0.o $(BUILD)/v8sys/libv8stubs.a
$(BUILD)/crt0.o: $(ROOT)compiler/crt0.s
	@mkdir -p $(BUILD)
	$(HOSTCC) -c $< -o $@

# ---------------------------------------------------------------------------
# libv8stubs.a -- the V8-named entry points, ONE OBJECT PER SYSCALL.
#
# The granularity is the point, not tidiness.  A linker pulls an archive member
# only when a symbol is still undefined, so a program defining its own rmdir
# never pulls the library's.  V8 programs do this: rm(1) has an rmdir(f, iflg)
# that prompts and forks /bin/rmdir, rcp(1) has its own mkdir.  With every
# wrapper in one object, both failed to link with duplicate symbols.  V8's libc
# had one file per syscall (libc/sys/mkdir.s, rmdir.s, ...) for this reason;
# this reproduces that from syscalls.def.
# ---------------------------------------------------------------------------
STUBFLAGS = -std=gnu89 -fcommon -w -fno-stack-protector -I$(ROOT)shim/include \
            -I$(ROOT)shim/v8sys
SYSDEF    = $(ROOT)shim/v8sys/syscalls.def
# name:impl:type:args, one per V8SYS() line.  ':' rather than ',' so the shell
# can split them without tripping over `char *`.
SYSCALLS := $(shell sed -n 's/^V8SYS(\([^,]*\),[ 	]*\([^,]*\),[ 	]*\([^,]*\),[ 	]*\([^,]*\),[ 	]*\([01]\)).*/\1:\2:\3:\4:\5/p' \
                    $(ROOT)shim/v8sys/syscalls.def | tr -d ' \t')
STUB_OBJ  = $(foreach s,$(SYSCALLS),$(BUILD)/v8sys/stub/$(word 1,$(subst :, ,$(s))).o) \
            $(BUILD)/v8sys/stub/errno.o $(BUILD)/v8sys/stub/exit.o \
            $(BUILD)/v8sys/stub/rawexit.o $(BUILD)/v8sys/stub/signal.o

# `rm -f' before every `ar' in this file, and it is not tidiness.  `ar r'
# REPLACES the members it is given and leaves the rest alone, so dropping a
# source file from an object list does not drop its object from the archive --
# it stays there, and stays linked, until someone happens to run `make clean'.
# Measured while mutation-testing tests/kmemu: removing atof from LIBC_GEN left
# atof.o in libv8c.a and seq went on using it, so the check that should have
# caught the removal saw a correct build.  The "a stale object does not look
# like a build problem" class, one layer below where it usually bites.
$(BUILD)/v8sys/libv8stubs.a: $(STUB_OBJ)
	@rm -f $@ && ar rcs $@ $(STUB_OBJ)
	@echo "built $@ ($(words $(STUB_OBJ)) objects)"

# One rule per syscall, generated: each names its own -D set.
define STUB_RULE
$(BUILD)/v8sys/stub/$(word 1,$(subst :, ,$(1))).o: $(ROOT)shim/v8sys/onestub.c $(SYSDEF)
	@mkdir -p $(BUILD)/v8sys/stub
	$(HOSTCC) $(STUBFLAGS) -DV8_NAME=$(word 1,$(subst :, ,$(1))) \
	    -DV8_IMPL=$(word 2,$(subst :, ,$(1))) \
	    -DV8_IMPLTYPE="$(word 3,$(subst :, ,$(1)))" \
	    -DV8_TYPE="$(word 4,$(subst :, ,$(1)))" \
	    -DV8_ARGS=$(word 5,$(subst :, ,$(1))) -c $$< -o $$@
endef
$(foreach s,$(SYSCALLS),$(eval $(call STUB_RULE,$(s))))

# The three that are not plain wrappers, each in its own object for the same
# reason: a program defining its own signal() must get its own.
$(BUILD)/v8sys/stub/errno.o: $(ROOT)shim/v8sys/stubs.c
	@mkdir -p $(BUILD)/v8sys/stub
	$(HOSTCC) $(STUBFLAGS) -DV8_PART_ERRNO -c $< -o $@
$(BUILD)/v8sys/stub/exit.o: $(ROOT)shim/v8sys/stubs.c
	@mkdir -p $(BUILD)/v8sys/stub
	$(HOSTCC) $(STUBFLAGS) -DV8_PART_EXIT -c $< -o $@
# _exit is its OWN member, matching V8's sys/_exit.s beside sys/exit.s.  A
# program defining its own exit() -- ex(1) does, to clean up before leaving --
# finishes with _exit(); with both in one object the linker pulls the member
# for _exit and the member's exit() then collides with the program's.
$(BUILD)/v8sys/stub/rawexit.o: $(ROOT)shim/v8sys/stubs.c
	@mkdir -p $(BUILD)/v8sys/stub
	$(HOSTCC) $(STUBFLAGS) -DV8_PART_RAWEXIT -c $< -o $@
$(BUILD)/v8sys/stub/signal.o: $(ROOT)shim/v8sys/stubs.c
	@mkdir -p $(BUILD)/v8sys/stub
	$(HOSTCC) $(STUBFLAGS) -DV8_PART_SIGNAL -c $< -o $@

libv8sys: $(BUILD)/v8sys/libv8sys.a
$(BUILD)/v8sys/libv8sys.a: $(SHIM_OBJ)
	@rm -f $@ && ar rcs $@ $(SHIM_OBJ)
	@echo "built $@"

# -fno-stack-protector and -fno-stack-check: both emit calls to libc helpers
# (___stack_chk_fail, ___chkstk_darwin), and the whole point of the shim is that
# it names no libc symbol -- see shim/v8sys/rawsys.h.
#
# AND THERE IS NO -O HERE ON PURPOSE, WHICH IS EASY TO READ AS AN OVERSIGHT.
# clang's loop-idiom pass recognises the shim's hand-written byte loops -- the
# bcopy_/bzero_ at the top of shim/v8sys/dir.c -- and rewrites them as calls.
# Measured: the same dir.c compiles to `nm -u ... _v8_errno _v8sys_alloc' with
# no -O, and gains `_bzero _memcpy' at -O2.  Adding -O is therefore a decision
# about the libc rule and not about speed.  It does not fail silently:
# rootfs/bin/cat links three symbols out of dir.o and imports nothing at all,
# and tests/kmemu sweeps every Mach-O in the rootfs with `nm -u' against an
# empty allowed list -- so the leak lands as a red suite, which is what makes
# it safe to leave this as a comment rather than a hand-rolled memset.
# V8ROOT_DEFAULT stamps the world's location into every V8 binary, so one run
# without $V8ROOT set does not silently operate on the real filesystem -- see
# v8root() in shim/v8sys/syscall.c.  In the build tree that is $(ROOTFS); `make
# install` overrides it with the install prefix.  The env var still wins, which
# is what lets a second tree be tested against the same binaries.
V8ROOT_DEFAULT ?= $(ROOTFS)
SHIMFLAGS = -std=gnu99 -Wall -Wno-unused-function \
            -fno-stack-protector -fno-stack-check \
            -DV8ROOT_DEFAULT='"$(V8ROOT_DEFAULT)"'

$(BUILD)/v8sys/%.o: $(ROOT)shim/v8sys/%.c
	@mkdir -p $(BUILD)/v8sys
	$(HOSTCC) $(SHIMFLAGS) $(DEPFLAGS) -c $< -o $@

# The shared codec, built with SHIMFLAGS like the rest of the client half -- in
# particular with NO -O, which is what keeps its byte loops from being rewritten
# as calls to memcpy.  The server compiles the same source with its own flags.
$(BUILD)/v8sys/%.o: $(ROOT)shim/p9/%.c
	@mkdir -p $(BUILD)/v8sys
	$(HOSTCC) $(SHIMFLAGS) $(DEPFLAGS) -c $< -o $@

# Assembly, so none of SHIMFLAGS applies -- the same one-line recipe crt0.s and
# setjmp.s get.  Explicit, so make's built-in %.o: %.s rule (which would run
# $(AS) with no flags of ours) never gets the chance.
$(BUILD)/v8sys/%.o: $(ROOT)shim/v8sys/%.s
	@mkdir -p $(BUILD)/v8sys
	$(HOSTCC) -c $< -o $@

# ---------------------------------------------------------------------------
# libkmemu -- the system-facts half of the shim, and the one part of it that
# may link the host's libc.  shim/libkmemu/synth.c states the boundary; PLAN.md
# section 7 records the decision.
#
# A SEPARATE ARCHIVE, deliberately, and NOT in $(V8LIBS).  Folding it into
# libv8sys.a would put libSystem imports in all 49 V8 binaries and end the
# freestanding guarantee -- tests/freestanding asserts with `nm -u' that a V8
# program takes nothing from libc.  Only the grovelers link it, and the shim
# reaches it through a weak symbol that is null everywhere else.
#
# Not under $(BUILD)/v8sys either: SHIM_SRC is a wildcard over shim/v8sys/*.c,
# so a file dropped in there would silently join libv8sys.a and every V8 binary
# would start importing libc with nothing to say it had happened.
# ---------------------------------------------------------------------------
KMEMU_SRC = $(wildcard $(ROOT)shim/libkmemu/*.c)
KMEMU_OBJ = $(patsubst $(ROOT)shim/libkmemu/%.c,$(BUILD)/kmemu/%.o,$(KMEMU_SRC))
KMEMU_LIB = $(BUILD)/kmemu/libkmemu.a

libkmemu: $(KMEMU_LIB)
$(KMEMU_LIB): $(KMEMU_OBJ)
	@rm -f $@ && ar rcs $@ $(KMEMU_OBJ)
	@echo "built $@"

$(BUILD)/kmemu/%.o: $(ROOT)shim/libkmemu/%.c
	@mkdir -p $(BUILD)/kmemu
	$(HOSTCC) $(SHIMFLAGS) $(DEPFLAGS) -c $< -o $@

# What a groveler adds to the standard link.
#
# TWO ARCHIVES DEFINE kmemu_synth: the real one here, and the do-nothing
# nokmemu.o inside libv8sys.a that every other V8 binary gets.  Which one a
# program ends up with is therefore a question about link order, and the answer
# is invisible -- both link cleanly, and the wrong one leaves `who' printing an
# empty file rather than failing.
#
# -force_load settles it by loading this archive outright.  Measured: it is NOT
# required today, because ld64 resolves archives iteratively rather than in the
# single left-to-right pass classic Unix ld made, so libkmemu.a sitting ahead of
# libv8sys.a is already enough.  It is here so that stays true if the library
# list is ever reordered, and because "the right definition wins" should be a
# statement about this line rather than about the linker's search strategy.
# tests/kmemu checks the symbol table of the built binary either way.
KMEMU_LDADD = -Wl,-force_load,$(KMEMU_LIB)

# ---------------------------------------------------------------------------
# libv8kern -- V8's own kernel source, running in the shim.
#
# THE FIRST BELL LABS KERNEL CODE IN THIS PORT.  src/sys/dev/stream.c is
# Dennis Ritchie's stream machinery, byte-identical to upstream, and
# shim/kern/ is the machine-dependent half it compiles against.  PLAN.md
# section 8a step 1; src/sys/PORTING.md has the account.
#
# A SEPARATE ARCHIVE, for libkmemu's reason and one more.  qinit() threads every
# block onto a freelist, touching ~60 KB of otherwise-clean pages, and NBLOCK
# blocks plus NQUEUE queues are ~85 KB of bss.  `cat' should not carry the
# storage of a message-passing engine it never opens.
#
# TWO DIALECTS, and they are not interchangeable.  stream.c is 1985 K&R --
# implicit int, implicit function declarations, no prototypes -- so it gets
# gnu89 with those diagnostics off.  That is a BUILD FLAG rather than a source
# change, which is the fidelity contract working: CLAUDE.md says do not
# modernise K&R declarations, and the way to obey that is to compile the
# dialect the code is written in.  machdep.c is ours and gets SHIMFLAGS.
#
# -Ishim/kern/dev is what makes `#include "../h/param.h"' land in shim/kern/h.
# A quoted include tries the includer's directory FIRST, so src/sys/h/stream.h
# -- authentic -- still wins for "../h/stream.h" from the same source file.
# Where an authentic header exists it is used; ours fill only the gaps.
KERNFLAGS = -std=gnu89 -Wall -fno-stack-protector -fno-stack-check \
            -Wno-implicit-int -Wno-implicit-function-declaration \
            -Wno-deprecated-non-prototype -Wno-parentheses -Wno-return-type \
            -Wno-char-subscripts -Wno-unused-variable \
            -I$(ROOT)shim/kern/dev -I$(SRC)/sys/research
#
# The last two are the only ones NOT about K&R syntax, so they were checked
# rather than waved through.  -Wchar-subscripts fires on `bsize[bp->class]',
# and char is signed on this target -- but class is assigned 0..3 by allocb()
# and by qinit(), nowhere else, so it cannot index backwards.  -Wunused-variable
# is freeb()'s bp1, which is used only under #ifdef CAREFUL, V8's own debug
# switch.  Left in the build noise they would train the eye past the warnings
# that matter.

# streamio.c -- the syscall side, PLAN.md section 8a step 1, imported whole.
#
# TWO MORE FLAGS THAN stream.c, and both are dialect rather than taste.
#
# -DKERNEL is upstream's own: conf/makefile:23 is `COPTS= ${IDENT} -DKERNEL',
# so every V8 kernel object was compiled with it.  Here it buys exactly one
# thing -- src/sys/h/file.h's guarded block, which declares `file', `fileNFILE'
# and `nfile', the table forceclose() walks.  inode.h's guarded block comes
# along and declares four names streamio.c never uses.
#
# -fcommon is 1985's linker, and without it the build does not link.  Those
# declarations are TENTATIVE definitions, not externs -- the K&R idiom, where
# the same three names appear in forty objects and the linker merges the
# commons.  Modern clang defaults to -fno-common, which makes each a real
# definition and collides with shim/kern/sys/fio.c's strong one.  Same shape as
# -std=gnu89 above: compile the dialect the code is written in rather than edit
# it into a newer one.
#
# TWO SUPPRESSIONS, AND THE FIRST ONE IS A HAZARD RATHER THAN NOISE.
#
# -Wno-incompatible-function-pointer-types fires three times, on streamio.c:25,
# :26 and :27 -- `struct qinit strdata = { strput, NULL, nulldev, ... }', where
# the qopen member is `long (*)()' (src/sys/h/stream.h:35) and nulldev and
# nilopen are declared `int' at :23.  Authentic, in an initialiser, so the
# alternative is editing Bell Labs' declaration.
#
# It is benign HERE and it is the same-register-return class CLAUDE.md records
# for `extern float atof()'.  THE MECHANISM IS NOT THE OBVIOUS ONE, and this
# comment said the obvious one until it was measured: the top half of x0 is not
# "unspecified garbage".  Any write to w0 zeroes bits 63:32 architecturally, so
# `return 1' and `return a+b' both come back zero-extended -- and a function
# whose int return is another call's result emits NO truncation at all, so a
# 64-bit pointer arrives intact.  Compiled and read, at -O0 and -O2.
#
# So the rule for a future driver is sharper than "declare a qopen long", and
# it is a rule about VALUES:
#
#   A qopen must return 0, or 1, or a real `struct inode *'.  NEVER a negative
#   int.  `return -1' becomes 0x00000000ffffffff, which stopen:124 does not see
#   as NULL and stopen:131 does not see as 1 -- so the open "succeeds" and
#   hands its caller an inode pointer of 0xffffffff.  -1 is not a hypothetical
#   convention: ufalloc() in this very tree uses it.
#
# The pointer-returning case works today by accident of code shape rather than
# by the type being right, which is the worse half: it would break on a
# different clang, a different -O, or a callee that spills across the return.
# Declaring a qopen `long' costs nothing and removes the accident.
#
# -Wno-extra-tokens is src/sys/h/file.h:16, `#endif CHAOS' -- the 1985 spelling
# of `#endif /* CHAOS */', which C89 made a constraint violation and every
# compiler still accepts.  Pure dialect.
STREAMIOFLAGS = $(KERNFLAGS) -DKERNEL -fcommon \
                -Wno-incompatible-function-pointer-types -Wno-extra-tokens
#
# It is deliberately NOT in KERNFLAGS.  stream.c compiles clean without any of
# the four, and widening a flag set for one file's sake is how a suppression
# that was argued for once ends up covering code nobody argued about.

# §8a step 5 -- V8's filesystem, six imported files plus the services they call.
#
# THEY GO IN A SUBDIRECTORY AND THAT IS NOT TIDINESS.  src/sys/sys/subr.c and
# shim/kern/sys/subr.c are two different files with the same basename, and the
# flat $(BUILD)/kern/%.o naming the rules above use would give them one object.
# Make would not warn: it would compile whichever rule it matched and the other
# file would silently not be in the archive.  $(BUILD)/kern/v8fs/ mirrors the
# source split instead, and the group is named for what it is.
#
# ONE FLAG BEYOND STREAMIOFLAGS, for the imported half: nothing.  All six
# compile under exactly the flags streamio.c needed, which is the useful
# measurement -- the dialect was already right and step 5 added no suppression.
#
# main.o IS OURS AND IT IS IN THIS GROUP RATHER THAN WITH slp/fio/subr/ioconf,
# because it needs -DKERNEL and -fcommon exactly as v8fs.o does: it writes the
# STRONG definitions for inode/ninode/inodeNINODE/buf/buffers/nbuf that the
# authentic headers declare as tentative inside #ifdef KERNEL, and it needs
# struct mount and struct filsys from behind the same guard.  Compiling it with
# bare SHIMFLAGS would hide those declarations and it would define six globals
# nothing had declared.
V8FS_OBJ = $(BUILD)/kern/v8fs/alloc.o $(BUILD)/kern/v8fs/iget.o \
           $(BUILD)/kern/v8fs/nami.o $(BUILD)/kern/v8fs/rdwri.o \
           $(BUILD)/kern/v8fs/subr.o $(BUILD)/kern/v8fs/bio.o \
           $(BUILD)/kern/v8fs/v8fs.o $(BUILD)/kern/v8fs/main.o

KERN_LIB = $(BUILD)/kern/libv8kern.a
KERN_OBJ = $(BUILD)/kern/stream.o $(BUILD)/kern/machdep.o \
           $(BUILD)/kern/streamio.o $(BUILD)/kern/slp.o \
           $(BUILD)/kern/fio.o $(BUILD)/kern/subr.o $(BUILD)/kern/ioconf.o \
           $(BUILD)/kern/ttyld.o $(BUILD)/kern/partab.o $(V8FS_OBJ)

# imgdev.o IS NOT IN THE ARCHIVE, AND THAT IS THE SECOND PLACE IT HAS BEEN PUT.
# Adding it there broke the case asserting libv8kern imports only _memcpy,
# _setjmp and _longjmp -- the driver does host I/O by definition, so it brought
# _pread and _pwrite in with it.  The guard was right and the placement was
# wrong: a DRIVER SET IS PART OF A CONFIGURATION, not of the kernel library.
# config(8) is what chooses one on a real V8, and v8k_bdconf already stands in
# for config(8), so the object belongs on the link line of whatever is being
# configured -- the server, and the probe.  Keeping it out is what lets the
# archive keep saying it names nothing of the host's.
IMGDEV_OBJ = $(BUILD)/kern/imgdev.o

# The headers streamio.c reaches, split by who wrote them.  Seven are AUTHENTIC
# and live in src/sys/h/, so a quoted "../h/x.h" from src/sys/sys/ finds them
# first; four are OURS in shim/kern/h/, reached only because the authentic ones
# are absent and -Ishim/kern/dev turns "../h/" into that directory.  Nothing in
# the include syntax says which is which, which is the property the seam wants.
#
# BUF.H CHANGED SIDES IN §8a STEP 5c WITHOUT ANY FILE BEING EDITED, which is
# the property this split exists to make visible and the one time it has
# actually moved.  streamio.c:4 has always said `#include "../h/buf.h"'.  It
# used to reach shim/kern/h/buf.h -- ours, two constants -- because src/sys/h/
# had none.  bio.c's import brought the authentic src/sys/h/buf.h into the tree,
# and the includer's-directory-first rule silently handed streamio.c that one
# instead.  Measured with `clang -M', which is the only way to see it: the
# source line is identical either way.  Our copy is deleted; nothing that
# included it used either constant.
KERN_AUTH_H = $(SRC)/sys/h/stream.h $(SRC)/sys/h/dir.h $(SRC)/sys/h/inode.h \
              $(SRC)/sys/h/ioctl.h $(SRC)/sys/h/ttyld.h $(SRC)/sys/h/file.h \
              $(SRC)/sys/h/inline.h $(SRC)/sys/h/buf.h \
              $(SRC)/sys/research/sparam.h
KERN_SHIM_H = $(ROOT)shim/kern/h/param.h $(ROOT)shim/kern/h/mtpr.h \
              $(ROOT)shim/kern/h/conf.h $(ROOT)shim/kern/h/user.h \
              $(ROOT)shim/kern/h/proc.h \
              $(SRC)/include/setjmp.h

libv8kern: $(KERN_LIB)
# THE `rm -f' IS LOAD-BEARING AND NOT FOR THE USUAL REASON.  This archive has
# TWO MEMBERS NAMED subr.o -- shim/kern/sys/subr.c (copyin, psignal, gsignal;
# 7 symbols) and the authentic src/sys/sys/subr.c (bmap and friends; 60) --
# because ar records a member's BASENAME and the two live in different build
# directories.  Measured: `ar t $(KERN_LIB) | sort | uniq -d' prints subr.o.
#
# It links correctly because Mach-O's symbol index maps each name to a member
# OFFSET, so both are reachable; every suite here proves it, since fsprobe
# calls into both.  What would break it is turning this into an incremental
# `ar r $@ $?', the obvious optimisation: `ar r' replaces a member BY NAME, and
# with two candidates the choice is the tool's rather than ours.  Building the
# archive from empty every time makes the question not arise.
$(KERN_LIB): $(KERN_OBJ)
	@rm -f $@ && ar rcs $@ $(KERN_OBJ)
	@echo "built $@"

$(BUILD)/kern/stream.o: $(SRC)/sys/dev/stream.c $(SRC)/sys/h/stream.h \
                        $(SRC)/sys/research/sparam.h \
                        $(ROOT)shim/kern/h/param.h $(ROOT)shim/kern/h/mtpr.h \
                        $(ROOT)shim/kern/h/conf.h
	@mkdir -p $(BUILD)/kern
	$(HOSTCC) $(KERNFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/kern/streamio.o: $(SRC)/sys/sys/streamio.c $(KERN_AUTH_H) $(KERN_SHIM_H)
	@mkdir -p $(BUILD)/kern
	$(HOSTCC) $(STREAMIOFLAGS) $(DEPFLAGS) -c $< -o $@

# ttyld.c -- V8's tty line discipline, line discipline 0 (conf/devices:75),
# imported byte-identical.  Its sixth include is "tty.h", the per-configuration
# header config(8) would have generated; ours is shim/kern/dev/tty.h and
# KERNFLAGS' -Ishim/kern/dev is already what finds it, by the same fall-through
# that turns "../h/param.h" into the stand-in.  Nothing new in the flags, and
# no edit to Bell Labs' source, which is the whole point of the arrangement.
#
# shim/kern/dev/tty.h is named as a prerequisite because it is the one input
# here that no other rule mentions -- change NTTY and this object must rebuild.
#
# ONE FLAG BEYOND KERNFLAGS, and only one: ttyld.c:34-35 initialise struct
# qinit -- whose qopen and qclose are `long (*)()' -- with ttyopen and ttyclose,
# which are int.  Identical to streamio.c's case and suppressed for the
# identical reason, but spelled separately rather than folded into KERNFLAGS,
# because stream.c compiles clean without it and a suppression argued for once
# should not quietly cover code nobody argued about.  ttyld does NOT need
# STREAMIOFLAGS' other three: no -DKERNEL, no -fcommon (tty[NTTY] is the only
# tentative definition and there is one translation unit), no -Wno-extra-tokens.
#
# What makes the suppression safe here is measured, not assumed.  CLAUDE.md's
# rule is that a qopen must never return a negative int, because `return -1'
# widens to 0x00000000ffffffff and stopen sees neither NULL nor 1.  ttyopen
# (ttyld.c:41-63) returns exactly 0 and 1 on every path, and tests/streams
# asserts the pair rather than this sentence.
TTYLDFLAGS = $(KERNFLAGS) -Wno-incompatible-function-pointer-types
$(BUILD)/kern/ttyld.o: $(SRC)/sys/dev/ttyld.c $(SRC)/sys/h/stream.h \
                       $(SRC)/sys/h/ioctl.h $(SRC)/sys/h/ttyld.h \
                       $(SRC)/sys/research/sparam.h \
                       $(ROOT)shim/kern/dev/tty.h \
                       $(ROOT)shim/kern/h/param.h $(ROOT)shim/kern/h/mtpr.h \
                       $(ROOT)shim/kern/h/conf.h
	@mkdir -p $(BUILD)/kern
	$(HOSTCC) $(TTYLDFLAGS) $(DEPFLAGS) -c $< -o $@

# partab.c is 51 lines of pure data -- the character parity/delay table ttyld.c
# declares as `extern char partab[]'.  It includes nothing at all, so it has no
# header prerequisites and that is not an omission.
$(BUILD)/kern/partab.o: $(SRC)/sys/sys/partab.c
	@mkdir -p $(BUILD)/kern
	$(HOSTCC) $(KERNFLAGS) $(DEPFLAGS) -c $< -o $@

# §8a step 5's imported half.  Two rules rather than one because the sources
# are in two upstream directories, and the prerequisite list is $(V8FS_H) --
# the headers step 5 added on top of what streamio.c already needed.
V8FS_H = $(ROOT)shim/kern/h/pte.h $(ROOT)shim/kern/h/vmmac.h \
         $(ROOT)shim/kern/h/vmparam.h $(ROOT)shim/kern/h/vmmeter.h \
         $(ROOT)shim/kern/h/vmsystm.h $(ROOT)shim/kern/h/filsys.h \
         $(ROOT)shim/kern/h/ino.h $(ROOT)shim/kern/h/fblk.h \
         $(SRC)/sys/h/systm.h $(SRC)/sys/h/mount.h $(SRC)/sys/h/cmap.h \
         $(SRC)/sys/h/acct.h $(SRC)/sys/h/vlimit.h $(SRC)/sys/h/vm.h \
         $(SRC)/sys/h/seg.h $(SRC)/sys/h/trace.h $(SRC)/sys/h/inline.h

$(BUILD)/kern/v8fs/%.o: $(SRC)/sys/sys/%.c $(KERN_AUTH_H) $(KERN_SHIM_H) $(V8FS_H)
	@mkdir -p $(BUILD)/kern/v8fs
	$(HOSTCC) $(STREAMIOFLAGS) $(DEPFLAGS) -c $< -o $@

# bio.c gets THREE FLAGS THE OTHER FIVE DO NOT, and they are spelled here
# rather than folded into STREAMIOFLAGS for ttyld's reason: a suppression
# argued for once should not quietly cover code nobody argued about.  All
# three fire only in swap(), the VAX paging path:
#
#   -Wno-pointer-to-int-cast   :555 `vtopte(p, btop(addr))' -- btop is
#   -Wno-int-to-pointer-cast   `((unsigned)(x)) >> PGSHIFT', so an 8-byte
#                              caddr_t is truncated to 4; :561 casts the
#                              result back.  THESE ARE REAL LP64 TRUNCATIONS
#                              IN AUTHENTIC SOURCE and they are suppressed
#                              rather than fixed because the line between them
#                              -- :556's vtopte() -- PANICS.  A deviation here
#                              would be editing Bell Labs' source to correct
#                              arithmetic that cannot execute.  If a pager ever
#                              lands, these two lines are the first to fix, and
#                              src/sys/PORTING.md says so.
#   -Wno-unused-but-set-variable  :643, swkill()'s `s' -- upstream saves an spl
#                              level it never restores, in the process-killing
#                              path.  Upstream's own dead store.
$(BUILD)/kern/v8fs/bio.o: $(SRC)/sys/dev/bio.c $(KERN_AUTH_H) $(KERN_SHIM_H) \
                          $(V8FS_H)
	@mkdir -p $(BUILD)/kern/v8fs
	$(HOSTCC) $(STREAMIOFLAGS) -Wno-pointer-to-int-cast \
	    -Wno-int-to-pointer-cast -Wno-unused-but-set-variable \
	    $(DEPFLAGS) -c $< -o $@

$(BUILD)/kern/v8fs/%.o: $(SRC)/sys/dev/%.c $(KERN_AUTH_H) $(KERN_SHIM_H) $(V8FS_H)
	@mkdir -p $(BUILD)/kern/v8fs
	$(HOSTCC) $(STREAMIOFLAGS) $(DEPFLAGS) -c $< -o $@

# v8fs.c is OURS -- the twenty services and the switch tables -- so it is
# modern C like slp.c and fio.c.  It needs three flags they do not, and each
# one is because this is the only modern C here that has to satisfy AUTHENTIC
# K&R declarations of functions IT DEFINES:
#
#   -DKERNEL   src/sys/h/mount.h:21's `struct mount mount[NMOUNT]' is inside
#              #ifdef KERNEL, and findmount walks it.  Same for inode.h's
#              inode/ninode/rootdir block.  Upstream's own flag; conf/makefile:23.
#   -fcommon   because those are TENTATIVE definitions -- the K&R idiom where
#              forty objects name the same array and the linker merges the
#              commons.  Identical to STREAMIOFLAGS' reason.
#   -Wno-deprecated-non-prototype
#              findmount and mfind are DEFINED old-style here, matching
#              mount.h:22 and cmap.h:36.  A prototyped definition conflicts
#              with those `()' declarations -- and not only by C99's rule, but
#              in C89 too, because dev_t is u_short and a u_short parameter is
#              affected by the default argument promotions.  v8fs.c says so at
#              the definition.
V8FSFLAGS = $(SHIMFLAGS) -DKERNEL -fcommon -Wno-deprecated-non-prototype \
            -Wno-extra-tokens
# ONE FLAG BEYOND V8FSFLAGS, on this object alone rather than in the group's
# variable, for ttyld.o's reason: a suppression argued for once should not
# quietly cover code nobody argued about.
#
#   -Wno-logical-not-parentheses   allocmount is sys3.c:239-259 TRANSCRIBED,
#   and :247 is `if(!mp->m_flags&M_MOUNTED)'.  `!' binds tighter than `&', so
#   the expression is `(m_flags == 0) & M_MOUNTED' rather than the
#   `!(m_flags & M_MOUNTED)' it reads as -- and the two agree, because
#   M_MOUNTED is 1 and it is the only flag `struct mount' has.  Upstream's
#   quirk, correct by a coincidence between a precedence and a bit value.  It
#   is transcribed rather than repaired so the body still diffs against
#   sys3.c, and v8fs.c argues that at the definition, including which way it
#   would fail if a second flag were ever added.
$(BUILD)/kern/v8fs/v8fs.o: $(ROOT)shim/kern/sys/v8fs.c $(KERN_AUTH_H) \
                           $(KERN_SHIM_H) $(V8FS_H) $(ROOT)shim/v8sys/rawsys.h
	@mkdir -p $(BUILD)/kern/v8fs
	$(HOSTCC) $(V8FSFLAGS) -Wno-logical-not-parentheses $(DEPFLAGS) \
	    -I$(ROOT)shim/kern/dev -c $< -o $@

# main.o -- the tables and the startup.  Same flags as v8fs.o and the same
# reason; V8FS_OBJ above says which.  It is the one file here that includes the
# AUTHENTIC buf.h by full path, so that edge is named explicitly: $(KERN_AUTH_H)
# carries it since §8a step 5c, when shim/kern/h/buf.h was found to be dead and
# deleted.
$(BUILD)/kern/v8fs/main.o: $(ROOT)shim/kern/sys/main.c $(KERN_AUTH_H) \
                           $(KERN_SHIM_H) $(V8FS_H)
	@mkdir -p $(BUILD)/kern/v8fs
	$(HOSTCC) $(V8FSFLAGS) $(DEPFLAGS) -I$(ROOT)shim/kern/dev -c $< -o $@

$(BUILD)/kern/machdep.o: $(ROOT)shim/kern/dev/machdep.c \
                         $(ROOT)shim/kern/h/param.h $(ROOT)shim/kern/h/mtpr.h \
                         $(ROOT)shim/v8sys/rawsys.h
	@mkdir -p $(BUILD)/kern
	$(HOSTCC) $(SHIMFLAGS) $(DEPFLAGS) -I$(ROOT)shim/kern/dev -c $< -o $@

# imgdev.o -- the block device whose platter is a host file.  It was in
# tests/streams/fsprobe.c until §8a step 5e, held out of the shim by the
# unconsumed-component rule; the v8fs server is the consumer that rule was
# waiting for, and the probe now uses the same one so its 236 cases are
# coverage for the server's block layer too.
#
# -DKERNEL and -fcommon for v8fs.o's reasons: struct buf's B_ flags and the
# bdevsw row come from behind the same guard, and the authentic headers it
# reaches declare tentative definitions.
$(BUILD)/kern/imgdev.o: $(ROOT)shim/kern/dev/imgdev.c \
                        $(ROOT)shim/kern/dev/imgdev.h $(KERN_AUTH_H) \
                        $(KERN_SHIM_H)
	@mkdir -p $(BUILD)/kern
	$(HOSTCC) $(V8FSFLAGS) $(DEPFLAGS) -I$(ROOT)shim/kern/dev -c $< -o $@

# ---------------------------------------------------------------------------
# v8fsd -- the v8fs file server.  §8a step 5e.
#
# A HOST BINARY, and that is the whole design rather than a convenience.  §8a
# step 5e was costed as a fourth type inside vfs.c and cannot be built: linking
# libv8kern beside a V8 program is 56 symbol collisions over 29 programs, 25 of
# them silent, and a descriptor table in process memory does not survive exec.
# Out here neither arises -- the same relationship tests/streams/fsprobe.c
# already has to this archive, which is what says it links cleanly in a program
# that is not a V8 one.
#
# It links $(P9_SRC), the same codec source libv8sys.a gets, compiled here with
# the server's flags.  shim/p9/p9.h says why one file rather than two.
#
# setjmp.o for fsprobe's reason: libv8kern imports _setjmp and _longjmp, which
# are V8's own (slp.c's tsleep), and tests/kmemu asserts the archive imports
# exactly those two plus _memcpy.
# ---------------------------------------------------------------------------
V8FSD = $(BUILD)/v8fsd/v8fsd
v8fsd: $(V8FSD)
$(V8FSD): $(ROOT)shim/v8fsd/v8fsd.c $(P9_SRC) $(P9_HOSTIO) $(ROOT)shim/p9/p9.h \
          $(IMGDEV_OBJ) $(KERN_LIB) $(BUILD)/libc/setjmp.o \
          $(KERN_AUTH_H) $(KERN_SHIM_H)
	@mkdir -p $(BUILD)/v8fsd
	$(HOSTCC) $(V8FSFLAGS) -I$(ROOT)shim/kern/dev -o $@ \
	    $(ROOT)shim/v8fsd/v8fsd.c $(P9_SRC) $(P9_HOSTIO) \
	    $(IMGDEV_OBJ) $(KERN_LIB) $(BUILD)/libc/setjmp.o

# THE SAME SERVER UNDER UndefinedBehaviorSanitizer, AND IT EXISTS BECAUSE A
# MUTATION WOULD NOT FIRE.
#
# do_seek's overflow guard tests before adding; an auditor found the first
# version adding and then testing, which is signed overflow.  Reverting it was
# mutated and the suite stayed green -- and for once that means neither of the
# two usual things.  The case is not vacuous and the code is not dead: every
# reachable overflow here wraps to a NEGATIVE value, so the broken form reaches
# the same EINVAL by executing undefined behaviour.  The defect is that the
# compiler is entitled to assume the overflow cannot happen and delete the
# check, which no behavioural test can see.
#
# A sanitizer can.  Measured: with the guard restored to its broken form this
# binary prints `signed integer overflow: 4611686018427387904 +
# 4611686018427387904' at v8fsd.c and dies; with the guard as written it is
# silent through the whole probe, `ls -l' and a 28000-byte cat.
#
# -fno-sanitize-recover=all so the FIRST diagnostic kills the process: a server
# that survives its own UB report would leave the suite asserting on stderr
# alone, and stderr is easier to lose than a dead peer.
#
# ONLY OUR OWN CODE IS INSTRUMENTED, and that is deliberate rather than a
# limitation.  $(KERN_LIB) is already compiled, so Bell Labs' 1985 kernel is
# linked in uninstrumented -- UB in imported source is upstream's and would be
# a different project.  What this covers is v8fsd.c and the codec.
V8FSD_UBSAN = $(BUILD)/v8fsd/v8fsd-ubsan
v8fsd-ubsan: $(V8FSD_UBSAN)
$(V8FSD_UBSAN): $(ROOT)shim/v8fsd/v8fsd.c $(P9_SRC) $(P9_HOSTIO) $(ROOT)shim/p9/p9.h \
          $(IMGDEV_OBJ) $(KERN_LIB) $(BUILD)/libc/setjmp.o \
          $(KERN_AUTH_H) $(KERN_SHIM_H)
	@mkdir -p $(BUILD)/v8fsd
	$(HOSTCC) $(V8FSFLAGS) -fsanitize=undefined -fno-sanitize-recover=all \
	    -I$(ROOT)shim/kern/dev -o $@ \
	    $(ROOT)shim/v8fsd/v8fsd.c $(P9_SRC) $(P9_HOSTIO) \
	    $(IMGDEV_OBJ) $(KERN_LIB) $(BUILD)/libc/setjmp.o

# The machine-dependent half of the SYSCALL side.  Modern C, like machdep.c and
# for the same reason: it is ours.  slp.c is tsleep/wakeup, fio.c the u-area
# and the file table, subr.c the twelve mechanical names, ioconf.c the stream
# configuration table config(8) would have generated.
#
# ONE FLAG BEYOND SHIMFLAGS, and it is there because this is the only modern C
# in the tree that includes Bell Labs' headers directly: fio.c reaches
# src/sys/h/file.h for `struct file', and file.h:16 is `#endif CHAOS' -- the
# 1985 spelling of `#endif /* CHAOS */'.  Dialect, in an authentic file, so it
# is suppressed rather than edited.  Same flag and same reason as STREAMIOFLAGS.
$(BUILD)/kern/%.o: $(ROOT)shim/kern/sys/%.c $(KERN_AUTH_H) $(KERN_SHIM_H) \
                   $(ROOT)shim/v8sys/rawsys.h
	@mkdir -p $(BUILD)/kern
	$(HOSTCC) $(SHIMFLAGS) -Wno-extra-tokens $(DEPFLAGS) \
	    -I$(ROOT)shim/kern/dev -c $< -o $@

# ---------------------------------------------------------------------------
# libv8c -- V8's own libc, compiled by v8cc, on top of the shim.
#
# This is authentic V8 source with three kinds of exception, each marked in the
# file that replaces it: the VAX assembly leaf routines (string ops, doprnt,
# the float primitives), which had to be rewritten; the syscall stubs, which
# are the shim; and crt0.
# ---------------------------------------------------------------------------
LIBCSRC = $(SRC)/libc
# gen/: authentic V8, except ieeefp.c and memops.c, which replace VAX assembly
# and say so at the top.  isatty is NOT here -- the shim owns it, since it is a
# question about the host terminal rather than about V8.
#
# sleep is the one that had to WAIT rather than be written.  It is alarm + a
# handler + `for(;;) pause()', so it needs signal DELIVERY, which this port did
# not have until shim/v8sys/sigtramp.s: every handler was installed with a null
# trampoline.  Imported once before that and taken straight back out, because it
# hung and took rm and tail with it.  The source needs no patch of any kind --
# its blob hash still matches upstream's -- which is the point: what was broken
# was underneath it.
LIBC_GEN = malloc ecvt ieeefp errlst perror memops \
           ctype atoi atol abs max min sgn gcd lcm \
           index rindex strrchr strdup strtok strcatn strcmpn strcpyn \
           calloc getenv qsort swab mktemp abort rand getopt stty \
           execvp exec getwd ftw valloc tell iread l3tol ltol3 nlist \
           opendir readdir closedir seekdir telldir \
           ctime timezone ttyname cttyname getlogin ttyslot \
           tolower toupper atof sleep linedis
LIBC_C  = $(patsubst %,$(LIBCSRC)/gen/%.c,$(LIBC_GEN)) \
          $(LIBCSRC)/stdio/data.c $(LIBCSRC)/stdio/doprnt.c \
          $(LIBCSRC)/stdio/printf.c $(LIBCSRC)/stdio/fprintf.c \
          $(LIBCSRC)/stdio/filbuf.c $(LIBCSRC)/stdio/flsbuf.c \
          $(LIBCSRC)/stdio/fputs.c $(LIBCSRC)/stdio/fgets.c \
          $(LIBCSRC)/stdio/fopen.c $(LIBCSRC)/stdio/fgetc.c \
          $(LIBCSRC)/stdio/fputc.c $(LIBCSRC)/stdio/ungetc.c \
          $(LIBCSRC)/stdio/rew.c $(LIBCSRC)/stdio/setbuf.c \
          $(LIBCSRC)/stdio/clrerr.c $(LIBCSRC)/stdio/error.c \
          $(LIBCSRC)/stdio/puts.c $(LIBCSRC)/stdio/gets.c \
          $(LIBCSRC)/stdio/getchar.c $(LIBCSRC)/stdio/putchar.c \
          $(LIBCSRC)/stdio/rdwr.c $(LIBCSRC)/stdio/sprintf.c \
          $(LIBCSRC)/stdio/freopen.c $(LIBCSRC)/stdio/fdopen.c \
          $(LIBCSRC)/stdio/fseek.c $(LIBCSRC)/stdio/ftell.c \
          $(LIBCSRC)/stdio/strout.c $(LIBCSRC)/stdio/getw.c \
          $(LIBCSRC)/stdio/getpw.c $(LIBCSRC)/stdio/putw.c $(LIBCSRC)/stdio/tmpnam.c \
          $(LIBCSRC)/stdio/getpwent.c $(LIBCSRC)/stdio/getpwuid.c \
          $(LIBCSRC)/stdio/getgrent.c $(LIBCSRC)/stdio/getgrnam.c \
          $(LIBCSRC)/stdio/getpwnam.c $(LIBCSRC)/stdio/getgrgid.c \
          $(LIBCSRC)/stdio/getpass.c $(LIBCSRC)/gen/crypt.c \
          $(LIBCSRC)/stdio/fstab.c \
          $(LIBCSRC)/stdio/doscan.c $(LIBCSRC)/stdio/scanf.c \
          $(LIBCSRC)/stdio/popen.c $(LIBCSRC)/stdio/system.c \
          $(patsubst %,$(LIBCSRC)/math/%.c,$(LIBC_MATH))
# THE MATH IS V8'S OWN, AND IT WAS PRODUCING WRONG ANSWERS BEFORE IT WAS.
#
# There is no libm in V8's tree -- the math lives in libc/math, which is why
# "port libm" was the wrong shape of question and reading the tree was the only
# way to find that out.  Until this list existed, pic and grap resolved sqrt,
# sin, atan2 and the rest out of the HOST's libSystem, and tests/kmemu carried
# libm as an allowed leak on the grounds that V8 shipped one and we had not
# built it.
#
# That was not a purity problem, it was a CORRECTNESS one, and the reason is the
# calling convention.  v8cc passes every argument positionally in x0-x7,
# including doubles -- the emitted code is
#
#	ldr	d16, [x9]         ; the double
#	str	d16, [sp, #384]
#	ldr	x0, [sp, #384]    ; ...into an INTEGER register
#	bl	_sqrt
#
# and AAPCS64 says a double argument goes in d0.  Apple's sqrt read whatever
# happened to be in d0.  Measured: sqrt(2.0) returned 0.000000, and `pic'
# rejected `circle rad 0.5' with "circle has invalid radius 0.000000".  Every
# drawing pic or grap ever produced here was geometrically wrong, and the wavec
# suite did not see it because it asserts that drawing commands come out, not
# that the numbers in them are right.
#
# Compiling V8's math with v8cc puts both ends of the call on the SAME
# convention, which is the whole fix -- all eighteen files build unmodified and
# all ten functions pic and grap use now agree to six decimal places.
#
# fabs is NOT here: ieeefp.o already defines it, and taking upstream's would be
# a duplicate symbol at link time.  The rest of upstream's directory is included
# even though nothing calls j0 or gamma yet, because leaving them out is a
# second list to keep in step with the first -- the DIRSIZ lesson.
LIBC_MATH = asin atan erf exp floor gamma hypot j0 j1 jn \
            log pow sin sinh sqrt tan tanh
# scanf/doscan, popen and system were missing until spell needed scanf.  A
# missing libc function does not fail the link: it is resolved from -lSystem,
# silently, and for a VARIADIC function that is an ABI mismatch rather than a
# compatible substitute -- v8cc passes arguments in x0-x7, Apple's ABI passes
# variadic arguments on the stack.  `sscanf("017651423", "%lo", &h)` took the
# host's sscanf, read the format pointer as the destination and faulted.  A
# non-variadic one would have quietly worked and hidden the gap.
#
# The string routines ship as .C -- portable references beside the VAX assembly
# that V8 actually built.  They are what a machine without those instructions
# was meant to use.
LIBC_STR = strlen strcpy strcmp strcat strncpy strncmp strncat strchr \
           strcspn strpbrk strspn

LIBC_OBJ = $(patsubst $(LIBCSRC)/%.c,$(BUILD)/libc/%.o,$(LIBC_C)) \
           $(patsubst %,$(BUILD)/libc/gen/%.o,$(LIBC_STR)) \
           $(BUILD)/libc/setjmp.o

# setjmp/longjmp: hand-written ARM64, since the VAX version walks call frames.
# See the note at the top of compiler/setjmp.s.
$(BUILD)/libc/setjmp.o: $(ROOT)compiler/setjmp.s
	@mkdir -p $(BUILD)/libc
	$(HOSTCC) -c $< -o $@

libv8c: $(BUILD)/libc/libv8c.a
# WHAT `rm -f' STILL DOES NOT FIX, so nobody has to rediscover it: make cannot
# notice a prerequisite that was REMOVED.  Drop a file from LIBC_GEN and the
# archive is still newer than every object left, so the rule does not run at
# all and the member survives.  Measured: taking sleep out of the list and
# deleting its source left sleep.o in libv8c.a, and rm and tail went on linking
# it -- which for sleep meant they went on hanging.
#
# Making the archives depend on $(MAKEFILE_LIST) does fix it and was tried.
# It costs too much: every edit to this file, comments included, relinks all
# four archives and the 221 build actions below them, and the first `make test'
# after any such edit fails because tests/deps runs before the other suites'
# prerequisites are rebuilt.  A stamp file holding the object list would work,
# but that is machinery for a case tests/deps already catches -- each of the
# `X -> libc' cases there fails if X leaves the list.  Guarded, not prevented.
$(BUILD)/libc/libv8c.a: $(LIBC_OBJ)
	@rm -f $@ && ar rcs $@ $(LIBC_OBJ)
	@echo "built $@"

# Compiled by v8cc itself -- this is the point.  V8ROOT has to be set for the
# driver to find its passes.
V8CCRUN = V8ROOT=$(ROOTFS) $(ROOTFS)/bin/cc -I$(LIBCSRC)/stdio

# ...except libc, which is compiled by the SEED driver, because the installed
# driver is linked against the library this rule produces.  See CCSEED at the
# top of this file.
#
# This does NOT weaken the claim.  A driver compiles nothing; it execs cpp and
# ccom, and both drivers exec the same two binaries with the same arguments.
# The objects are byte-identical either way -- tests/v8cc asserts exactly that,
# by compiling one libc source with each driver and comparing.  What differs is
# the process, not the code it emits.
V8CCSEED = V8ROOT=$(ROOTFS) $(CCSEED) -I$(LIBCSRC)/stdio

$(BUILD)/libc/gen/%.o: $(LIBCSRC)/gen/%.c $(SEED_DEPS)
	@mkdir -p $(BUILD)/libc/gen
	$(V8CCSEED) -c -o $@ $<

$(BUILD)/libc/gen/%.o: $(LIBCSRC)/gen/%.C $(SEED_DEPS)
	@mkdir -p $(BUILD)/libc/gen
	cp $< $(BUILD)/libc/gen/$*.c
	$(V8CCSEED) -c -o $@ $(BUILD)/libc/gen/$*.c

$(BUILD)/libc/stdio/%.o: $(LIBCSRC)/stdio/%.c $(SEED_DEPS)
	@mkdir -p $(BUILD)/libc/stdio
	$(V8CCSEED) -c -o $@ $<

$(BUILD)/libc/math/%.o: $(LIBCSRC)/math/%.c $(SEED_DEPS)
	@mkdir -p $(BUILD)/libc/math
	$(V8CCSEED) -c -o $@ $<

# ---------------------------------------------------------------------------
# v8cc -- V8's cc(1) driver, retargeted, in two stages.
#
# Paths resolve from $V8ROOT; the assembler and link editor are the host's,
# reached through clang.  Everything else -- the flag surface, the temp-file
# dance, the order of the passes -- is V8's own driver.
#
# STAGE 0, cc-seed: clang-built, host libc, invisible to the shim.  Exists only
# to compile libv8c; never installed.
#
# STAGE 1, v8cc: the same cc.c, compiled by cc-seed and linked freestanding
# against crt0 + libv8c + libv8sys exactly like every other V8 program.  THIS is
# the one that gets installed, and being a V8 binary is the point: every path it
# opens and every pass it execs now goes through libv8sys, so `V8JAIL=strict cc`
# is a claim the jail can actually check.  Before this it was a clang binary
# with zero V8 symbols, and the jail could not see a single thing it did.
#
# No $(STAGE0_COMPAT) on either line: cc.c stopped needing _sobuf when it
# stopped being linked against the host stdio.
# ---------------------------------------------------------------------------
V8CC_INC = -DV8ROOT_DEFAULT='"$(V8ROOT_DEFAULT)"'

$(CCSEED): $(SRC)/cmd/cc.c
	@mkdir -p $(BUILD)/cc
	$(HOSTCC) $(KRFLAGS) $(V8CC_INC) -o $@ $<
	@echo "built $@"

v8cc: $(BUILD)/cc/v8cc
$(BUILD)/cc/cc.o: $(SRC)/cmd/cc.c $(SEED_DEPS)
	@mkdir -p $(BUILD)/cc
	$(V8CCSEED) $(V8CC_INC) -c -o $@ $<
$(BUILD)/cc/v8cc: $(BUILD)/cc/cc.o $(V8DEPS)
	$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(BUILD)/cc/cc.o $(V8LIBS)
	@echo "built $@"

# ---------------------------------------------------------------------------
# sh -- the Bourne shell, the centrepiece of the port.
#
# Compiled by v8cc from authentic V8 source, linked freestanding against V8's
# own libc.  The object list is the makefile's OFILES, unchanged; profile.c is
# not in it there either.  src/cmd/sh/PORTING.md records the four LP64 changes.
# ---------------------------------------------------------------------------
SHSRC = $(SRC)/cmd/sh
SH_OBJ_NAMES = setbrk blok stak cmd fault main word string name args xec \
               service error io print macro expand ctype msg defs pathserv \
               func spname
SH_OBJ = $(patsubst %,$(BUILD)/sh/%.o,$(SH_OBJ_NAMES))

sh: $(BUILD)/sh/sh
$(BUILD)/sh/sh: $(SH_OBJ) $(BUILD)/crt0.o $(BUILD)/libc/libv8c.a \
                $(BUILD)/v8sys/libv8stubs.a $(BUILD)/v8sys/libv8sys.a
	$(HOSTCC) -nostdlib -e _v8start -o $@ $(BUILD)/crt0.o $(SH_OBJ) \
	    $(BUILD)/libc/libv8c.a $(BUILD)/v8sys/libv8stubs.a \
	    $(BUILD)/v8sys/libv8sys.a -lSystem
	@echo "built $@"

$(BUILD)/sh/%.o: $(SHSRC)/%.c $(V8CC_DEPS)
	@mkdir -p $(BUILD)/sh
	$(V8CCRUN) -I$(SHSRC) -c -o $@ $<

$(SH_OBJ): $(wildcard $(SHSRC)/*.h)

# ---------------------------------------------------------------------------
# nroff -- Wave C.  Built from the original makefile's NFILES, with the same
# -DSMALLER -DNROFF it used; troff is the same sources with t6/t10 instead of
# n6/n10.  See src/cmd/troff/PORTING.md.
# ---------------------------------------------------------------------------
TROFFSRC = $(SRC)/cmd/troff
NROFF_NAMES = n1 n2 n3 n4 n5 n6 n7 n8 n9 n10 ni nii hytab suftab
TROFF_NAMES = n1 n2 n3 n4 n5 t6 n7 n8 n9 t10 ni nii hytab suftab
NROFF_OBJ = $(patsubst %,$(BUILD)/nroff/%.o,$(NROFF_NAMES))
TROFF_OBJ = $(patsubst %,$(BUILD)/troff/%.o,$(TROFF_NAMES))

nroff: $(BUILD)/nroff/nroff
$(BUILD)/nroff/nroff: $(NROFF_OBJ) $(BUILD)/crt0.o $(BUILD)/libc/libv8c.a \
                      $(BUILD)/v8sys/libv8stubs.a $(BUILD)/v8sys/libv8sys.a
	$(HOSTCC) -nostdlib -e _v8start -o $@ $(BUILD)/crt0.o $(NROFF_OBJ) \
	    $(BUILD)/libc/libv8c.a $(BUILD)/v8sys/libv8stubs.a \
	    $(BUILD)/v8sys/libv8sys.a -lSystem
	@echo "built $@"

troff: $(BUILD)/troff/troff
$(BUILD)/troff/troff: $(TROFF_OBJ) $(BUILD)/crt0.o $(BUILD)/libc/libv8c.a \
                      $(BUILD)/v8sys/libv8stubs.a $(BUILD)/v8sys/libv8sys.a
	$(HOSTCC) -nostdlib -e _v8start -o $@ $(BUILD)/crt0.o $(TROFF_OBJ) \
	    $(BUILD)/libc/libv8c.a $(BUILD)/v8sys/libv8stubs.a \
	    $(BUILD)/v8sys/libv8sys.a -lSystem
	@echo "built $@"

$(BUILD)/nroff/%.o: $(TROFFSRC)/%.c $(V8CC_DEPS)
	@mkdir -p $(BUILD)/nroff
	$(V8CCRUN) -DINCORE -DSMALLER -DNROFF -I$(TROFFSRC) -c -o $@ $<
$(BUILD)/troff/%.o: $(TROFFSRC)/%.c $(V8CC_DEPS)
	@mkdir -p $(BUILD)/troff
	$(V8CCRUN) -DINCORE -I$(TROFFSRC) -c -o $@ $<
$(NROFF_OBJ) $(TROFF_OBJ): $(wildcard $(TROFFSRC)/*.h)

# ---------------------------------------------------------------------------
# tbl -- the table preprocessor.  Its header is called t..c and the .c files
# include it by that name, so a *.c wildcard would compile the header as a
# translation unit; the object list is explicit instead.
# ---------------------------------------------------------------------------
TBLSRC = $(SRC)/cmd/tbl
TBL_NAMES = t0 t1 t2 t3 t4 t5 t6 t7 t8 t9 tb tc te tf tg ti tm tr ts tt tu tv
TBL_OBJ = $(patsubst %,$(BUILD)/tbl/%.o,$(TBL_NAMES))

tbl: $(BUILD)/tbl/tbl
$(BUILD)/tbl/tbl: $(TBL_OBJ) $(BUILD)/crt0.o $(BUILD)/libc/libv8c.a \
                  $(BUILD)/v8sys/libv8stubs.a $(BUILD)/v8sys/libv8sys.a
	$(HOSTCC) -nostdlib -e _v8start -o $@ $(BUILD)/crt0.o $(TBL_OBJ) \
	    $(BUILD)/libc/libv8c.a $(BUILD)/v8sys/libv8stubs.a \
	    $(BUILD)/v8sys/libv8sys.a -lSystem
	@echo "built $@"

$(BUILD)/tbl/%.o: $(TBLSRC)/%.c $(V8CC_DEPS)
	@mkdir -p $(BUILD)/tbl
	$(V8CCRUN) -I$(TBLSRC) -c -o $@ $<
# t..c is tbl's header under a name that is not .h, and every object #includes
# it.  Invisible to any dependency scanner; spelled out here.
$(TBL_OBJ): $(TBLSRC)/t..c

# ---------------------------------------------------------------------------
# yacc and eqn.
#
# V8's OWN yacc, not the host's: modern bison emits #elif, which Reiser's 1978
# cpp does not understand, so y.tab.c would not even preprocess.  V8's accepts
# the `={ ... }` action syntax natively too, so no YACCFIX is needed.  It needs
# yaccpar at /usr/lib, which the shim resolves inside $V8ROOT.
# ---------------------------------------------------------------------------
YACCSRC = $(SRC)/cmd/yacc
YACC_OBJ = $(patsubst %,$(BUILD)/yacc/%.o,y1 y2 y3 y4)

v8yacc: $(BUILD)/yacc/yacc
$(BUILD)/yacc/yacc: $(YACC_OBJ) $(BUILD)/crt0.o $(BUILD)/libc/libv8c.a \
                    $(BUILD)/v8sys/libv8stubs.a $(BUILD)/v8sys/libv8sys.a
	$(HOSTCC) -nostdlib -e _v8start -o $@ $(BUILD)/crt0.o $(YACC_OBJ) \
	    $(BUILD)/libc/libv8c.a $(BUILD)/v8sys/libv8stubs.a \
	    $(BUILD)/v8sys/libv8sys.a -lSystem
	@echo "built $@"
$(BUILD)/yacc/%.o: $(YACCSRC)/%.c $(V8CC_DEPS)
	@mkdir -p $(BUILD)/yacc
	$(V8CCRUN) -I$(YACCSRC) -c -o $@ $<
# y1..y4 all `#include "dextern"`, and dextern in turn includes "files".
# Neither ends in .h, so neither is visible to anything that scans for headers.
$(YACC_OBJ): $(YACCSRC)/dextern $(YACCSRC)/files

# ---------------------------------------------------------------------------
# /bin -- the V8 world's own commands, as REAL INSTALLED BINARIES.
#
# There were none before this.  tests/wavea/run.sh compiled twelve programs
# into $TMP and deleted them on exit, so "Wave A: 78 pure filters" was backed
# by a passing suite and no artifacts.  Nothing could be installed, and nothing
# could be executed by anything other than that script.
#
# They are needed now because V8 make execs a shell for every command line
# (dosys.c, SHELLCOM "/bin/sh"), and the point of the bootstrap is that what
# that shell then runs -- cp, rm, sed, cc -- is V8 code too.  rm alone appears
# 26 times across the authentic makefiles.
#
# NOT here, deliberately: as, ld, ar, strip, nm.  The object format is Mach-O
# and porting V8's a.out assembler and link editor is out of scope (PLAN.md
# section 1).  That is the one hole in the jail, and it is a decision rather
# than an omission.
# ---------------------------------------------------------------------------
BINDIR = $(BUILD)/bin

# ---------------------------------------------------------------------------
# WHERE A COMMAND IS INSTALLED IS UPSTREAM'S DECISION, and upstream wrote it
# down -- twice, in two places that agree.
#
# This port put everything in /bin for as long as /bin was the only directory
# it had, and that was wrong for FORTY-ONE commands.  V8's /bin is a 56-entry
# root-filesystem set from the era when / had to fit on one pack; wc, tr, sort,
# sed, yacc, lex, dc and the rest live in /usr/bin.  So `ls /bin' inside the
# jail listed a machine that never existed.
#
# The two sources:
#
#	third_party/.../cmd/Admin/dest		the build description.  A shell
#		script that looks a name up in binfiles, etcfiles, libfiles and
#		falls through to /usr/bin.  It is what Admin/Mk calls to decide
#		where to put each of the bare cmd/*.c programs, and it is the
#		makefile-equivalent for the half of cmd/ that has no makefile.
#	third_party/.../v8/{bin,usr/bin,lib,etc}	the shipped tree itself.
#
# They agree on 41 of the 46 commands this port installs and the other five
# (bcd, head, morse, unexpand, yes) were never shipped at all, so only the
# first has an opinion -- /usr/bin, by fall-through.  Where they DO differ is
# outside this port: `login', `pstat', `rsh' and `track' are listed and not
# shipped, `[', `oops' and `tp' shipped and not listed.
#
# DERIVED RATHER THAN RETYPED, which is the whole point.  A list of 46
# destinations copied into this file would be right today and would not survive
# the next import; reading Bell Labs' own tables means a newly imported command
# lands where V8 put it without anyone deciding.  tests/wavea asserts the
# agreement from the far end, against the shipped directories rather than
# against these tables, so the two sources still check each other.
#
# ULIBFILES IS DELIBERATELY NOT READ, and the reason is a measurement rather
# than the one that used to be written here.  This said Admin/dest "greps a
# ulibfiles that is not in the directory".  IT IS IN THE DIRECTORY -- 51 names.
# What is true is that it is the table the two upstream sources DISAGREE about.
# Resolving all 128 listed names through dest and looking each up in the shipped
# tree: 122 agree, 36 were never shipped, and SIX disagree --
#
#	crontab  dest=/usr/lib  shipped=/etc
#	lint     dest=/usr/lib  shipped=/usr/bin
#	login    dest=/bin      shipped=/etc
#	man      dest=/usr/lib  shipped=/usr/bin
#	pstat    dest=/bin      shipped=/etc
#	uucp     dest=/usr/lib  shipped=/usr/bin
#
# Four of the six are ulibfiles entries that V8 shipped somewhere else, and one
# of them, `man', is a command this port installs.  Omitting the arm sends man
# to /usr/bin by fall-through, which is where the shipped tree has it -- so the
# omission is what makes us right, and adding the arm for symmetry would break
# it.  The shipped tree is the authority when they differ, because it is what
# the machine actually looked like.
#
# tests/wavea asserts the choice rather than leaving it as a comment: it
# recomputes the disagreement set and fails if anything this port installs
# enters it.  Import `lint' or `crontab' tomorrow and that case goes red, which
# is the only warning there would otherwise be.
# ---------------------------------------------------------------------------
# Read from src/, not third_party/: these are now imported with PROVENANCE like
# any other upstream file, because the rootfs stages them at /usr/src/cmd/Admin
# so Bell Labs' own Admin/Mk can run in the jail.  One copy, so the build-time
# derivation and the run-time script cannot disagree.
ADMIN    := $(SRC)/cmd/Admin
ADMINBIN := $(shell cat $(ADMIN)/binfiles)
ADMINETC := $(shell cat $(ADMIN)/etcfiles)
ADMINLIB := $(shell cat $(ADMIN)/libfiles)

# $(call v8dest,NAME) -> the directory under $(ROOTFS), without a leading slash.
#
# $(strip) is load-bearing and is not tidiness.  A backslash-newline inside a
# variable's VALUE becomes a single space and the next line's indentation is
# kept, so without it this returns "          etc" -- and make then splits
# $(ROOTFS)/          etc/mkfs into TWO targets, $(ROOTFS)/ and etc/mkfs.  Seen:
# "warning: overriding commands for target .../rootfs/" followed by
# "No rule to make target `usr/bin/touch'", neither of which names the cause.
#
# A THIRD SOURCE, and it disagrees with Admin/dest exactly once in eleven.
# Eleven imported makefiles state their own destination -- `mv lex
# $(DESTDIR)/usr/bin', `cp ps /bin', `D=/etc/quot' -- and they agree with the
# tables on ten: lex, cpp, make, yacc, ps, sh, df, quot, load, w.  The
# exception is `dump', which is in NONE of binfiles, etcfiles or libfiles, so
# the tables answer by FALL-THROUGH -- and a fall-through is "nobody said",
# not "V8 said /usr/bin".  Somebody did say: dump's own Makefile ends
# `mv dump $(DESTDIR)/etc', and its two siblings restor and dumpdir are both
# etcfiles entries.  So dump goes to /etc, and it is the only name here that
# needs this arm.
#
# Written as a list rather than parsed out of the makefiles at build time
# because a $(shell) per program is slow and $(MKDEST_$(1)) would read an
# undefined variable for every program that has no makefile -- the exact thing
# --warn-undefined-variables exists to catch, and the reason $(dirsrcs) is
# spelled out too.  tests/wavea does the general sweep instead: it recomputes
# the destination from all eleven makefiles and fails if the disagreement set
# is anything but this list, which is the same treatment the six-name
# Admin/dest-versus-shipped-tree disagreement already gets.
MKFILEETC := dump

v8dest = $(strip \
         $(if $(filter $(1),$(ADMINBIN)),bin,\
         $(if $(filter $(1),$(ADMINETC)),etc,\
         $(if $(filter $(1),$(ADMINLIB)),lib,\
         $(if $(filter $(1),$(MKFILEETC)),etc,usr/bin)))))

# $(call v8install,NAMES) -> the installed paths for a list of commands.
v8install = $(foreach p,$(1),$(ROOTFS)/$(call v8dest,$(p))/$(p))

# Single-source commands.  rm, touch, ln, test and chmod were imported for the
# bootstrap: the authentic makefiles cannot run without them and none of them
# had been ported.
V8BIN = cat echo cmp rm touch ln test chmod pwd wc head tail tee tr \
        grep fgrep sort uniq comm cut paste col fold expand unexpand rev \
        basename printenv split sum od pr look join number seq yes ls v8 newer \
        deroff \
        ascii bcd cal morse ptx units vis date \
        who \
\
        apply asa byteyears checkeq chgrp chown coreid cpio ctags dd du file \
        find fsplit getopt getuid id idiff last mesg mkstr news nice random \
        readslow sdiff sleep tabs tar time tty xstr crypt kill sync hang tape \
\
        spline uuencode uudecode stty mc

# Every single-file command imported into src/cmd is now installed, not just
# the ones the bootstrap needs.  `cc' is the only .c in there that is NOT in
# this list, because the driver has its own rule (it links against libv8c and
# is what compiles libv8c -- see the cc-seed note).  Keep it that way:
#	ls src/cmd/*.c | sed 's|.*/||;s|\.c$$||'
# should differ from $(V8BIN) by `cc' alone.  tests/wavea asserts it.

V8BIN_BUILT   = $(patsubst %,$(BINDIR)/%,$(V8BIN))
V8BIN_INSTALL = $(call v8install,$(V8BIN))

# Data files two of those commands read by absolute path at RUN time, the same
# shape as troff's tmac and grap.defines.  Without them `units' answers "no
# table" and `ptx' says "Cannot open  file /usr/lib/eign" -- both of which look
# like a broken port and are just a missing install.  Upstream's files, unchanged.
V8LIBDATA = $(ROOTFS)/usr/lib/units $(ROOTFS)/usr/lib/eign

$(ROOTFS)/usr/lib/units: $(REPO)third_party/Research-Unix-v8/v8/usr/lib/units
	@mkdir -p $(@D) && cp $< $@
$(ROOTFS)/usr/lib/eign: $(REPO)third_party/Research-Unix-v8/v8/usr/lib/eign
	@mkdir -p $(@D) && cp $< $@

# ---------------------------------------------------------------------------
# COMMANDS UPSTREAM KEEPS IN A DIRECTORY OF THEIR OWN, imported and then never
# built.  Eleven of them, and the V8 world had no `cp', no `mv', no `mkdir',
# no `rmdir' and no `sed'.
#
# HOW ELEVEN PROGRAMS HID.  $(V8BIN) above is checked for completeness -- the
# comment there says `ls src/cmd/*.c' should differ from it by `cc' alone, and
# tests/wavea asserts exactly that.  The glob is the hole: it matches
# src/cmd/cat.c and never src/cmd/mv/mv.c, so a program upstream happens to
# keep in its own directory was invisible to the check that exists to find
# missing programs.  The test now walks directories too.
#
# It surfaced from the other end, which is the argument for rung 5: four
# upstream makefiles (eqn, pic, grap, make) died on `Cannot load mv' -- V8's
# make trying to run `mv y.tab.c grap.c', the ordinary yacc rename.  Our own
# rules never needed mv because they were written to work.
#
# All eleven compile under v8cc with ZERO warnings and needed no source change.
V8DIRBIN = cp dc ed factor mkdir mv primes rmdir sed fmt tsort \
           cb su compress

# The objects each one is built from, spelled out for ALL of them rather than
# defaulted for the eight whose single source is named after the program.  The
# short version costs nothing and reads better:
#
#	dirsrcs = $(if $(dirsrc_$(1)),$(dirsrc_$(1)),$(1))
#
# but $(if $(dirsrc_cp),...) READS an undefined variable, and this Makefile is
# checked with --warn-undefined-variables precisely because a variable used
# above its definition expands to nothing in a prerequisite and the dependency
# silently is not there.  A probe that is legitimate and a mistake that is not
# are indistinguishable to that flag, so the flag wins and these are explicit.
# .claude/hooks/check-makefile.sh refused the short version on the way in.
#
# The three multi-object lists are upstream's own: sed's makefile says
# `sed: sed0.o sed1.o', fmt's says `OBJ = fmt.o head.o'.
dirsrc_cp     = cp
dirsrc_dc     = dc
dirsrc_ed     = ed
dirsrc_factor = factor
dirsrc_mkdir  = mkdir
dirsrc_mv     = mv
dirsrc_primes = primes
dirsrc_rmdir  = rmdir
dirsrc_sed    = sed0 sed1
dirsrc_fmt    = fmt head
dirsrc_tsort  = refstore subs tsort
dirsrc_cb       = cb
dirsrc_su       = su
dirsrc_compress = compress
dirsrcs       = $(dirsrc_$(1))

# GENERATED RULES, and the reason is a measured limit rather than a preference.
# The natural spelling is a static pattern rule,
#
#	$(DIRBIN): $(BINDIR)/%: $(SRC)/cmd/%/%.c
#
# and GNU make substitutes the stem for only the FIRST % in a prerequisite
# pattern, so the second stays literal and make reports
# "No rule to make target src/cmd/mv/%.c".  .SECONDEXPANSION would do it and
# changes how EVERY prerequisite in this file is expanded, which is too large a
# blast radius for eleven programs.  foreach/eval is contained.
# Objects go in $(BUILD)/<prog>/, NOT $(BINDIR)/<prog>/, and that is not a
# style choice: the binary is $(BINDIR)/<prog>, so objects under the same name
# make the path a directory and a file at once.  ld says
# "open() failed, errno=21 (Is a directory)", which reads as a linker problem
# and is a layout one.  Every other multi-file program here already uses
# $(BUILD)/<prog>/ -- build/stage0/ps, build/stage0/eqn -- so this follows them.
define dirobj_rule
$(BUILD)/$(1)/$(2).o: $$(SRC)/cmd/$(1)/$(2).c $$(V8CC_DEPS)
	@mkdir -p $$(@D)
	@$$(V8CCRUN) -I$$(SRC)/cmd/$(1) -c -o $$@ $$<
endef

define dirprog_rule
$(BINDIR)/$(1): $(patsubst %,$(BUILD)/$(1)/%.o,$(2)) $$(V8DEPS)
	@mkdir -p $$(@D)
	@$$(HOSTCC) $$(V8LDFLAGS) -o $$@ $$(BUILD)/crt0.o \
	    $(patsubst %,$(BUILD)/$(1)/%.o,$(2)) $$(V8LIBS)
endef

$(foreach p,$(V8DIRBIN),\
  $(foreach s,$(call dirsrcs,$(p)),$(eval $(call dirobj_rule,$(p),$(s))))\
  $(eval $(call dirprog_rule,$(p),$(call dirsrcs,$(p)))))

# ---------------------------------------------------------------------------
# diff -- TWO programs and THREE compiled-in paths, so it cannot use the
# generic $(V8DIRBIN) rule above.
#
# Upstream's Makefile is `-DDIFF -DDIFFH -DPR', each a string naming where the
# helper will be INSTALLED, because diff execs them at run time.  That makes
# the flags part of the install layout rather than of the compile, and this
# port has already been bitten by exactly that: forty-one commands moved out of
# /bin when $(call v8dest,...) started reading Bell Labs' own tables.
#
# So the paths are DERIVED, not copied.  Upstream says PR=/bin/pr; Admin's
# tables put pr in /bin here too, and $(call v8dest,pr) is what says so -- if
# that ever disagrees, diff follows the tables rather than the comment.  diffh
# is the exception and is NOT in any table: upstream's own makefile is the only
# source for its location, and it says /usr/lib/diffh, which is the same
# fall-through-versus-stated distinction the cpp and dump entries record.
DIFFPATHS = -DDIFF='"/$(call v8dest,diff)/diff"' \
            -DDIFFH='"/usr/lib/diffh"' \
            -DPR='"/$(call v8dest,pr)/pr"'

$(BUILD)/diff/%.o: $(SRC)/cmd/diff/%.c $(V8CC_DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) -I$(SRC)/cmd/diff $(DIFFPATHS) -c -o $@ $<

$(BINDIR)/diff: $(BUILD)/diff/diff.o $(BUILD)/diff/diffdir.o \
                $(BUILD)/diff/diffreg.o $(V8DEPS)
	@mkdir -p $(@D)
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o \
	    $(BUILD)/diff/diff.o $(BUILD)/diff/diffdir.o \
	    $(BUILD)/diff/diffreg.o $(V8LIBS)
	@echo "built $@"

# diffh is a SEPARATE program that diff execs for -h, and it goes to /usr/lib
# rather than a bin directory -- upstream's makefile is the only thing that
# says so, and it is the same kind of statement cpp's `/lib' turned out to be.
$(BINDIR)/diffh: $(BUILD)/diff/diffh.o $(V8DEPS)
	@mkdir -p $(@D)
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(BUILD)/diff/diffh.o $(V8LIBS)
	@echo "built $@"

$(ROOTFS)/usr/lib/diffh: $(BINDIR)/diffh
	@mkdir -p $(@D) && cp $< $@

DIFF_INSTALL = $(ROOTFS)/$(call v8dest,diff)/diff $(ROOTFS)/usr/lib/diffh

# cb.o's #INCLUDED NON-HEADER, and the fourteenth member of that list.
# Upstream's own Makefile says it outright -- `cb.o: cb.c cbtype.c cbtype.h' --
# and building cbtype.c as a translation unit instead gives a DUPLICATE SYMBOL,
# because it defines the _ctype_ table that cbtype.h declares extern.  The
# linker caught it; a dependency scanner would not have.
$(BUILD)/cb/cb.o: $(SRC)/cmd/cb/cbtype.c $(SRC)/cmd/cb/cbtype.h

# ul -- the first consumer of libtermcap, and the reason it cannot use the
# generic $(V8DIRBIN) rule: that rule links $(V8LIBS) and nothing else, and ul
# is upstream's `cc -o ul ul.o -ltermcap'.
#
# libtermcap goes BEFORE $(V8LIBS) on the link line, because it is a consumer
# of libc rather than a peer -- termcap.c calls strlen, strcpy and open, and an
# archive only satisfies references the linker has already seen.
#
# All three sources agree on /usr/bin for once, so there is nothing to decide:
# ul is in neither Admin/binfiles nor Admin/etcfiles (so Admin/dest falls
# through), its own Makefile says `cp ul /usr/bin', and the shipped tree has
# usr/bin/ul.  Contrast cpp and dump, where the fall-through disagrees with
# both of the other two.
$(BUILD)/ul/%.o: $(SRC)/cmd/ul/%.c $(V8CC_DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) -I$(SRC)/cmd/ul -c -o $@ $<

$(BINDIR)/ul: $(BUILD)/ul/ul.o $(BUILD)/termlib/libtermcap.a $(V8DEPS)
	@mkdir -p $(@D)
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(BUILD)/ul/ul.o \
	    $(BUILD)/termlib/libtermcap.a $(V8LIBS)
	@echo "built $@"

UL_INSTALL = $(ROOTFS)/$(call v8dest,ul)/ul

# ---------------------------------------------------------------------------
# ex(1) -- which IS vi(1).  The largest program in this port: 28 objects,
# 19358 lines, and the second consumer of libtermcap.
#
# THE OBJECT LIST IS UPSTREAM'S OBJS, SPELLED OUT, minus exactly one member.
# `strings.o' is the xstr artefact: upstream compiles every file through
# `cc -E | xstr -c -', which lifts string literals into a shared table, and
# then builds strings.o by rewriting assembler with an ed script -- the same
# read-only-shared-text optimisation that stops sh and cpp at rung 5 and that
# arm64 Mach-O structurally cannot do.  It is a pure SIZE optimisation and
# nothing depends on it: measured, the only references to `xstr' in the whole
# directory are two comments and the `char xstr[1];' dummies in expreserve and
# exrecover, which exist precisely because those two do NOT go through it.
#
# THE FLAGS ARE UPSTREAM'S, including -DVFORK.  Its makefile is
# `OPTIONS= -DCRYPT -DLISPCODE -DCHDIR -DUCVISUAL -DVFORK -DVMUNIX -DSTDIO'
# with `CFLAGS= -DTABS=8 $(OPTIONS)'.  VFORK is safe rather than tolerated:
# ex's one vfork (ex_unix.c:149) is the ordinary fork-and-exec, and the shim
# maps vfork to fork with the reasoning at syscall.c:1550 -- "plain fork is
# always a correct substitute and never a dangerous one".
#
# Do NOT spell these as a glob.  ex*.c also matches exrecover.c and
# expreserve.c, which are SEPARATE programs with their own main().
EXSRC  = $(SRC)/cmd/ex
EXOPTS = -DCRYPT -DLISPCODE -DCHDIR -DUCVISUAL -DVFORK -DVMUNIX -DSTDIO -DTABS=8
EX_NAMES = ex ex_addr ex_cmds ex_cmds2 ex_cmdsub ex_data ex_extern ex_get \
           ex_io ex_put ex_re ex_set ex_subr ex_temp ex_tty ex_unix \
           ex_v ex_vadj ex_vget ex_vmain ex_voper ex_vops ex_vops2 ex_vops3 \
           ex_vput ex_vwind printf bcopy
EX_OBJ = $(patsubst %,$(BUILD)/ex/%.o,$(EX_NAMES))

$(BUILD)/ex/%.o: $(EXSRC)/%.c $(V8CC_DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) $(EXOPTS) -I$(EXSRC) -c -o $@ $<

# Every object includes ex.h, which includes ex_tune.h and ex_vars.h; ex_tty.h
# and the rest are named because a header is invisible to the .d files a rule
# like this does not generate.  Upstream says `${OBJS}: ex_vars.h' and stops
# there, which is one edge of the several that exist.
$(EX_OBJ): $(EXSRC)/ex.h $(EXSRC)/ex_tune.h $(EXSRC)/ex_vars.h

$(BINDIR)/ex: $(EX_OBJ) $(BUILD)/termlib/libtermcap.a $(V8DEPS)
	@mkdir -p $(@D)
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(EX_OBJ) \
	    $(BUILD)/termlib/libtermcap.a $(V8LIBS)
	@echo "built $@"

# THE THREE OTHER NAMES ARE HARD LINKS, and the set is MEASURED rather than
# taken from the makefile.  Upstream's install arms variously mention vi, view,
# edit and `e'; the shipped tree has usr/bin/{ex,vi,view,edit} at 116736 bytes
# each and NO usr/bin/e.  So `e' is an arm that was never run, and four names
# is what V8 actually had.
EX_LINKS = vi view edit
EX_INSTALL = $(ROOTFS)/$(call v8dest,ex)/ex \
             $(patsubst %,$(ROOTFS)/$(call v8dest,ex)/%,$(EX_LINKS))

$(patsubst %,$(ROOTFS)/$(call v8dest,ex)/%,$(EX_LINKS)): $(ROOTFS)/$(call v8dest,ex)/ex
	@rm -f $@ && ln $< $@

# ---------------------------------------------------------------------------
# awk -- a yacc grammar, a lex scanner, six ordinary objects, and a NINTH
# source file that does not exist until the build makes it.
#
# THE GENERATOR IS THE REASON THIS CANNOT BE A $(V8DIRBIN) ENTRY.  proctab.c
# holds `Cell *(*proctab[])()' -- one function pointer per grammar token, in
# token order -- and awk's own makefile builds it by compiling maketab.c,
# running it, and capturing its stdout.  maketab reads y.tab.h, so the table
# cannot be written by hand without going stale the moment the grammar gains a
# token; upstream's makefile opens with four lines of comment saying exactly
# that ("This makefile is wrong -- it doesn't properly recompile everything
# when a new token is added to awk.g.y.  Watch out!").  The dependency here is
# the one upstream wished it had: proctab.c <- maketab <- y.tab.c.
#
# maketab is built by V8's cc and RUN, which is upstream's own mechanism and
# not a host convenience -- $(YACC) and $(LEX) in the grap and pic blocks are
# V8 binaries executed by this Makefile for the same reason.  It is the third
# kind of build-time V8 program in the tree.
#
# The redirection goes through a temporary because .DELETE_ON_ERROR is not set
# here: `> proctab.c' creates the file before maketab runs, so a maketab that
# died would leave an empty proctab.c newer than its prerequisites and the next
# make would call it current.  That is the stale-object failure this tree has
# already paid for twice, arriving through a shell redirect rather than a rule.
AWKSRC = $(SRC)/cmd/awk
AWK_NAMES = b lib main parse run tran
AWK_OBJ = $(patsubst %,$(BUILD)/awk/%.o,$(AWK_NAMES)) \
          $(BUILD)/awk/awkgram.o $(BUILD)/awk/awklex.o $(BUILD)/awk/proctab.o

$(BINDIR)/awk: $(AWK_OBJ) $(V8DEPS)
	@mkdir -p $(@D)
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(AWK_OBJ) $(V8LIBS)
	@echo "built $@"

# y.tab.h comes out of this same invocation and is deliberately not a target of
# its own: GNU make 3.81 has no grouped targets, so naming both would be two
# rules sharing a recipe and would race under -j.  Everything that needs the
# header depends on y.tab.c instead, as the grap block does.
$(BUILD)/awk/y.tab.c: $(AWKSRC)/awk.g.y $(YACC) $(ROOTFS_YACCPAR)
	@mkdir -p $(BUILD)/awk
	cd $(BUILD)/awk && V8ROOT=$(ROOTFS) $(YACC) -d $(AWKSRC)/awk.g.y

$(BUILD)/awk/lex.yy.c: $(AWKSRC)/awk.lx.l $(LEX) $(ROOTFS_NCFORM) $(BUILD)/awk/y.tab.c
	@mkdir -p $(BUILD)/awk
	cd $(BUILD)/awk && V8ROOT=$(ROOTFS) $(LEX) $(AWKSRC)/awk.lx.l

$(BUILD)/awk/maketab: $(AWKSRC)/maketab.c $(BUILD)/awk/y.tab.c $(V8CC_DEPS) $(V8DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) -I$(BUILD)/awk -I$(AWKSRC) -c -o $(BUILD)/awk/maketab.o $<
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(BUILD)/awk/maketab.o $(V8LIBS)

# maketab opens "y.tab.h" by that name, so it has to run in the directory yacc
# wrote it in.
$(BUILD)/awk/proctab.c: $(BUILD)/awk/maketab
	cd $(BUILD)/awk && V8ROOT=$(ROOTFS) ./maketab > proctab.c.tmp \
	    && mv proctab.c.tmp proctab.c

$(BUILD)/awk/awkgram.o: $(BUILD)/awk/y.tab.c $(V8CC_DEPS)
	@$(V8CCRUN) -I$(BUILD)/awk -I$(AWKSRC) -c -o $@ $(BUILD)/awk/y.tab.c
$(BUILD)/awk/awklex.o: $(BUILD)/awk/lex.yy.c $(V8CC_DEPS)
	@$(V8CCRUN) -I$(BUILD)/awk -I$(AWKSRC) -c -o $@ $(BUILD)/awk/lex.yy.c
$(BUILD)/awk/proctab.o: $(BUILD)/awk/proctab.c $(V8CC_DEPS)
	@$(V8CCRUN) -I$(BUILD)/awk -I$(AWKSRC) -c -o $@ $(BUILD)/awk/proctab.c
$(BUILD)/awk/%.o: $(AWKSRC)/%.c $(BUILD)/awk/y.tab.c $(V8CC_DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) -I$(BUILD)/awk -I$(AWKSRC) -c -o $@ $<

# Every one of the nine includes awk.h, generated ones included -- maketab
# writes `#include "awk.h"' as proctab.c's first line.  Upstream says
# `$(OFILES): prevy.tab.h awk.h'; the y.tab.c edge is already in the rules
# above, so this is the other half of that line.
$(AWK_OBJ) $(BUILD)/awk/maketab.o: $(AWKSRC)/awk.h

# ---------------------------------------------------------------------------
# Wave A2 batch 2c -- four directory programs, and each is here for a DIFFERENT
# build idiom rather than for its size.  Together they are the shapes the other
# fifty-one will need.
#
# expr   a grammar and NOTHING ELSE: one .y, one object, no lexer file at all
#        (the scanner is C in the third section).  The minimal yacc program.
# m4     three .c and a .y, which is grap's shape without the lexer.
# diff3  the SPELL SHAPE: the command in /usr/bin is a SHELL SCRIPT and the
#        binary it execs lives in /usr/lib, exactly as spell/spellprog and
#        diff/diffh already do here.
# pack   two programs from one directory and a HARD LINK for the third name,
#        which is ex/vi's shape at a smaller scale.
# ---------------------------------------------------------------------------
EXPRSRC = $(SRC)/cmd/expr
$(BUILD)/expr/y.tab.c: $(EXPRSRC)/expr.y $(YACC) $(ROOTFS_YACCPAR)
	@mkdir -p $(BUILD)/expr
	cd $(BUILD)/expr && V8ROOT=$(ROOTFS) $(YACC) $(EXPRSRC)/expr.y
$(BUILD)/expr/expr.o: $(BUILD)/expr/y.tab.c $(EXPRSRC)/regexp.h $(V8CC_DEPS)
	@$(V8CCRUN) -I$(BUILD)/expr -I$(EXPRSRC) -c -o $@ $(BUILD)/expr/y.tab.c
$(BINDIR)/expr: $(BUILD)/expr/expr.o $(V8DEPS)
	@mkdir -p $(@D)
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(BUILD)/expr/expr.o $(V8LIBS)

M4SRC = $(SRC)/cmd/m4
M4_NAMES = m4 m4ext m4macs
M4_OBJ = $(patsubst %,$(BUILD)/m4/%.o,$(M4_NAMES)) $(BUILD)/m4/m4y.o
$(BUILD)/m4/y.tab.c: $(M4SRC)/m4y.y $(YACC) $(ROOTFS_YACCPAR)
	@mkdir -p $(BUILD)/m4
	cd $(BUILD)/m4 && V8ROOT=$(ROOTFS) $(YACC) $(M4SRC)/m4y.y
$(BUILD)/m4/m4y.o: $(BUILD)/m4/y.tab.c $(V8CC_DEPS)
	@$(V8CCRUN) -I$(BUILD)/m4 -I$(M4SRC) -c -o $@ $(BUILD)/m4/y.tab.c
$(BUILD)/m4/%.o: $(M4SRC)/%.c $(V8CC_DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) -I$(BUILD)/m4 -I$(M4SRC) -c -o $@ $<
# Upstream: `m4.o m4ext.o m4macs.o : m4.h'.  m4y.o is deliberately NOT in that
# line -- upstream's own makefile leaves it out, and m4y.y does not include the
# header.  Copied as written rather than widened, because a dependency this
# tree ADDS is a claim about the program that upstream did not make.
$(patsubst %,$(BUILD)/m4/%.o,$(M4_NAMES)): $(M4SRC)/m4.h
$(BINDIR)/m4: $(M4_OBJ) $(V8DEPS)
	@mkdir -p $(@D)
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(M4_OBJ) $(V8LIBS)

# diff3: the BINARY is /usr/lib/diff3 and the COMMAND is a shell script, which
# is upstream's own split -- `mv diff3 /usr/lib; cp diff3.sh /usr/bin/diff3'.
# The script has no `#!' line, like Admin/Mk and Admin/dest, and V8's sh runs
# it; it hardcodes /usr/lib/diff3 and calls diff, both of which resolve inside
# the jail.  $(call v8dest,diff3) is NOT used: it would answer usr/bin by
# fall-through for the BINARY, which is the one thing upstream is explicit
# about.  Same treatment as spellprog.
DIFF3SRC = $(SRC)/cmd/diff3
$(BUILD)/diff3/diff3.o: $(DIFF3SRC)/diff3.c $(V8CC_DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) -c -o $@ $<
$(BINDIR)/diff3: $(BUILD)/diff3/diff3.o $(V8DEPS)
	@mkdir -p $(@D)
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(BUILD)/diff3/diff3.o $(V8LIBS)
$(ROOTFS)/usr/lib/diff3: $(BINDIR)/diff3
	@mkdir -p $(@D) && cp $< $@
$(ROOTFS)/usr/bin/diff3: $(DIFF3SRC)/diff3.sh
	@mkdir -p $(@D) && cp $< $@ && chmod +x $@
DIFF3_INSTALL = $(ROOTFS)/usr/lib/diff3 $(ROOTFS)/usr/bin/diff3

# pack/unpack, and pcat is a HARD LINK -- upstream's install is
# `ln /usr/bin/unpack /usr/bin/pcat'.  MEASURED rather than taken from the
# makefile, because the ex/vi step found an install arm that had never run:
# the shipped usr/bin/pcat and usr/bin/unpack are byte-identical at 10240, and
# the tarball simply lost the link on extraction.
PACKSRC = $(SRC)/cmd/pack
$(BUILD)/pack/%.o: $(PACKSRC)/%.c $(V8CC_DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) -c -o $@ $<
$(BINDIR)/pack: $(BUILD)/pack/pack.o $(V8DEPS)
	@mkdir -p $(@D)
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(BUILD)/pack/pack.o $(V8LIBS)
$(BINDIR)/unpack: $(BUILD)/pack/unpack.o $(V8DEPS)
	@mkdir -p $(@D)
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(BUILD)/pack/unpack.o $(V8LIBS)
$(ROOTFS)/$(call v8dest,pcat)/pcat: $(ROOTFS)/$(call v8dest,unpack)/unpack
	@rm -f $@ && ln $< $@
PACK_INSTALL = $(call v8install,pack unpack) $(ROOTFS)/$(call v8dest,pcat)/pcat

# THREE MORE NAMES BELL LABS' OWN INSTALL ARMS MAKE, from binaries this port
# ALREADY builds -- so they cost no source, no compile and no decision:
#
#	cmd/ed/Makefile:	rm -f /bin/e ; ln /bin/ed /bin/e
#	cmd/compress/Makefile:	ln $D/compress $D/uncompress
#				ln $D/uncompress $D/zcat
#
# MEASURED the way pcat above was, because an install arm can be one that never
# ran: shipped bin/e is byte-identical to bin/ed at 13312, and usr/bin/{compress,
# uncompress,zcat} are byte-identical at 16384, each trio with DIFFERENT inodes
# because the tarball lost the links on extraction.
#
# `e' IS THE ONE WORTH READING THE COMMENT FOR, because this Makefile already
# reasoned about it and reached a correct conclusion about the wrong program.
# The $(EX_LINKS) note says `e' is an ex arm that was never run -- and that is
# TRUE: ex/makefile:150 links it under BINDIR, ex/makefile:22 sets BINDIR to
# /usr/bin, and there is no usr/bin/e in the shipped tree.  But `e' is shipped,
# in /bin, and it is 13312 bytes against ex's 116736 -- it is ED's link, made by
# a different makefile entirely.  So the ex arm never ran AND `e' was a genuine
# gap, and only the first half had been written down.  That is the vi-is-ex rule
# (`when a program is absent from cmd/, check whether it is a LINK') with the
# check aimed at the wrong parent: a correct answer to the wrong question, which
# is more durable than a wrong one because nothing about it invites re-checking.
#
# Destinations are DERIVED per name, not shared: $(call v8install,...) asks
# $(call v8dest,...) for each, so if Bell Labs' tables ever put two of these in
# different directories the rule follows rather than silently co-locating them.
# Today they agree with the shipped tree -- `e' is in Admin/binfiles so it lands
# in /bin beside ed, and the compress trio fall through to /usr/bin.
$(call v8install,e): $(call v8install,ed)
	@rm -f $@ && ln $< $@
$(call v8install,uncompress zcat): $(call v8install,compress)
	@rm -f $@ && ln $< $@
LINKED_INSTALL = $(call v8install,e uncompress zcat)

# FIVE COMMANDS V8 SHIPPED AS SHELL SCRIPTS, so there is nothing to compile and
# the installed file IS the source.  They are the cheap half of the breadth
# remainder: measured, their whole dependency set (expr, nice, test, grep, sh)
# was already installed before this block existed.
#
# WHERE EACH ONE CAME FROM IS RECORDED BY WHERE IT SITS, which is why they are
# not all in one directory.  `false' has upstream source at usr/src/cmd/false.sh
# and lands in $(SRC)/cmd like every other imported program; the other four have
# NO usr/src copy at all, so what was imported is the shipped artefact itself
# and tools/import.sh maps it to $(SRC)/bin.  That asymmetry is information
# rather than untidiness -- PROVENANCE names the exact upstream path either way,
# and the split says at a glance which four are files Bell Labs shipped without
# shipping a source for.  It is the more/pg situation with the opposite verdict:
# those are opaque binaries and stay out, these are readable text, so installing
# upstream's own bytes is strictly more faithful than writing a replacement.
#
# `true' IS ZERO BYTES -- the original empty file, not a program that exits 0 --
# and that is upstream's, byte for byte, rather than an artefact of the import.
#
# Destinations are DERIVED, never spelled: $(call v8dest,...) reads Bell Labs'
# tables, which put true/false/nohup in Admin/binfiles (so /bin) and let
# dirname/whois fall through (so /usr/bin).  Checked against the shipped tree --
# all five agree, so there is no cpp/dump-shaped disagreement here.
V8SCRIPT          = true false nohup dirname whois
scriptsrc_true    = $(SRC)/bin/true
scriptsrc_false   = $(SRC)/cmd/false.sh
scriptsrc_nohup   = $(SRC)/bin/nohup
scriptsrc_dirname = $(SRC)/bin/dirname
scriptsrc_whois   = $(SRC)/bin/whois

# foreach/eval for $(V8DIRBIN)'s reason: a static pattern rule cannot expand a
# per-name source path, and .SECONDEXPANSION is too large a blast radius.
define script_rule
$(ROOTFS)/$(call v8dest,$(1))/$(1): $(scriptsrc_$(1))
	@mkdir -p $$(@D) && cp $$< $$@ && chmod +x $$@
endef
$(foreach p,$(V8SCRIPT),$(eval $(call script_rule,$(p))))

SCRIPT_INSTALL = $(call v8install,$(V8SCRIPT))

# ---------------------------------------------------------------------------
# Wave A2 batch 2d -- seven directory programs and one LIBRARY.  The batch was
# scoped as "ten, all pure computation"; three of the ten (graph, plot, prof)
# turned out to share one blocker and are task #89, so what is here is seven.
#
# libl   V8's lex library, and the SECOND library import after libtermlib.  pp
#        is the first program here to link -ll.
# hoc    Kernighan and Pike's calculator: a grammar, four .c, and a build idiom
#        nothing else here has -- see x.tab.h below.
# calendar the SPELL SHAPE at its largest: five artefacts from one directory,
#        two of them shell scripts, and the only member of the batch whose
#        makefile disagrees with Admin/dest.
# p      a pager, and the THIRD copy of spname.c in the tree.
# pp     one .c and one .l -- a lex file with NO yacc file, which is an idiom
#        none of 2a-2c had.
# newgrp, showq, dmesg  the three that install outside /usr/bin, and the last
#        two are grovelers in the load(1)/w(1) sense.
# ---------------------------------------------------------------------------

# libl: five tiny objects, and main.o is the interesting one.  It defines
# main(), which every lex program also does -- and that is fine rather than a
# collision, because a linker searches an archive only for symbols still
# undefined, and pp.o (an explicit object on the link line) has already
# satisfied crt0's reference.  Upstream relies on exactly that; the member
# exists for `lex spec.l && cc lex.yy.c -ll' with no main of your own.
LIBLSRC   = $(SRC)/lib/libl
LIBL_NAME = allprint main reject yyless yywrap
LIBL_OBJ  = $(patsubst %,$(BUILD)/libl/%.o,$(LIBL_NAME))
$(BUILD)/libl/%.o: $(LIBLSRC)/%.c $(V8CC_DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) -c -o $@ $<
$(BUILD)/libl/libl.a: $(LIBL_OBJ)
	@rm -f $@ && ar rcs $@ $(LIBL_OBJ)
	@echo "built $@"
$(ROOTFS)/usr/lib/libl.a: $(BUILD)/libl/libl.a
	@mkdir -p $(@D) && cp $< $@

# hoc.  The link needs no -lm: upstream says `$(CC) $(OBJS) -lm -o hoc' and
# V8's math is in libc here, with shim/libm/dummy.c reproducing the 216-byte
# empty archive V8 actually shipped -- so -lm links nothing on either machine.
#
# x.tab.h IS A REAL OPTIMISATION AND NOT A VESTIGE, which is worth saying
# because it reads like one: the three objects `#include "y.tab.h"' and NOTHING
# includes x.tab.h.  Upstream's rule is
#       x.tab.h: y.tab.h
#               -cmp -s x.tab.h y.tab.h || cp y.tab.h x.tab.h
#       code.o init.o symbol.o: x.tab.h
# so the PREREQUISITE is a file whose mtime moves only when the token numbers
# actually change -- content-addressed rebuild avoidance, in 1984 make, spelled
# with cmp and the `-' that lets the first run fail.  It is deliberately NOT
# reproduced: our y.tab.c rule already gates on the grammar, GNU make 3.81 has
# no grouped targets so y.tab.h is not a target of its own here (the awk block
# says why), and adding a second header target would be a rule that races
# under -j to save three compiles.  Recorded rather than copied.
HOCSRC   = $(SRC)/cmd/hoc
HOC_NAME = code init math symbol
HOC_OBJ  = $(patsubst %,$(BUILD)/hoc/%.o,$(HOC_NAME)) $(BUILD)/hoc/hocgram.o
$(BUILD)/hoc/y.tab.c: $(HOCSRC)/hoc.y $(YACC) $(ROOTFS_YACCPAR)
	@mkdir -p $(BUILD)/hoc
	cd $(BUILD)/hoc && V8ROOT=$(ROOTFS) $(YACC) -d $(HOCSRC)/hoc.y
$(BUILD)/hoc/hocgram.o: $(BUILD)/hoc/y.tab.c $(V8CC_DEPS)
	@$(V8CCRUN) -I$(BUILD)/hoc -I$(HOCSRC) -c -o $@ $(BUILD)/hoc/y.tab.c
$(BUILD)/hoc/%.o: $(HOCSRC)/%.c $(BUILD)/hoc/y.tab.c $(V8CC_DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) -I$(BUILD)/hoc -I$(HOCSRC) -c -o $@ $<
# Upstream: `hoc.o code.o init.o symbol.o: hoc.h' and `code.o init.o symbol.o:
# x.tab.h'.  math.o is deliberately absent from the first line and present in
# neither -- it includes <math.h> and nothing of hoc's -- so it is not listed.
$(BUILD)/hoc/hocgram.o $(patsubst %,$(BUILD)/hoc/%.o,code init symbol): $(HOCSRC)/hoc.h
$(BINDIR)/hoc: $(HOC_OBJ) $(V8DEPS)
	@mkdir -p $(@D)
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(HOC_OBJ) $(V8LIBS)

# calendar: FIVE artefacts, and the split is upstream's --
#       cp calendar /usr/bin;  cp calendar3 /usr/lib
#       mv calendar1 calendar2 calendar4 /usr/lib
# so the COMMAND is a shell script in /usr/bin and the four helpers (three
# binaries and a second script) are in /usr/lib.  spell/spellprog and
# diff3/diff3.sh are the same shape with fewer pieces.
#
# $(call v8dest,...) is not used for the helpers: none of calendar1..4 appears
# in any Admin table, so dest answers /usr/bin by FALL-THROUGH -- "nobody said",
# not "V8 said".  The makefile and the shipped tree both say /usr/lib, which is
# two sources against the fall-through, exactly cpp's pattern.  tests/wavea's
# disagreement set grows from three to seven for this reason.
#
# whereis is imported and NOT installed, deliberately: it is a script sitting in
# the source directory that upstream's makefile never mentions and that the
# shipped tree does not have.  A file in a source directory is not an artefact.
CALSRC  = $(SRC)/cmd/calendar
CAL_BIN = calendar1 calendar2 calendar4
$(BUILD)/calendar/%.o: $(CALSRC)/%.c $(V8CC_DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) -c -o $@ $<
# A STATIC pattern rule, not `$(BINDIR)/calendar%: .../calendar%.o'.  Chaining
# two pattern rules makes the object an INTERMEDIATE and make deletes it --
# measured, `rm .../calendar1.o calendar2.o calendar4.o' at the end of every
# build.  A static pattern rule's prerequisites are explicit for each named
# target, so the objects survive and tests/deps can name them.
$(patsubst %,$(BINDIR)/%,$(CAL_BIN)): $(BINDIR)/%: $(BUILD)/calendar/%.o $(V8DEPS)
	@mkdir -p $(@D)
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $< $(V8LIBS)
$(ROOTFS)/usr/lib/calendar1 $(ROOTFS)/usr/lib/calendar2 $(ROOTFS)/usr/lib/calendar4: \
    $(ROOTFS)/usr/lib/%: $(BINDIR)/%
	@mkdir -p $(@D) && cp $< $@
$(ROOTFS)/usr/bin/calendar: $(CALSRC)/calendar
	@mkdir -p $(@D) && cp $< $@ && chmod +x $@
$(ROOTFS)/usr/lib/calendar3: $(CALSRC)/calendar3
	@mkdir -p $(@D) && cp $< $@ && chmod +x $@
CAL_INSTALL = $(patsubst %,$(ROOTFS)/usr/lib/%,$(CAL_BIN)) \
              $(ROOTFS)/usr/bin/calendar $(ROOTFS)/usr/lib/calendar3

# p: three objects, and upstream's one dependency line is `pad.o: pad.h'.
PSRC   = $(SRC)/cmd/p
P_NAME = p pad spname
P_OBJ  = $(patsubst %,$(BUILD)/p/%.o,$(P_NAME))
$(BUILD)/p/%.o: $(PSRC)/%.c $(V8CC_DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) -c -o $@ $<
$(BUILD)/p/pad.o: $(PSRC)/pad.h
$(BINDIR)/p: $(P_OBJ) $(V8DEPS)
	@mkdir -p $(@D)
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(P_OBJ) $(V8LIBS)

# pp: a lex file with no grammar, so there is no y.tab.h and the scanner
# stands alone.  libl goes BEFORE $(V8LIBS) on the link line for the reason
# libtermcap does: it is a consumer of libc, not a peer of it.
PPSRC  = $(SRC)/cmd/pp
PP_OBJ = $(BUILD)/pp/pp.o $(BUILD)/pp/scan.o
$(BUILD)/pp/lex.yy.c: $(PPSRC)/scan.l $(LEX) $(ROOTFS_NCFORM)
	@mkdir -p $(BUILD)/pp
	cd $(BUILD)/pp && V8ROOT=$(ROOTFS) $(LEX) $(PPSRC)/scan.l
$(BUILD)/pp/scan.o: $(BUILD)/pp/lex.yy.c $(PPSRC)/pp.h $(V8CC_DEPS)
	@$(V8CCRUN) -I$(BUILD)/pp -I$(PPSRC) -c -o $@ $(BUILD)/pp/lex.yy.c
$(BUILD)/pp/pp.o: $(PPSRC)/pp.c $(PPSRC)/pp.h $(PPSRC)/dev.h $(V8CC_DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) -I$(PPSRC) -c -o $@ $<
$(BINDIR)/pp: $(PP_OBJ) $(BUILD)/libl/libl.a $(V8DEPS)
	@mkdir -p $(@D)
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(PP_OBJ) \
	    $(BUILD)/libl/libl.a $(V8LIBS)

# newgrp, showq, dmesg: one object each, and all three install OUTSIDE
# /usr/bin -- newgrp to /bin (Admin/binfiles), showq and dmesg to /etc
# (Admin/etcfiles).  $(call v8dest,...) derives that; nothing is spelled here.
#
# showq and dmesg are GROVELERS and neither can answer.  Both nlist() a kernel
# and read /dev/kmem: dmesg wants _msgbuf, showq wants eight STREAMS symbols
# (_queue _cblock _streams _qfreelist _dtlinfo _dkpinfo _Nblock _Nqueue).
# libkmemu manufactures a namelist and a /dev/kmem from one table and that
# table has none of the nine, so both say so and exit -- which is the load(1)
# and w(1) precedent, and the reason neither is linked against libkmemu here.
# Adding nine symbols would mean manufacturing a STREAMS subsystem and a kernel
# message buffer, which is a decision and not a side effect of this batch.
B2DSRC = $(SRC)/cmd
$(BUILD)/newgrp/newgrp.o: $(B2DSRC)/newgrp/newgrp.c $(V8CC_DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) -c -o $@ $<
$(BUILD)/showq/showq.o: $(B2DSRC)/showq/showq.c $(V8CC_DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) -c -o $@ $<
$(BUILD)/dmesg/dmesg.o: $(B2DSRC)/dmesg/dmesg.c $(V8CC_DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) -c -o $@ $<
$(BINDIR)/newgrp: $(BUILD)/newgrp/newgrp.o $(V8DEPS)
	@mkdir -p $(@D)
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $< $(V8LIBS)
$(BINDIR)/showq: $(BUILD)/showq/showq.o $(V8DEPS)
	@mkdir -p $(@D)
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $< $(V8LIBS)
$(BINDIR)/dmesg: $(BUILD)/dmesg/dmesg.o $(V8DEPS)
	@mkdir -p $(@D)
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $< $(V8LIBS)

# ---------------------------------------------------------------------------
# libplot -- the THIRD library import, and the one whose shape was recorded
# wrong before it was measured.
#
# usr/src/libplot is a SIBLING of usr/src/lib, which is why no survey had read
# it: seven device libraries, one per terminal.  whoami() is what says so --
# libplot returns "general", lib4014 returns "tek", lib2621 returns "hp".
#
# THE PLOT INTERFACE IS A HEADER, NOT A LIBRARY, WHICH IS THE WHOLE POINT AND
# WAS MISSED FIRST TIME.  <iplot.h> and <plot.h> are files of MACROS --
# `#define erase() printf("e\n")', `#define line(a,b,c,d) printf("li %g %g %g
# %g\n",...)' -- so a program that includes one emits plot(1)'s textual command
# language and calls NOTHING.  Measured: graph.o's undefined symbols are atof,
# ceil, fabs, floor, log10, malloc, printf, realloc, scanf, sprintf, strcat,
# strcpy, strlen, ungetc, and not one plot function.  Same for prof.o built
# with upstream's own -Dplot.
#
# So libplot.a is COMPLETE at two members.  subr.c's putnum() is the one thing
# the macros cannot express inline (the spline and fill macros pass arrays),
# and whoami.c names the device.  An earlier note in this tree said "-lplot
# would not have linked on a VAX either"; that was wrong, and it was wrong
# because a grep for `line(' cannot tell a macro invocation from a call.
#
# WHAT DOES NEED A DEVICE LIBRARY IS plot(1) ITSELF.  driver.c parses the
# language and dispatches through `struct pcall { void (*plot)(); }', so it
# references the functions for real: 28 of them undefined, measured, and
# lib4014 defines all 28 (38 in total).  That is what makes `tek' buildable
# and it is upstream's own link line, unchanged.
#
# THE BUILD IDIOM IS NEW HERE: the sources live inside an `ar' archive and the
# makefile extracts them before compiling.  Upstream:
#	lib4014.a: tek.c.a
#		mkdir xplot; cd xplot; ar x ../tek.c.a; cc -c -O *.c
#		cd xplot; ar rc ../lib4014.a *.o; rm -r xplot
# Reproduced as ONE rule per archive rather than per object, deliberately:
# the member list does not exist until the extraction has run, and $(wildcard)
# is expanded when make READS the rule, so a stamp-and-wildcard scheme would
# compile nothing on a clean tree and everything on the next run.  One rule is
# also upstream's own granularity.  31 files, under a second.
#
# PROVENANCE covers the BUNDLE, which is the honest unit: tools/import.sh
# records the blob hash of the one file upstream ships.  Nothing inside needed
# patching -- the LP64 and address-0 sweeps came back clean over all 937 lines
# -- so the question of how to record a patched member has not arisen.  When it
# does, the answer is to extract that member into src/ beside the bundle and
# record it separately; do not rewrite the archive.
LIBPLOTSRC = $(SRC)/libplot

$(BUILD)/libplot/libplot.a: $(LIBPLOTSRC)/libplot/plot.c.a $(V8CC_DEPS)
	@rm -rf $(BUILD)/libplot && mkdir -p $(BUILD)/libplot
	@cd $(BUILD)/libplot && ar x $(LIBPLOTSRC)/libplot/plot.c.a && \
	    for c in *.c; do $(V8CCRUN) -I$(LIBPLOTSRC)/libplot -c $$c || exit 1; done && \
	    rm -f libplot.a && ar rcs libplot.a *.o
	@echo "built $@"

$(BUILD)/lib4014/lib4014.a: $(LIBPLOTSRC)/lib4014/tek.c.a $(V8CC_DEPS)
	@rm -rf $(BUILD)/lib4014 && mkdir -p $(BUILD)/lib4014
	@cd $(BUILD)/lib4014 && ar x $(LIBPLOTSRC)/lib4014/tek.c.a && \
	    for c in *.c; do $(V8CCRUN) -I. -c $$c || exit 1; done && \
	    rm -f lib4014.a && ar rcs lib4014.a *.o
	@echo "built $@"

$(ROOTFS)/usr/lib/libplot.a: $(BUILD)/libplot/libplot.a
	@mkdir -p $(@D) && cp $< $@
$(ROOTFS)/usr/lib/lib4014.a: $(BUILD)/lib4014/lib4014.a
	@mkdir -p $(@D) && cp $< $@

# graph: upstream's link line is `cc -O graph.c -o graph -lplot -lm', and BOTH
# flags now resolve inside the jail and link nothing that graph needs -- libm.a
# is the 216-byte stub V8 shipped and libplot.a is the two-member general
# device.  That is not a defect in either: graph's plot calls are macros.
# Naming them on the link line is what keeps `libpath()' from letting -lplot
# escape to the host SDK, which is the whole reason src/cmd/cc.c resolves
# -lNAME against $V8ROOT/usr/lib first.
GRAPHSRC = $(SRC)/cmd/graph
$(BUILD)/graph/graph.o: $(GRAPHSRC)/graph.c $(V8CC_DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) -c -o $@ $<
$(BINDIR)/graph: $(BUILD)/graph/graph.o $(BUILD)/libplot/libplot.a $(V8DEPS)
	@mkdir -p $(@D)
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(BUILD)/graph/graph.o \
	    $(BUILD)/libplot/libplot.a $(V8LIBS)

# prof: upstream's CFLAGS carry -Dplot with a comment saying to take it out if
# you do not want plotting.  It is KEPT, because with the macro header there is
# nothing to pay for it -- prof.o built with -Dplot has no plot symbols either,
# so Bell Labs' own build description produces the same program ours does and
# prof is NOT a fifth rung-5 exclusion.  That was the open question in the note
# this block replaces.
PROFSRC = $(SRC)/cmd/prof
$(BUILD)/prof/prof.o: $(PROFSRC)/prof.c $(V8CC_DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) -Dplot -c -o $@ $<
$(BINDIR)/prof: $(BUILD)/prof/prof.o $(BUILD)/libplot/libplot.a $(V8DEPS)
	@mkdir -p $(@D)
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(BUILD)/prof/prof.o \
	    $(BUILD)/libplot/libplot.a $(V8LIBS)

# tek: plot(1)'s Tektronix renderer, and the ONE program here that genuinely
# needs a device library -- driver.c dispatches through a table of 28 real
# function pointers.  Upstream: `cc -s -o tek driver.o -l4014'.  hpplot is its
# sibling and is NOT built: `-l2621 -lcurses -ltermlib', and libcurses is 43
# unported files.
PLOTSRC = $(SRC)/cmd/plot
$(BUILD)/plot/driver.o: $(PLOTSRC)/driver.c $(V8CC_DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) -c -o $@ $<
$(BINDIR)/tek: $(BUILD)/plot/driver.o $(BUILD)/lib4014/lib4014.a $(V8DEPS)
	@mkdir -p $(@D)
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(BUILD)/plot/driver.o \
	    $(BUILD)/lib4014/lib4014.a $(V8LIBS)

# ---------------------------------------------------------------------------
# qed -- Thompson's editor, ed's ancestor.  Fourteen sources and vars.h, and
# upstream's makefile is one dependency line (`$(FILES): vars.h') plus a link.
#
# ITS TARGET IS `a.out', which is eqn's shape: `cc $(FILES)' with no -o, then
# `cp a.out /usr/bin/qed' in the install arm.  PLAN.md records eqn being
# recorded as rung-5 blocked "on a link failure" when the real cause was asking
# make for a target its makefile never names.  Nothing here depends on that --
# our rule names the binary directly -- but it is why the destination sweep in
# tests/wavea sees `a.out' rather than `qed' for this directory.
QEDSRC   = $(SRC)/cmd/qed
QED_NAME = address blkio com getchar getfile glob main misc move pattern \
           putchar setaddr string subs
QED_OBJ  = $(patsubst %,$(BUILD)/qed/%.o,$(QED_NAME))
$(BUILD)/qed/%.o: $(QEDSRC)/%.c $(V8CC_DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) -I$(QEDSRC) -c -o $@ $<
# Upstream's whole dependency statement, copied rather than widened.
$(QED_OBJ): $(QEDSRC)/vars.h
$(BINDIR)/qed: $(QED_OBJ) $(V8DEPS)
	@mkdir -p $(@D)
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(QED_OBJ) $(V8LIBS)

PLOT_NAMES   = graph prof tek qed
PLOT_BUILT   = $(patsubst %,$(BINDIR)/%,$(PLOT_NAMES))
PLOT_INSTALL = $(call v8install,$(PLOT_NAMES)) \
               $(ROOTFS)/usr/lib/libplot.a $(ROOTFS)/usr/lib/lib4014.a

# ---------------------------------------------------------------------------
# Wave A2 batch 2e -- struct(1), Brenda Baker's Fortran-to-Ratfor restructurer.
# 40 .c, a grammar, a lexer, 5695 lines, TWO programs and a shell script.
#
# THE SPELL/diff3 SHAPE, a third time: the command in /usr/bin is a SHELL
# SCRIPT and the binaries live in a DIRECTORY of their own.  Upstream's
# install is `mkdir /usr/lib/struct; cp structure beautify /usr/lib/struct;
# cp struct.sh /usr/bin/struct', and the shipped tree agrees -- it really has
# usr/lib/struct/{structure,beautify} and a 310-byte usr/bin/struct.  So
# $(call v8dest,...) is NOT used for the two binaries: struct appears in no
# Admin table at all, so dest would answer /usr/bin by fall-through, and
# /usr/lib/struct is the one thing upstream is explicit about.  The SCRIPT
# does go to /usr/bin, where dest's fall-through happens to be right.
#
# Three build idioms, all already in this file:
#   - beauty.c and y.tab.h are CHECKED IN, which is ccom's cgram.c idiom and
#     the reason the built-in %.c: %.y rule must stay cancelled.
#   - lextab.o comes from a lex file with NO yacc file beside it -- pp's idiom
#     from batch 2d -- and links -ll, which is why libl.a is a prerequisite.
#     Upstream writes `-lln'; there is no libln.a anywhere in the archive,
#     measured, and nm -u on the four objects wants only yywrap, which libl.a
#     provides.
#   - the link line globs `0.*.o 1.*.o ...' while the FILES lists do not:
#     2.test.c and 3.test.c each carry their own main and must NOT be built.
#     Compile only what the lists name.  Same trap as tbl's t?.o.
STRUCTSRC = $(SRC)/cmd/struct
STRUCT_0 = 0.alloc 0.args 0.def 0.extr 0.graph 0.list 0.parts 0.string
STRUCT_1 = 1.finish 1.form 1.fort 1.hash 1.init 1.line 1.main 1.node 1.recog \
           1.tables
STRUCT_2 = 2.dfs 2.dom 2.head 2.inarc 2.main 2.tree
STRUCT_3 = 3.branch 3.flow 3.loop 3.main 3.reach 3.then
STRUCT_4 = 4.brace 4.form 4.main 4.out
STRUCT_NAMES = main $(STRUCT_0) $(STRUCT_1) $(STRUCT_2) $(STRUCT_3) $(STRUCT_4)
STRUCT_OBJ = $(patsubst %,$(BUILD)/struct/%.o,$(STRUCT_NAMES))

$(BUILD)/struct/%.o: $(STRUCTSRC)/%.c $(V8CC_DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) -I$(STRUCTSRC) -c -o $@ $<

# Upstream's own dependency lines, transcribed.  def.h reaches every object;
# the numbered headers reach their own group.  This is the edge tests/deps
# exists for -- def.h is where VERT lives, so a stale object here is a graph
# built at one cell width and read at another.
$(STRUCT_OBJ): $(STRUCTSRC)/def.h
$(patsubst %,$(BUILD)/struct/%.o,$(STRUCT_1)): $(STRUCTSRC)/1.defs.h $(STRUCTSRC)/1.incl.h
$(patsubst %,$(BUILD)/struct/%.o,$(STRUCT_2)): $(STRUCTSRC)/2.def.h
$(patsubst %,$(BUILD)/struct/%.o,$(STRUCT_3)): $(STRUCTSRC)/3.def.h
$(patsubst %,$(BUILD)/struct/%.o,$(STRUCT_4)): $(STRUCTSRC)/4.def.h

$(BINDIR)/structure: $(STRUCT_OBJ) $(V8DEPS)
	@mkdir -p $(@D)
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(STRUCT_OBJ) $(V8LIBS)

# beautify.  y.tab.h is checked in and both lextab.o and tree.o include it,
# which is upstream's `lextab.o tree.o: y.tab.h' -- kept as a real edge rather
# than a comment, because a stale y.tab.h is a token-number skew and those are
# silent.
$(BUILD)/struct/lex.yy.c: $(STRUCTSRC)/lextab.l $(LEX) $(ROOTFS_NCFORM)
	@mkdir -p $(@D)
	cd $(BUILD)/struct && V8ROOT=$(ROOTFS) $(LEX) $(STRUCTSRC)/lextab.l
$(BUILD)/struct/lextab.o: $(BUILD)/struct/lex.yy.c $(STRUCTSRC)/b.h \
                          $(STRUCTSRC)/y.tab.h $(V8CC_DEPS)
	@$(V8CCRUN) -I$(BUILD)/struct -I$(STRUCTSRC) -c -o $@ $(BUILD)/struct/lex.yy.c
$(BUILD)/struct/tree.o: $(STRUCTSRC)/tree.c $(STRUCTSRC)/b.h \
                        $(STRUCTSRC)/y.tab.h $(V8CC_DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) -I$(STRUCTSRC) -c -o $@ $<
# -DYYSTYPE=long, and it is upstream's OWN hook rather than a patch.  beauty.c
# is the CHECKED-IN parser, generated by 1985's yacc, so it carries that yacc's
# default -- `#ifndef YYSTYPE / #define YYSTYPE int' at beauty.c:37 -- and the
# whole value stack `YYSTYPE yyv[YYMAXDEPTH]' is therefore int.  Every $1 in
# this grammar is a string pointer (`putout(xxident,$1)', and beauty.c:226
# free()s it), so every one was a heap address with its top half gone.
#
# This port's yacc emits `#define YYSTYPE long' (y2.c:318) for grammars it
# generates; beauty.c predates that and cannot know it.  The #ifndef is the
# escape hatch upstream shipped for exactly this, so no source is edited and
# the checked-in parser stays byte-identical.  lextab.l's `extern long yylval'
# must agree with this and does.
$(BUILD)/struct/beauty.o: $(STRUCTSRC)/beauty.c $(STRUCTSRC)/b.h $(V8CC_DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) -DYYSTYPE=long -I$(STRUCTSRC) -c -o $@ $<
$(BUILD)/struct/bdef.o: $(STRUCTSRC)/bdef.c $(STRUCTSRC)/b.h $(V8CC_DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) -I$(STRUCTSRC) -c -o $@ $<
BEAUTIFY_OBJ = $(BUILD)/struct/beauty.o $(BUILD)/struct/tree.o \
               $(BUILD)/struct/lextab.o $(BUILD)/struct/bdef.o
$(BINDIR)/beautify: $(BEAUTIFY_OBJ) $(BUILD)/libl/libl.a $(V8DEPS)
	@mkdir -p $(@D)
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(BEAUTIFY_OBJ) \
	    $(BUILD)/libl/libl.a $(V8LIBS)

$(ROOTFS)/usr/lib/struct/structure: $(BINDIR)/structure
	@mkdir -p $(@D) && cp $< $@
$(ROOTFS)/usr/lib/struct/beautify: $(BINDIR)/beautify
	@mkdir -p $(@D) && cp $< $@
$(ROOTFS)/usr/bin/struct: $(STRUCTSRC)/struct.sh
	@mkdir -p $(@D) && cp $< $@ && chmod +x $@
STRUCT_BUILT   = $(BINDIR)/structure $(BINDIR)/beautify
STRUCT_INSTALL = $(ROOTFS)/usr/lib/struct/structure \
                 $(ROOTFS)/usr/lib/struct/beautify $(ROOTFS)/usr/bin/struct

# ---------------------------------------------------------------------------
# csh(1) -- Bill Joy's C shell, 4.1BSD, and libjobs under it.  /bin per
# Admin/binfiles:10, confirmed by the shipped tree.
#
# libjobs is the FOURTH library import and the smallest: seven upstream files,
# and only sigset.c is built.  Four of the other six are VAX .s whose contents
# this port already has -- killpg, setpgrp and wait3 in shim/v8sys, getwd.c in
# src/libc/gen -- and the fifth, signal.s, supplies sigsys and sigpeel, which
# are now v8s_ entry points because they are kernel interfaces rather than
# library code.  src/cmd/csh/PORTING.md has the whole account.
LIBJOBSSRC = $(SRC)/lib/libjobs
$(BUILD)/libjobs/sigset.o: $(LIBJOBSSRC)/sigset.c $(V8CC_DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) -c -o $@ $<
$(BUILD)/libjobs/libjobs.a: $(BUILD)/libjobs/sigset.o
	@rm -f $@ && ar rcs $@ $(BUILD)/libjobs/sigset.o
$(ROOTFS)/usr/lib/libjobs.a: $(BUILD)/libjobs/libjobs.a
	@mkdir -p $(@D) && cp $< $@

# UPSTREAM'S OWN CFLAGS, less nothing: -DTELL -DVMUNIX -Ddebug -DVFORK.
# VMUNIX is the one that matters -- it selects the wait3-with-vtimes arm of
# pchild, which is what found the struct-at-the-seam bug in v8s_wait3.
#
# THE OBJECT LIST IS NOT upstream's OBJS.  Three of its members cannot be
# built here and each is a different reason:
#   strings.o  produced by xstr, whose point is `cc -c -R' putting the shared
#              string table in read-only TEXT.  -R is the arm64 shared-text
#              stop that already blocks rung 5 for cpp and sh.
#   doprnt.o   IS NOT C.  It is VAX assembly wrapped in cpp macros (.globl
#              __doprnt, #define flags r10), which is why upstream's rule for
#              it is `cc -E | as' rather than a compile.
#   printf.o   two lines calling that _doprnt.  With it gone, libv8c's printf
#              is what csh gets -- and libv8c's is this port's fixed one.
CSHSRC = $(SRC)/cmd/csh
CSHFLAGS = -DTELL -DVMUNIX -Ddebug -DVFORK
CSH_NAMES = sh sh.dol sh.err sh.exec sh.exp sh.func sh.glob sh.hist sh.lex \
            sh.misc sh.parse sh.print sh.sem sh.set sh.proc sh.dir sh.time \
            alloc sh.init
CSH_OBJ = $(patsubst %,$(BUILD)/csh/%.o,$(CSH_NAMES))

$(BUILD)/csh/%.o: $(CSHSRC)/%.c $(V8CC_DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) $(CSHFLAGS) -I$(CSHSRC) -c -o $@ $<

# Upstream's `csh: ${OBJS} sh.local.h' plus the two headers its own makefile
# does not name.  sh.h reaches every object and sh.dir.h/sh.proc.h reach the
# two that include them; a stale sh.h here is a struct-layout skew across
# nineteen objects.
$(CSH_OBJ): $(CSHSRC)/sh.h $(CSHSRC)/sh.local.h
$(BUILD)/csh/sh.dir.o $(BUILD)/csh/sh.func.o: $(CSHSRC)/sh.dir.h
$(BUILD)/csh/sh.proc.o $(BUILD)/csh/sh.sem.o: $(CSHSRC)/sh.proc.h

$(BINDIR)/csh: $(CSH_OBJ) $(BUILD)/libjobs/libjobs.a $(V8DEPS)
	@mkdir -p $(@D)
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(CSH_OBJ) \
	    $(BUILD)/libjobs/libjobs.a $(V8LIBS)
# INSTALLED.  It stood built-but-not-installed for one commit because it ran
# the whole language and then hung on every external command; that was a
# 16-bit p_pid in sh.proc.h, not a signal bug, and the hang is gone.
CSH_BUILT   = $(BINDIR)/csh
CSH_INSTALL = $(call v8install,csh) $(ROOTFS)/usr/lib/libjobs.a

# ---------------------------------------------------------------------------
# Wave A2 batch 3 -- the language systems, smallest first.
#
# ratfor is Kernighan's Rational Fortran preprocessor: it reads C-like control
# flow and writes Fortran 66.  It is the SMALLEST of the five by a wide margin
# -- 1263 lines against lcomp 1925, efl 10089, f77 16140 (plus 5165 more in
# libF77 and libI77) and cfront 22442 -- and it is the one with a consumer
# already installed, because struct(1) is its inverse: struct reads Fortran and
# writes Ratfor.  So the pair round-trips, which is an end-to-end assertion
# needing no f77.
#
# THE GRAMMAR IS r.g, NOT r.y, AND THAT IS NOT COSMETIC.  This port's own
# documented sweep for the yylval-width class globs src/cmd/*/*.y and
# src/cmd/*/*.l; ratfor's grammar matches neither, and its lexer is rlex.c
# rather than a .l file.  Both live instances were in r.h -- a HEADER, which
# that sweep does not glob either.  See src/cmd/ratfor/PORTING.md.
#
# y.tab.h comes out of the -d invocation and is deliberately not a target of
# its own, for the reason the awk block gives: GNU make 3.81 has no grouped
# targets, so naming both would be two rules sharing a recipe and would race
# under -j.  Everything needing the header depends on y.tab.c instead -- and
# here that is EVERY object, because r.h includes "y.tab.h" and all six .c
# files include r.h.  Upstream states the same edge six times over, one line
# per object: `r0.o: r.h y.tab.h r0.c'.
RATSRC = $(SRC)/cmd/ratfor
RAT_NAMES = r0 r1 r2 rio rlex rlook
RAT_OBJ = $(patsubst %,$(BUILD)/ratfor/%.o,$(RAT_NAMES)) $(BUILD)/ratfor/y.tab.o

$(BUILD)/ratfor/y.tab.c: $(RATSRC)/r.g $(YACC) $(ROOTFS_YACCPAR)
	@mkdir -p $(BUILD)/ratfor
	cd $(BUILD)/ratfor && V8ROOT=$(ROOTFS) $(YACC) -d $(RATSRC)/r.g

# Explicit, so it beats the %.o pattern below -- which would otherwise take the
# stem `y.tab' and go looking for $(RATSRC)/y.tab.c.  The name is upstream's:
# its own link line is `cc r*.o y.tab.o'.
$(BUILD)/ratfor/y.tab.o: $(BUILD)/ratfor/y.tab.c $(V8CC_DEPS)
	@$(V8CCRUN) -I$(BUILD)/ratfor -I$(RATSRC) -c -o $@ $(BUILD)/ratfor/y.tab.c

$(BUILD)/ratfor/%.o: $(RATSRC)/%.c $(BUILD)/ratfor/y.tab.c $(V8CC_DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) -I$(BUILD)/ratfor -I$(RATSRC) -c -o $@ $<

$(patsubst %,$(BUILD)/ratfor/%.o,$(RAT_NAMES)): $(RATSRC)/r.h

$(BINDIR)/ratfor: $(RAT_OBJ) $(V8DEPS)
	@mkdir -p $(@D)
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(RAT_OBJ) $(V8LIBS)

RAT_BUILT   = $(BINDIR)/ratfor
RAT_INSTALL = $(call v8install,ratfor)

# efl is Feldman's Extended Fortran Language (1977): structured control flow,
# real data structures and generic procedures, compiling down to Fortran 66.
# 12k lines over 25 objects -- the largest program in this port after cfront --
# and the only build here with THREE generated inputs made by three different
# V8 programs:
#
#   tokdefs   grep -n . tokens | sed ...   token numbers ARE line numbers in
#                                          `tokens', so an inserted line
#                                          renumbers everything below it
#   lex.c     lex lex.l, then `fixuplex'   which is an ed(1) SCRIPT that
#                                          patches the generated scanner
#   gram.c    NOT GENERATED.  Checked in, and upstream's rule for it is
#             commented out with a reason: "gram.c can no longer be made on a
#             pdp11 because of yacc limits".
#
# That last one is why $(EFL_NAMES) has no yacc step and why tests/wavea
# carries a case nothing else here needs.  gram.c:1-83 spells out the same 83
# token numbers the tokdefs rule derives, and with the regeneration gone there
# is no step left that would notice them disagreeing -- two hand-maintained
# copies of one list, which is the shape that let v8fsd's and p9cl's errno
# tables agree perfectly about a set missing seven names.  The lexer would
# simply start returning numbers the parser reads as different tokens.
#
# The tools are V8's own -- $(BINDIR)/{grep,sed,ed} beside $(LEX) -- for the
# reason maketab is built by V8's cc and run: upstream's makefile invokes them,
# so using the host's would leave nothing to say the step was V8's.  fixuplex
# reaches ed through PATH, so PATH is set to $(BINDIR) ALONE rather than
# prepended: the Mac's ed would run this script perfectly well and silently.
# The SHELL running fixuplex is deliberately the host's, spelled /bin/sh
# because PATH no longer finds one: V8's sh is not a $(BINDIR) binary (it has
# its own :fix pass), and every other recipe in this Makefile runs under GNU
# make's /bin/sh anyway.  Rung 5 is where V8's sh runs recipes; the tool
# upstream's makefile NAMES at this step is ed.
#
# A FAILED fixuplex IS A SILENT NO-OP THAT REPORTS SUCCESS, which is the
# opposite of what it looks like and is why the && chain below cannot be the
# guard.  Measured, not assumed: V7 ed reading from a here-document ABORTS the
# whole script on the first error -- the append never runs, `w' never runs, the
# file is untouched -- and it still EXITS 0.  So running fixuplex twice is
# harmless (measured: 1482 lines both times, one pushlex block), and a fixuplex
# whose substitution stopped matching would hand the build an unpatched scanner
# with nothing in the exit status to say so.  tests/wavea checks the CONTENT --
# that the input() macro reaches efgetc and that getc(yyin) survives nowhere --
# because that is the only observable there is.
#
# lex and fixuplex are still one recipe, which is how upstream wrote it, and the
# `mv' still comes last behind && so a failed LEX leaves no target.
EFLSRC = $(SRC)/cmd/efl
EFL_NAMES = main init tailor gram symtab dcl addr struct field blklab mk \
            misc alloc print simple exec temp io error pass2 icfile free \
            dclgen namgen
EFL_OBJ = $(patsubst %,$(BUILD)/efl/%.o,$(EFL_NAMES)) $(BUILD)/efl/lex.o

# test -s because a pipeline's exit status is its LAST element: a grep that
# failed would leave sed reporting success over an empty file, and every token
# would then be undefined a hundred lines further on in someone else's object.
$(BUILD)/efl/tokdefs: $(EFLSRC)/tokens $(BINDIR)/grep $(BINDIR)/sed
	@mkdir -p $(BUILD)/efl
	V8ROOT=$(ROOTFS) $(BINDIR)/grep -n . $(EFLSRC)/tokens | \
	  V8ROOT=$(ROOTFS) $(BINDIR)/sed "s/\([^:]*\):\(.*\)/#define \2 \1/" >$@.tmp
	@test -s $@.tmp && mv $@.tmp $@

$(BUILD)/efl/lex.c: $(EFLSRC)/lex.l $(EFLSRC)/fixuplex $(LEX) $(ROOTFS_NCFORM) \
                    $(BINDIR)/ed
	@mkdir -p $(BUILD)/efl
	cd $(BUILD)/efl && V8ROOT=$(ROOTFS) $(LEX) $(EFLSRC)/lex.l && \
	  V8ROOT=$(ROOTFS) PATH=$(BINDIR) /bin/sh $(EFLSRC)/fixuplex && \
	  mv lex.yy.c lex.c

# Explicit, so it beats the %.o pattern below -- which would take the stem
# `lex' and go looking for $(EFLSRC)/lex.c, a file that does not exist and
# never will: the scanner lives in lex.l.
$(BUILD)/efl/lex.o: $(BUILD)/efl/lex.c $(V8CC_DEPS)
	@$(V8CCRUN) -I$(BUILD)/efl -I$(EFLSRC) -c -o $@ $(BUILD)/efl/lex.c

$(BUILD)/efl/%.o: $(EFLSRC)/%.c $(V8CC_DEPS)
	@mkdir -p $(@D)
	@$(V8CCRUN) -I$(BUILD)/efl -I$(EFLSRC) -c -o $@ $<

# Upstream states both of these, one line each: `$(OBJECTS) : defs' and
# `lex.o init.o : tokdefs'.  defs is an #include'd non-header -- invisible to
# every dependency scanner AND to a *.c glob, which is the edge whose absence
# caused this port's worst bug.
$(EFL_OBJ): $(EFLSRC)/defs
$(BUILD)/efl/lex.o $(BUILD)/efl/init.o: $(BUILD)/efl/tokdefs

$(BINDIR)/efl: $(EFL_OBJ) $(V8DEPS)
	@mkdir -p $(@D)
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(EFL_OBJ) $(V8LIBS)

EFL_BUILT   = $(BINDIR)/efl
EFL_INSTALL = $(call v8install,efl)

# B2D_NAMES is the set whose destination $(call v8dest,...) derives, so it is
# also what the $(ROOTFS)/etc/% generator further down has to iterate over --
# showq and dmesg land in /etc and the generator's own comment promises "the
# next section-8 command imported needs no new line".  That promise is only
# kept if a new list of programs is added to the foreach, so it is, and this
# variable is defined here because make expands a variable in a rule when it
# READS the rule and a definition below first use silently expands to nothing.
B2D_NAMES   = hoc p pp newgrp showq dmesg
B2D_BUILT   = $(patsubst %,$(BINDIR)/%,$(B2D_NAMES) $(CAL_BIN))
B2D_INSTALL = $(call v8install,$(B2D_NAMES)) $(CAL_INSTALL) \
              $(ROOTFS)/usr/lib/libl.a

V8DIRBIN_BUILT   = $(patsubst %,$(BINDIR)/%,$(V8DIRBIN)) $(BINDIR)/ul $(BINDIR)/ex \
                   $(BINDIR)/awk $(BINDIR)/expr $(BINDIR)/m4 $(BINDIR)/diff3 \
                   $(BINDIR)/pack $(BINDIR)/unpack $(B2D_BUILT) $(PLOT_BUILT) \
                   $(STRUCT_BUILT) $(CSH_BUILT) $(RAT_BUILT) $(EFL_BUILT)
V8DIRBIN_INSTALL = $(call v8install,$(V8DIRBIN)) $(DIFF_INSTALL) $(UL_INSTALL) \
                   $(EX_INSTALL) $(call v8install,awk expr m4) \
                   $(DIFF3_INSTALL) $(PACK_INSTALL) $(B2D_INSTALL) \
                   $(PLOT_INSTALL) $(STRUCT_INSTALL) $(CSH_INSTALL) \
                   $(RAT_INSTALL) $(EFL_INSTALL) $(LINKED_INSTALL)

# Same reason as $(V8BIN_BUILT) above: without this make treats each of these
# as an intermediate of the $(ROOTFS)/bin/% copy and deletes it, leaving a
# working rootfs and no build tree to compare it against.
.SECONDARY: $(V8DIRBIN_BUILT)

v8bin: $(V8BIN_BUILT) $(V8BIN_INSTALL) $(V8LIBDATA) \
       $(V8DIRBIN_BUILT) $(V8DIRBIN_INSTALL) \
       $(BINDIR)/df $(BINDIR)/load $(BINDIR)/w $(BINDIR)/ps \
       $(call v8install,df load w ps) $(ROOTFS)/usr/bin/uptime \
       $(SCRIPT_INSTALL) \
       imgbin

# Without this make DELETES every binary in $(BINDIR) as soon as it has copied
# it to the rootfs.  Two pattern rules chain here -- cat.c -> build/bin/cat ->
# rootfs/bin/cat -- and a file made by a pattern rule only to satisfy another
# pattern rule is an INTERMEDIATE, which make removes when the chain finishes.
#
# It is invisible from outside: the rootfs copy exists and works, so every
# functional test passes.  What is lost is the build tree -- 38 binaries gone,
# relinked from scratch on the next build, and nothing to compare a rootfs copy
# against.  tests/deps caught it precisely because it asserts on the
# intermediate rather than on the observable behaviour.
.SECONDARY: $(V8BIN_BUILT)

$(BINDIR)/%: $(SRC)/cmd/%.c $(V8CC_DEPS) $(V8DEPS)
	@mkdir -p $(BINDIR)
	@$(V8CCRUN) -c -o $(BINDIR)/$*.o $<
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(BINDIR)/$*.o $(V8LIBS)

# who -- a groveler, so it links libkmemu on top of the standard list.  An
# EXPLICIT rule for a target the pattern rule above would otherwise match; GNU
# make prefers the explicit one, and who stays in $(V8BIN) so that it is built,
# installed and counted by the tests/wavea check that every src/cmd/*.c but cc
# is a real installed binary.
#
# The compile is identical -- who.c is authentic V8 and needed no change at all.
# Everything Phase 4 costs it is on the LINK line, which is the argument for
# having the shim manufacture /etc/utmp rather than patching who to ask for it.
$(BINDIR)/who: $(SRC)/cmd/who.c $(V8CC_DEPS) $(V8DEPS) $(KMEMU_LIB)
	@mkdir -p $(BINDIR)
	@$(V8CCRUN) -c -o $(BINDIR)/who.o $<
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(BINDIR)/who.o \
	    $(KMEMU_LDADD) $(V8LIBS)

# df, the second groveler.  It lives in a directory of its own upstream, so it
# is not in $(V8BIN) and does not use the single-file pattern rule; odf.C in the
# same directory is the older version and is not built, exactly as upstream's
# own makefile leaves it.
$(BINDIR)/df: $(SRC)/cmd/df/df.c $(V8CC_DEPS) $(V8DEPS) $(KMEMU_LIB)
	@mkdir -p $(BINDIR)
	@$(V8CCRUN) -c -o $(BINDIR)/df.o $<
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(BINDIR)/df.o \
	    $(KMEMU_LDADD) $(V8LIBS)

# load, the third groveler and the first to need a namelist: it looks _avenrun
# up in /unix and then seeks to that address in /dev/kmem.  libkmemu writes both
# from one table -- see shim/libkmemu/kmem.c.
$(BINDIR)/load: $(SRC)/cmd/load/load.c $(V8CC_DEPS) $(V8DEPS) $(KMEMU_LIB)
	@mkdir -p $(BINDIR)
	@$(V8CCRUN) -c -o $(BINDIR)/load.o $<
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(BINDIR)/load.o \
	    $(KMEMU_LDADD) $(V8LIBS)

# w, the fourth groveler, and the one that is TWO programs: upstream's makefile
# links /usr/bin/uptime to it and w branches on argv[0].  Only the uptime half
# runs here -- see src/cmd/w/PORTING.md -- and it is the same binary either way,
# so the link is what makes that fact true rather than merely described.
$(BINDIR)/w: $(SRC)/cmd/w/w.c $(V8CC_DEPS) $(V8DEPS) $(KMEMU_LIB)
	@mkdir -p $(BINDIR)
	@$(V8CCRUN) -c -o $(BINDIR)/w.o $<
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(BINDIR)/w.o \
	    $(KMEMU_LDADD) $(V8LIBS)

# `ln', not `cp', because upstream says ln and because a copy would let the two
# drift.  An EXPLICIT rule for a target the $(ROOTFS)/bin/% pattern would
# otherwise match, so that the recipe can delete uptime.
#
# THAT DELETION IS THE WHOLE POINT, and it is not tidiness.  Deleting
# rootfs/usr/bin/w breaks the link, so the next build must remake it -- but make
# decides that by comparing mtimes, and macOS make compares them at WHOLE-SECOND
# granularity.  The cp below and the ln beneath it land in the same second, so
# uptime is never *older* than w, so it is never remade, and the two names stay
# on different inodes with nothing to say so.  Measured: tests/deps caught it on
# the first run, 144966450 against 144966446.
#
# Removing uptime here replaces the mtime question with an existence question,
# which has no granularity.  cp(1) writes through a live hard link, so in the
# ordinary case uptime was already correct and this just relinks it; in the
# broken case it is the repair.
$(ROOTFS)/usr/bin/w: $(BINDIR)/w
	@mkdir -p $(@D) && cp $< $@ && rm -f $(@D)/uptime

$(ROOTFS)/usr/bin/uptime: $(ROOTFS)/usr/bin/w
	@mkdir -p $(@D) && rm -f $@ && ln $< $@


# ---------------------------------------------------------------------------
# ps -- and it is not a groveler.  Every other program in this section reads a
# FILE this port manufactures; ps reads a FILESYSTEM this port implements.  It
# getdirs /proc, opens /proc/<pid>, asks PIOCGETPR for a struct proc and reads a
# struct user from the file at virtual address UBASE.  That is Killian's process
# filesystem, V8's own, and PLAN.md section 8a step 3 is where it came from.
#
# So the dependency is $(ROOTFS_DEV) as well as $(KMEMU_LIB): ps calls error()
# and exits before doing anything at all if /dev/dk, /dev/pt or /dev/drum is
# missing (ps.c:21-28).  Naming them here rather than under `rootfs' is what
# makes `make ps' produce something that runs.
#
# The object list is upstream's OFILES, unchanged.
# ---------------------------------------------------------------------------
PSSRC = $(SRC)/cmd/ps
PS_OBJ_NAMES = ps doselect printp fdprint getargs gettty getuname getfs \
               sort nalloc
PS_OBJ = $(patsubst %,$(BUILD)/ps/%.o,$(PS_OBJ_NAMES))

$(BUILD)/ps/%.o: $(PSSRC)/%.c $(V8CC_DEPS)
	@mkdir -p $(BUILD)/ps
	@$(V8CCRUN) -I$(PSSRC) -c -o $@ $<

# Upstream's `$(OBJ): ps.h', which is the whole of its dependency information.
$(PS_OBJ): $(PSSRC)/ps.h

$(BINDIR)/ps: $(PS_OBJ) $(V8DEPS) $(KMEMU_LIB) $(ROOTFS_DEV)
	@mkdir -p $(BINDIR)
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(PS_OBJ) \
	    $(KMEMU_LDADD) $(V8LIBS)
	@echo "built $@"

# ---------------------------------------------------------------------------
# THE IMAGE TOOLS -- mkfs, icheck, dcheck.  PLAN.md section 8a step 4.
#
# They are a group rather than three programs that happen to sit together, and
# the -D is what makes them one: each of them reads or writes a FILESYSTEM
# IMAGE, so each has to agree with 1985 rather than merely be correct on this
# machine, and DIRSIZ is where those two part company.
#
# This port raises DIRSIZ 14 -> 254 in three headers because macOS filenames
# exceed fourteen characters.  That is right for every directory the shim
# serves off the host and wrong for every directory inside an image.  The
# failure would not have looked like one: a V8 kernel reading a 256-byte record
# finds `.', then fifteen slots whose d_ino is 0 -- V7's own encoding for a
# deleted entry, the accident src/include/sys/dir.h already documents -- so it
# reads the directory correctly and then ALLOCATES into them, and the result
# parses on neither side.  Measured: a three-entry root has i_size 768 at 254
# and 48 at 14.
#
# A -D is exactly the kind of thing that gets forgotten, so tests/mkfs asserts
# it from the far end -- on the bytes of a generated image, where `.' is at
# offset 0 and `..' at 16 -- rather than on this command line, which would only
# be reading the rule back to itself.
#
# WHY A GROUP AND NOT A FLAG ON MKFS.  When mkfs was alone the flag looked like
# a property of that program.  dcheck reads the directories mkfs writes and
# needs exactly the same 14, so it is a property of TALKING TO AN IMAGE.  Naming
# the set is what stops the next one being compiled without it -- which would
# not fail to build and would not fail to run.
#
# WHAT IT WOULD DO WAS STATED BACKWARDS HERE, and the measurement is in
# tests/mkfs section 8.  This said "report a healthy filesystem as corrupt",
# which is what happens when the CHECKER has the wrong DIRSIZ.  The direction
# that matters is the other one, because it is the direction upstream's own
# build description produces: Admin/Mk compiles a bare cmd/*.c with
# `cc $CFLAGS -o $B $B.c' and no -D at all, correct on a machine whose param.h
# says 14.  Built that way here, mkfs writes a root with i_size 512 and `..' at
# offset 256 -- and icheck, dcheck AND fsck all pronounce it clean, because the
# 240 bytes between `.' and `..' are zero and a zero d_ino is V7's own encoding
# for a deleted entry.  A WRONG WRITER IS INVISIBLE TO EVERY READER WE HAVE.
# Only the byte-level cases see it, which is why the format is asserted on the
# bytes of a generated image and never on this line.
#
# AND WITH FSCK THE COST OF FORGETTING IT CHANGED KIND.  For the four readers a
# wrong DIRSIZ is a wrong answer.  fsck's pass2() copies DIRSIZ bytes per path
# component into pathname[200] with no bound, so at the host's 254 a SINGLE
# component overruns it by 54 bytes -- in the one program here that writes to
# filesystems.  The flag is now a memory-safety property as well as a format
# one; src/cmd/fsck.PORTING.md has it.
#
# They install in /etc rather than /bin because all five are section 8 and V8
# keeps section 8 there; each man page's synopsis says /etc/<name>, and
# Admin/etcfiles lists every one.  Being explicit targets is also what keeps
# them clear of the $(ROOTFS)/etc/% rule further down, which copies data files
# out of src/etc and has no source for a binary.
# ---------------------------------------------------------------------------
IMGDIRSIZ = 14
IMGBIN    = mkfs icheck dcheck clri fsck ncheck quot restor dumpdir dump

# Where each one's sources sit under $(SRC)/cmd, as a LIST -- eight are a bare
# cmd/<name>.c, quot is cmd/quot/quot.c because upstream gave it a makefile of
# its own, and dump is six objects in a directory.  Spelled out for all ten
# rather than defaulted for the eight, for $(dirsrcs)' reason: the short form
# $(if $(imgsrc_$(1)),...) READS an undefined variable, and this Makefile is
# checked with --warn-undefined-variables precisely because a variable used
# above its definition expands to nothing in a prerequisite.
imgsrc_mkfs    = mkfs
imgsrc_icheck  = icheck
imgsrc_dcheck  = dcheck
imgsrc_clri    = clri
imgsrc_fsck    = fsck
imgsrc_ncheck  = ncheck
imgsrc_quot    = quot/quot
imgsrc_restor  = restor
imgsrc_dumpdir = dumpdir
imgsrc_dump    = dump/dumpmain dump/dumptraverse dump/dumptape \
                 dump/dumpoptr dump/dumpitime dump/unctime
imgsrcs        = $(imgsrc_$(1))

# The include path each one needs, for the same "define above first use"
# reason.  Only dump has a private header ("dump.h"); the rest reach everything
# through $(ROOTFS_INC).
imginc_mkfs    =
imginc_icheck  =
imginc_dcheck  =
imginc_clri    =
imginc_fsck    =
imginc_ncheck  =
imginc_quot    =
imginc_restor  =
imginc_dumpdir =
imginc_dump    = -I$(SRC)/cmd/dump
imgincs        = $(imginc_$(1))

# ...and the private headers as PREREQUISITES, which is a different statement
# from -I and the one that was missing.  -I tells cpp where to look; this tells
# make to recompile.  All six dump objects include "dump.h", and without this
# editing it rebuilt nothing -- the failure this whole suite exists for, and
# tests/deps caught it on the first run.
imgdep_mkfs    =
imgdep_icheck  =
imgdep_dcheck  =
imgdep_clri    =
imgdep_fsck    =
imgdep_ncheck  =
imgdep_quot    =
imgdep_restor  =
imgdep_dumpdir =
imgdep_dump    = $(SRC)/cmd/dump/dump.h
imgdeps        = $(imgdep_$(1))

# quot IS in the group and does NOT need the -D, which is the distinction worth
# keeping rather than smoothing over.  It reads inodes and never a directory --
# no <sys/dir.h>, no struct direct, no DIRSIZ -- so its object is BYTE-IDENTICAL
# with and without the flag; tests/mkfs asserts that rather than this sentence.
# It is here because the group is "programs that talk to a 1985 image", and
# membership is what stops the next import being compiled without the flag.
# The measured no-op has a payoff: quot is the one image tool whose own
# upstream makefile (CFLAGS = -O, no -D) therefore builds the SAME program, so
# it is the only one of the seven that is not blocked from rung 5.
#
# ncheck is the opposite and is the reason the flag is asserted twice now.
# Built at the host's 254 it prints NOTHING AT ALL and exits 0: NDIR(dev) comes
# out 4 instead of 64 and each step is 256 bytes rather than 16, so a root whose
# di_size is 48 is exhausted by its own `.' entry, which dotname() then filters.
# So it must never join $(V8BIN) -- that list is what $(SRCTREE) stages for
# Admin/Mk, and Mk compiles a bare cmd/*.c with `cc $CFLAGS -o $B $B.c' and no
# -D of any kind.  tests/jail builds it both ways and asserts the pair.

# Generated for the reason $(V8DIRBIN)'s rules are: a static pattern rule cannot
# put the stem in two places, and .SECONDEXPANSION is too large a blast radius.
# Objects go in $(BUILD)/<prog>/ and NOT $(BINDIR)/<prog>/, because the binary
# is $(BINDIR)/<prog> and a directory of that name makes the path a file and a
# directory at once -- ld says "Is a directory", which reads as a linker problem
# and is a layout one.
# Two rules per program now: one object rule per source (dump has six) and one
# link rule collecting them.  $(imgobjs) turns `dump/dumpmain dump/unctime'
# into $(BUILD)/dump/dumpmain.o $(BUILD)/dump/unctime.o -- notdir, because the
# objects all live under $(BUILD)/<prog>/ whatever subdirectory the source came
# from, exactly as $(V8DIRBIN)'s do.
imgobjs = $(patsubst %,$(BUILD)/$(1)/%.o,$(notdir $(call imgsrcs,$(1))))

define imgobj_rule
$(BUILD)/$(1)/$(notdir $(2)).o: $$(SRC)/cmd/$(2).c $(call imgdeps,$(1)) \
                                $$(V8CC_DEPS)
	@mkdir -p $$(@D)
	@$$(V8CCRUN) $(call imgincs,$(1)) -DDIRSIZ=$$(IMGDIRSIZ) -c -o $$@ $$<
endef

define imgprog_rule
$(BINDIR)/$(1): $(call imgobjs,$(1)) $$(V8DEPS)
	@mkdir -p $$(BINDIR)
	@$$(HOSTCC) $$(V8LDFLAGS) -o $$@ $$(BUILD)/crt0.o \
	    $(call imgobjs,$(1)) $$(V8LIBS)
	@echo "built $$@"

$(ROOTFS)/$$(call v8dest,$(1))/$(1): $(BINDIR)/$(1)
	@mkdir -p $$(@D) && cp $$< $$@
endef

$(foreach p,$(IMGBIN),\
  $(foreach s,$(call imgsrcs,$(p)),$(eval $(call imgobj_rule,$(p),$(s))))\
  $(eval $(call imgprog_rule,$(p))))

IMGBIN_BUILT   = $(patsubst %,$(BINDIR)/%,$(IMGBIN))
IMGBIN_INSTALL = $(call v8install,$(IMGBIN))

# Same reason as $(V8BIN_BUILT): without this make treats each as an
# intermediate of the install copy and deletes it, leaving a working rootfs and
# nothing in the build tree to compare it against.
.SECONDARY: $(IMGBIN_BUILT)

imgbin: $(IMGBIN_BUILT) $(IMGBIN_INSTALL)
mkfs: imgbin

# ---------------------------------------------------------------------------
# /usr/src/cmd -- V8's own source tree, staged inside the jail.
#
# WHY, and it is a rung-5 claim rather than decoration.  Rung 5 so far means
# "V8's make reads Bell Labs' makefile", demonstrated on seventeen programs.
# More than half of cmd/ HAS NO MAKEFILE: those are bare *.c files, and their
# build description is Admin/Mk, which for each one runs
#
#	eval D=`Admin/dest $B`; cc $CFLAGS -o $B $B.c && install $B $D/$B
#
# Mk opens with `cd /usr/src/cmd' -- the only absolute path in it -- so without
# a /usr/src inside the jail that description cannot run at all.  shim/v8sys/
# vfs.c gains the mount row; this stages the files it expects to find.
#
# Sources are OURS, not third_party's pristine copies, by the same rule every
# other rung-5 entry follows: the claim is that the build DESCRIPTION is Bell
# Labs', not that the source is unpatched.  Admin/ is upstream and imported with
# PROVENANCE, and $(ADMIN) above now reads the same copy, so the destination
# this Makefile derives at build time and the one Mk derives at run time cannot
# drift apart.
#
# Per-file targets rather than a directory stamp, for the reason tests/deps
# already records: a stamp answers "the install ran", which is not the question
# when someone deletes one file.
# ---------------------------------------------------------------------------
ADMINFILES = Mk dest lookline binfiles etcfiles libfiles ulibfiles large
SRCTREE    = $(ROOTFS)/usr/src/cmd

ROOTFS_ADMIN = $(patsubst %,$(SRCTREE)/Admin/%,$(ADMINFILES))
ROOTFS_CSRC  = $(patsubst %,$(SRCTREE)/%.c,$(V8BIN))

# dest and lookline are invoked as COMMANDS by Mk, not as `sh dest', so the
# execute bit has to survive the copy -- plain cp does that, and upstream has
# the three scripts 755 and the four tables 644 already, so anything stronger
# here would make the data files executable and say something untrue about
# them.  None of the three has a #! line; V8's sh runs them itself when execve
# answers ENOEXEC.
$(SRCTREE)/Admin/%: $(SRC)/cmd/Admin/%
	@mkdir -p $(@D) && cp $< $@

$(SRCTREE)/%.c: $(SRC)/cmd/%.c
	@mkdir -p $(@D) && cp $< $@

srctree: $(ROOTFS_ADMIN) $(ROOTFS_CSRC)

# Installed under the rootfs so the shim's rootpath() can resolve /bin/... to
# them -- this port's chroot, implemented where its kernel lives.
$(ROOTFS)/bin/%: $(BINDIR)/%
	@mkdir -p $(@D) && cp $< $@

# ...and the same for /usr/bin, which is where $(call v8dest,...) sends most of
# them.  Two pattern rules rather than one parameterised by directory, because
# a target either begins with $(ROOTFS)/bin/ or it does not and make picks
# without ambiguity.  The Wave C tools have EXPLICIT rules for their own
# $(ROOTFS)/usr/bin targets, sourced from $(BUILD)/<prog>/<prog> rather than
# from $(BINDIR), and an explicit rule beats a pattern -- so nroff, troff, tbl,
# eqn, pic, refer, grap, spell and man are untouched by this.
$(ROOTFS)/usr/bin/%: $(BINDIR)/%
	@mkdir -p $(@D) && cp $< $@

# ...and /etc, which $(call v8dest,...) picks for the section-8 commands --
# chgrp and chown are both in Admin/etcfiles, and Wave A2 is the first batch to
# contain any.
#
# A THIRD PATTERN RULE IS NOT THE WAY, and $(IMGBIN) already worked that out:
# $(ROOTFS)/etc/% exists further down for the genuine V8 data files, and two
# pattern rules matching one target leave make choosing by which prerequisite
# happens to exist -- which is a coincidence, not a decision.  The image tools
# spelled their five targets out one at a time to stay clear of it; this
# generates the same EXPLICIT rule for whichever $(V8BIN) members land there,
# so the next section-8 command imported needs no new line and cannot silently
# get the data-file recipe.
define v8bin_etc_rule
$(ROOTFS)/etc/$(1): $(BINDIR)/$(1)
	@mkdir -p $$(@D) && cp $$< $$@
endef
$(foreach p,$(V8BIN) $(V8DIRBIN) $(B2D_NAMES),\
  $(if $(filter etc,$(call v8dest,$(p))),$(eval $(call v8bin_etc_rule,$(p)))))

# macos(1) -- THIS PORT'S OWN, and it lives in shim/ rather than src/ for the
# reason the layer rule gives: src/ is authentic V8 source and V8 never had a
# way to reach a Mac.  It is installed into the world because that is where it
# is used, not because it is V8.
$(ROOTFS)/usr/bin/macos: $(ROOT)/shim/world/macos
	@mkdir -p $(@D) && cp $< $@ && chmod 755 $@

# sh is built from a directory, not a single file, so it gets its own install
# rule.  It is the one that matters most: it is what make execs.
$(ROOTFS)/bin/sh: $(BUILD)/sh/sh
	@mkdir -p $(@D) && cp $< $@

# yacc and lex, same shape -- and they are not optional decoration.  A program
# built from its OWN V8 makefile runs `yacc parser.y`, and until these were
# installed the jail refused it and said so:
#
#	v8sys: exec leaves the jail: /bin/yacc (refused: V8JAIL=strict)
#	Make: Cannot load yacc.  Stop.
#
# which is the guard working exactly as intended -- they were built, tested and
# used by this repo's own Makefile for months without ever being reachable from
# inside the world they belong to.
# `[` is test under its other name, and shell scripts use that name almost
# exclusively -- man.sh opens with `if [ -d $$MAN ]`, which without this simply
# fails and sends man to a cache directory that does not exist.  V8 shipped it
# as a link; a copy is the same thing here and avoids a link rule that would
# need a different recipe on a filesystem without hard links.
$(ROOTFS)/bin/[: $(BINDIR)/test
	@mkdir -p $(@D) && cp $< $@

# yacc and lex are /usr/bin commands, on both of upstream's sources -- the
# shipped tree has them there and Admin/binfiles does not list them.  They stay
# reachable from inside the jail because tests/jail exports PATH=/bin:/usr/bin
# and both halves resolve through rootpath(); V8's make execs /bin/sh, and it is
# sh that searches.  These two are named explicitly rather than going through
# $(call v8dest,...) because their source is $(YACC)/$(LEX) in the build tree
# rather than $(BINDIR)/<name>.
$(ROOTFS)/usr/bin/yacc: $(YACC)
	@mkdir -p $(@D) && cp $< $@
$(ROOTFS)/usr/bin/lex: $(LEX)
	@mkdir -p $(@D) && cp $< $@
$(ROOTFS)/bin/make: $(BUILD)/make/make
	@mkdir -p $(@D) && cp $< $@

# ---------------------------------------------------------------------------
# make -- the build driver itself, and the rung of the bootstrap that was
# skipped.  It was listed in Phase 1a and never landed, so this port has been
# built by the HOST's make throughout.
#
# It cannot be "the first program after the compiler", which is the obvious
# thing to assume: make has a 440-line gram.y, so it needs yacc.  The order is
# cc -> yacc -> make.  It needs no lex.
#
# Object list, flags and the `$(OBJECTS): defs` line are lifted from V8's own
# Makefile unchanged.  `defs` is make's shared header under a name that is not
# .h, so it is invisible to every dependency scanner -- the same shape as lex's
# once.c, tbl's t..c and refer's refer..c.
# ---------------------------------------------------------------------------
MAKESRC = $(SRC)/cmd/make
MAKE_NAMES = ident main doname dosys misc files
MAKE_OBJ = $(patsubst %,$(BUILD)/make/%.o,$(MAKE_NAMES)) $(BUILD)/make/gram.o
MAKEFLAGS_V8 = -DASCARCH -DVERSION8

v8make: $(BUILD)/make/make
$(BUILD)/make/make: $(MAKE_OBJ) $(V8DEPS)
	$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(MAKE_OBJ) $(V8LIBS)
	@echo "built $@"

$(BUILD)/make/gram.c: $(MAKESRC)/gram.y $(YACC) $(ROOTFS_YACCPAR)
	@mkdir -p $(BUILD)/make
	cd $(BUILD)/make && V8ROOT=$(ROOTFS) $(YACC) $(MAKESRC)/gram.y
	@mv -f $(BUILD)/make/y.tab.c $(BUILD)/make/gram.c

$(BUILD)/make/gram.o: $(BUILD)/make/gram.c $(V8CC_DEPS)
	$(V8CCRUN) $(MAKEFLAGS_V8) -I$(MAKESRC) -c -o $@ $(BUILD)/make/gram.c
$(BUILD)/make/%.o: $(MAKESRC)/%.c $(V8CC_DEPS)
	@mkdir -p $(BUILD)/make
	$(V8CCRUN) $(MAKEFLAGS_V8) -I$(MAKESRC) -c -o $@ $<
$(MAKE_OBJ): $(MAKESRC)/defs

# ---------------------------------------------------------------------------
# lex.  The tree arrays name/left/right/parent/nullstr are DECLARED in once.c
# and ALLOCATED in parser.y, using sizeof(*left) so the size follows the type.
# That split is why the dependencies below are spelled out rather than left to
# the pattern rule: once.c widening left[] to `long` changes sizeof(*left) from
# 4 to 8, and a y.tab.o built before that change allocates 1700*4 bytes for an
# array written as 8-byte longs -- a 2x overrun straight through the next
# block's malloc header.  See src/cmd/lex/PORTING.md.
#
# Every lex source #includes ldefs.c, and lmain.c also #includes once.c, so
# neither shows up as a compiler-visible dependency.  Both are listed here.
# ---------------------------------------------------------------------------
LEXSRC = $(SRC)/cmd/lex
LEX_NAMES = lmain sub1 sub2 header
LEX_OBJ = $(patsubst %,$(BUILD)/lex/%.o,$(LEX_NAMES)) $(BUILD)/lex/y.tab.o

v8lex: $(BUILD)/lex/lex
$(BUILD)/lex/lex: $(LEX_OBJ) $(BUILD)/crt0.o $(BUILD)/libc/libv8c.a \
                  $(BUILD)/v8sys/libv8stubs.a $(BUILD)/v8sys/libv8sys.a
	$(HOSTCC) -nostdlib -e _v8start -o $@ $(BUILD)/crt0.o $(LEX_OBJ) \
	    $(BUILD)/libc/libv8c.a $(BUILD)/v8sys/libv8stubs.a \
	    $(BUILD)/v8sys/libv8sys.a -lSystem
	@echo "built $@"

$(BUILD)/lex/y.tab.c: $(LEXSRC)/parser.y $(YACC) $(ROOTFS_YACCPAR)
	@mkdir -p $(BUILD)/lex
	cd $(BUILD)/lex && V8ROOT=$(ROOTFS) $(YACC) $(LEXSRC)/parser.y

$(BUILD)/lex/y.tab.o: $(BUILD)/lex/y.tab.c $(V8CC_DEPS)
	$(V8CCRUN) -I$(LEXSRC) -c -o $@ $(BUILD)/lex/y.tab.c
$(BUILD)/lex/%.o: $(LEXSRC)/%.c $(V8CC_DEPS)
	@mkdir -p $(BUILD)/lex
	$(V8CCRUN) -I$(LEXSRC) -c -o $@ $<

# The two files that are #included, not compiled.  Every object depends on
# ldefs.c; lmain.o and y.tab.o additionally depend on once.c, which is where
# the widths live.
$(LEX_OBJ): $(LEXSRC)/ldefs.c $(LEXSRC)/once.c

# ---------------------------------------------------------------------------
# pic -- the first program that needs BOTH of V8's own generators: yacc for
# picy.y and lex for picl.l.  The rest of the objects include pic.ydef, which is
# y.tab.h under another name; the original makefile copies it across only when
# it changes, to avoid rebuilding everything on every yacc run.  We depend on it
# directly and let make decide.
# ---------------------------------------------------------------------------
# V8DEPS, V8LIBS and V8LDFLAGS are defined at the top of this file.

PICSRC = $(SRC)/cmd/pic
PIC_NAMES = main print misc symtab blockgen boxgen circgen arcgen linegen \
            movegen textgen input for pltroff
PIC_OBJ = $(patsubst %,$(BUILD)/pic/%.o,$(PIC_NAMES)) \
          $(BUILD)/pic/picy.o $(BUILD)/pic/picl.o

pic: $(BUILD)/pic/pic
$(BUILD)/pic/pic: $(PIC_OBJ) $(BUILD)/crt0.o $(BUILD)/libc/libv8c.a \
                  $(BUILD)/v8sys/libv8stubs.a $(BUILD)/v8sys/libv8sys.a
	$(HOSTCC) -nostdlib -e _v8start -o $@ $(BUILD)/crt0.o $(PIC_OBJ) \
	    $(BUILD)/libc/libv8c.a $(BUILD)/v8sys/libv8stubs.a \
	    $(BUILD)/v8sys/libv8sys.a -lSystem
	@echo "built $@"

# ONE target per rule.  This was written as `y.tab.c pic.ydef: ...`, which make
# 3.81 -- the make macOS ships, and the one this builds under -- reads as two
# separate rules that happen to share a recipe, so the recipe is eligible to run
# twice, and does race under -j.  Grouped targets (&:) need make 4.3.
$(BUILD)/pic/y.tab.c: $(PICSRC)/picy.y $(YACC) $(ROOTFS_YACCPAR)
	@mkdir -p $(BUILD)/pic
	cd $(BUILD)/pic && V8ROOT=$(ROOTFS) $(YACC) -d $(PICSRC)/picy.y

# pic.ydef is y.tab.h under another name.  The original makefile copied it only
# when it CHANGED, to avoid rebuilding the other 14 objects after a grammar edit
# that left the token numbers alone.  That trick is NOT reproduced here, and the
# reason is worth recording: a conditional copy leaves pic.ydef permanently
# older than y.tab.c, so make reports "must remake" for it on every run forever.
# Harmless in itself -- it re-runs one cmp -- but it makes `make -n` claim the
# dependent objects rebuild when a real `make` correctly leaves them alone.
# A build whose dry run lies is worse than one that spends two seconds
# recompiling, and this whole exercise is about the build being believable.
$(BUILD)/pic/pic.ydef: $(BUILD)/pic/y.tab.c
	@cp $(BUILD)/pic/y.tab.h $@

$(BUILD)/pic/lex.yy.c: $(PICSRC)/picl.l $(LEX) $(ROOTFS_NCFORM) $(BUILD)/pic/pic.ydef
	@mkdir -p $(BUILD)/pic
	cd $(BUILD)/pic && V8ROOT=$(ROOTFS) $(LEX) $(PICSRC)/picl.l

$(BUILD)/pic/picy.o: $(BUILD)/pic/y.tab.c $(V8CC_DEPS)
	$(V8CCRUN) -I$(BUILD)/pic -I$(PICSRC) -c -o $@ $(BUILD)/pic/y.tab.c
$(BUILD)/pic/picl.o: $(BUILD)/pic/lex.yy.c $(V8CC_DEPS)
	$(V8CCRUN) -I$(BUILD)/pic -I$(PICSRC) -c -o $@ $(BUILD)/pic/lex.yy.c
$(BUILD)/pic/%.o: $(PICSRC)/%.c $(BUILD)/pic/pic.ydef $(V8CC_DEPS)
	@mkdir -p $(BUILD)/pic
	$(V8CCRUN) -I$(BUILD)/pic -I$(PICSRC) -c -o $@ $<
$(PIC_OBJ): $(PICSRC)/pic.h

# ---------------------------------------------------------------------------
# spell -- four programs sharing hash.c/huff.c, plus the shell driver.
#
#   hashmake   words           -> octal hash codes
#   spellin    hash codes      -> the compressed hlist spellprog reads
#   spellprog  hlist + text    -> the words not in it
#   hashcheck  hlist           -> back to hash codes, for verification
# ---------------------------------------------------------------------------
SPELLSRC = $(SRC)/cmd/spell
SPELL_SHARED = hash huff hashlook
SPELL_PROGS = spellprog spellin hashmake hashcheck
SPELL_OBJ = $(patsubst %,$(BUILD)/spell/%.o,$(SPELL_SHARED) $(SPELL_PROGS))

spell: $(patsubst %,$(BUILD)/spell/%,$(SPELL_PROGS))

$(BUILD)/spell/spellprog: $(BUILD)/spell/spellprog.o $(BUILD)/spell/hash.o \
                          $(BUILD)/spell/hashlook.o $(BUILD)/spell/huff.o $(V8DEPS)
	$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(BUILD)/spell/spellprog.o $(BUILD)/spell/hash.o $(BUILD)/spell/hashlook.o $(BUILD)/spell/huff.o $(V8LIBS)
	@echo "built $@"
$(BUILD)/spell/spellin: $(BUILD)/spell/spellin.o $(BUILD)/spell/huff.o \
                        $(BUILD)/spell/hash.o $(V8DEPS)
	$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(BUILD)/spell/spellin.o $(BUILD)/spell/huff.o $(BUILD)/spell/hash.o  $(V8LIBS)
	@echo "built $@"
$(BUILD)/spell/hashmake: $(BUILD)/spell/hashmake.o $(BUILD)/spell/hash.o $(V8DEPS)
	$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(BUILD)/spell/hashmake.o $(BUILD)/spell/hash.o  $(V8LIBS)
	@echo "built $@"
$(BUILD)/spell/hashcheck: $(BUILD)/spell/hashcheck.o $(BUILD)/spell/hash.o \
                          $(BUILD)/spell/huff.o $(V8DEPS)
	$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(BUILD)/spell/hashcheck.o $(BUILD)/spell/hash.o $(BUILD)/spell/huff.o  $(V8LIBS)
	@echo "built $@"

$(BUILD)/spell/%.o: $(SPELLSRC)/%.c $(V8CC_DEPS)
	@mkdir -p $(BUILD)/spell
	$(V8CCRUN) -I$(SPELLSRC) -c -o $@ $<
# hash.h carries HALFWORD, which picks the fetch macro; huff.h was missing from
# the old pattern rule.  See src/cmd/spell/PORTING.md -- getting the fetch macro
# wrong is the one bug the V8 authors left a runtime assertion for.
$(SPELL_OBJ): $(SPELLSRC)/hash.h $(SPELLSRC)/huff.h

# ---------------------------------------------------------------------------
# refer -- the bibliographic preprocessor, and the four helpers it EXECS.
#
# refer(1) is a front end: it shells out to /usr/lib/refer/mkey and hunt to do
# the actual lookup, so those have to exist in the rootfs before refer works at
# all.  Its first failure here was not a hang but an interactive shell -- see
# src/libc/gen/exec.c for why.
#
# whatabout/ and the six files it needs (flagger, kaiser, thash, what1/2/4) are
# NOT built: they are not in upstream's `all` either, and they use a pre-C89
# initialiser syntax (`int x 5;`) that V8's own grammar rejects.
# ---------------------------------------------------------------------------
REFERSRC = $(SRC)/cmd/refer
REFER_MAIN = glue1 glue2 glue3 glue4 glue5 refer0 refer1 refer2 refer4 refer5 \
             refer6 refer7 refer8 hunt2 hunt3 hunt5 hunt6 hunt7 hunt8 hunt9 \
             mkey3 shell deliv2
REFER_MKEY = mkey1 mkey2 mkey3 deliv2
REFER_INV  = inv1 inv2 inv3 inv5 inv6 deliv2
REFER_HUNT = hunt1 hunt2 hunt3 hunt5 hunt6 hunt7 glue5 refer3 hunt9 shell \
             deliv2 hunt8 glue4 tick
REFER_DELIV = deliv1 deliv2
REFER_PROGS = refer mkey inv hunt deliv
# The five programs share objects, so sort/dedupe rather than list four times.
REFER_OBJ = $(sort $(patsubst %,$(BUILD)/refer/%.o, \
              $(REFER_MAIN) $(REFER_MKEY) $(REFER_INV) $(REFER_HUNT) $(REFER_DELIV)))

refer: $(patsubst %,$(ROOTFS)/usr/lib/refer/%,mkey inv hunt deliv) \
       $(BUILD)/refer/refer

$(BUILD)/refer/refer: $(patsubst %,$(BUILD)/refer/%.o,$(REFER_MAIN)) $(V8DEPS)
	$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o \
	    $(patsubst %,$(BUILD)/refer/%.o,$(REFER_MAIN)) $(V8LIBS)
	@echo "built $@"
$(BUILD)/refer/mkey: $(patsubst %,$(BUILD)/refer/%.o,$(REFER_MKEY)) $(V8DEPS)
	$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o \
	    $(patsubst %,$(BUILD)/refer/%.o,$(REFER_MKEY)) $(V8LIBS)
$(BUILD)/refer/inv: $(patsubst %,$(BUILD)/refer/%.o,$(REFER_INV)) $(V8DEPS)
	$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o \
	    $(patsubst %,$(BUILD)/refer/%.o,$(REFER_INV)) $(V8LIBS)
$(BUILD)/refer/hunt: $(patsubst %,$(BUILD)/refer/%.o,$(REFER_HUNT)) $(V8DEPS)
	$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o \
	    $(patsubst %,$(BUILD)/refer/%.o,$(REFER_HUNT)) $(V8LIBS)
$(BUILD)/refer/deliv: $(patsubst %,$(BUILD)/refer/%.o,$(REFER_DELIV)) $(V8DEPS)
	$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o \
	    $(patsubst %,$(BUILD)/refer/%.o,$(REFER_DELIV)) $(V8LIBS)

# refer execs these by absolute path; the shim resolves /usr/lib inside $$V8ROOT.
$(ROOTFS)/usr/lib/refer/%: $(BUILD)/refer/%
	@mkdir -p $(ROOTFS)/usr/lib/refer
	@cp $< $@

$(BUILD)/refer/%.o: $(REFERSRC)/%.c $(V8CC_DEPS)
	@mkdir -p $(BUILD)/refer
	$(V8CCRUN) -I$(REFERSRC) -c -o $@ $<
# Twelve of refer's files `#include "refer..c"` -- refer's shared declarations,
# under a name ending in .c, so it is invisible both to a header scanner and to
# any *.c glob.  Exactly the shape that made lex's once.c go stale.
$(REFER_OBJ): $(REFERSRC)/refer..c

EQNSRC = $(SRC)/cmd/eqn
EQN_NAMES = main diacrit eqnbox font fromto funny glob integral input lex \
            lookup mark matrix move over paren pile shift size sqrt text
EQN_OBJ = $(patsubst %,$(BUILD)/eqn/%.o,$(EQN_NAMES)) $(BUILD)/eqn/eqn.o

eqn: $(BUILD)/eqn/eqn
$(BUILD)/eqn/eqn: $(EQN_OBJ) $(BUILD)/crt0.o $(BUILD)/libc/libv8c.a \
                  $(BUILD)/v8sys/libv8stubs.a $(BUILD)/v8sys/libv8sys.a
	$(HOSTCC) -nostdlib -e _v8start -o $@ $(BUILD)/crt0.o $(EQN_OBJ) \
	    $(BUILD)/libc/libv8c.a $(BUILD)/v8sys/libv8stubs.a \
	    $(BUILD)/v8sys/libv8sys.a -lSystem
	@echo "built $@"

# The grammar, run through V8's yacc.  e.def is y.tab.h, which the other files
# include for the token numbers; split into its own rule for the same reason as
# pic's pic.ydef, and copied only when changed for the same reason too.
$(BUILD)/eqn/y.tab.c: $(EQNSRC)/eqn.y $(YACC) $(ROOTFS_YACCPAR)
	@mkdir -p $(BUILD)/eqn
	cd $(BUILD)/eqn && V8ROOT=$(ROOTFS) $(YACC) -d $(EQNSRC)/eqn.y

$(BUILD)/eqn/e.def: $(BUILD)/eqn/y.tab.c
	@cp $(BUILD)/eqn/y.tab.h $@

$(BUILD)/eqn/eqn.o: $(BUILD)/eqn/y.tab.c $(V8CC_DEPS)
	$(V8CCRUN) -I$(BUILD)/eqn -I$(EQNSRC) -c -o $@ $(BUILD)/eqn/y.tab.c
$(BUILD)/eqn/%.o: $(EQNSRC)/%.c $(BUILD)/eqn/e.def $(V8CC_DEPS)
	@mkdir -p $(BUILD)/eqn
	$(V8CCRUN) -I$(BUILD)/eqn -I$(EQNSRC) -c -o $@ $<
$(EQN_OBJ): $(EQNSRC)/e.h

# ---------------------------------------------------------------------------
# rootfs -- the V8-shaped tree v8cc runs out of.  $V8ROOT points here.
# ---------------------------------------------------------------------------

# A phony ALIAS over real files, not a recipe.  Nothing depends on this name
# any more; the things that used to say `| rootfs` now name the files they
# actually read, so make can tell when they change.
rootfs: $(V8CC_DEPS) $(ROOTFS_YACCPAR) $(ROOTFS_NCFORM) $(ROOTFS_TERM) $(ROOTFS_FONT) $(ROOTFS_ETC) $(ROOTFS_DEV) $(ROOTFS)/usr/bin/macos

# /dev/dk and /dev/pt are DIRECTORY targets, which is the one place that is
# right: the rule against directory stamps elsewhere in this file is about a
# stamp standing in for many files, and here the directory IS the artefact,
# because its content is nothing.  Delete one and make puts it back.
$(ROOTFS)/dev/dk:
	@mkdir -p $@
$(ROOTFS)/dev/pt:
	@mkdir -p $@
$(ROOTFS)/dev/drum:
	@mkdir -p $(@D) && : > $@

# The 132 /dev/fd nodes, as STATIC PATTERN rules rather than one multi-target
# rule.  The distinction is the one check-makefile.sh is about: a static pattern
# expands to one independent rule per target, each recipe creating exactly its
# own $@, where `a b c:' with a recipe is three rules make may run concurrently
# for reasons it cannot see.  Delete any one node and make puts back that node.
$(ROOTFS_DEVFD): $(ROOTFS)/dev/fd/%:
	@mkdir -p $(@D) && : > $@
$(ROOTFS_DEVSTD): $(ROOTFS)/dev/%:
	@mkdir -p $(@D) && : > $@

# The three passes v8cc execs, each tracking its build-tree original.
$(ROOTFS_CPP): $(BUILD)/cpp/cpp
	@mkdir -p $(@D) && cp $< $@
$(ROOTFS_CCOM): $(A64BUILD)/v8ccom
	@mkdir -p $(@D) && cp $< $@
$(ROOTFS_CC): $(BUILD)/cc/v8cc
	@mkdir -p $(@D) && cp $< $@

# The header tree, as one stamp.  Per-file rules for ~90 upstream headers would
# buy nothing: the unit that matters is "is the include path current", and any
# header changing invalidates every object either way.
# $(SRC)/include/sys/*.h is listed SEPARATELY and must stay that way.  The
# glob above it, $(SRC)/include/*, matches the `sys' DIRECTORY -- and on macOS a
# directory's mtime moves when an entry is created or removed, not when a file
# inside it is edited.  So adding src/include/sys/param.h rebuilt the stamp
# (the directory appeared) and every later edit to it did not, leaving the
# rootfs carrying a header that no longer matched its source.  Caught by a
# mutation test that refused to fail; tests/deps has the case now.
ROOTFS_INC_SRC = $(wildcard $(V8INCSRC)/*.h) $(wildcard $(V8INCSRC)/sys/*.h) \
                 $(wildcard $(SRC)/include/*) $(wildcard $(SRC)/include/sys/*.h) \
                 $(JERQINC)/jioctl.h
$(ROOTFS_INC): $(ROOTFS_INC_SRC)
	@mkdir -p $(ROOTFS)/usr/include
	@cp -R $(V8INCSRC)/. $(ROOTFS)/usr/include/
	@# Patched headers go on LAST, over the pristine ones.  Only headers the
	@# port genuinely had to change live in src/include -- so far setjmp.h,
	@# whose jmp_buf is 40 VAX bytes and cannot hold AAPCS64's callee-saved
	@# set.  src/include/PROVENANCE records the upstream hash of each, so the
	@# diff against pristine V8 stays reconstructible.
	@cp -R $(SRC)/include/. $(ROOTFS)/usr/include/
	@# The jerq headers, which ls(1) and mc(1) reach for by absolute path.
	@# Unchanged upstream files; only their location moves.
	@cp $(JERQINC)/jioctl.h $(ROOTFS)/usr/include/
	@touch $@

# The data files V8 programs open by absolute path.  The shim resolves
# /usr/lib/... inside $V8ROOT -- see rootpath() in shim/v8sys/syscall.c.
# These are INPUTS TO PROGRAMS, so the rules that run those programs name them:
# yaccpar is a prerequisite of every yacc run, ncform of every lex run, and the
# term and font tables of the nroff and troff test suites.
$(ROOTFS_YACCPAR): $(YACCSRC)/yaccpar
	@mkdir -p $(@D) && cp $< $@
$(ROOTFS_NCFORM): $(LEXSRC)/ncform
	@mkdir -p $(@D) && cp $< $@

# The genuine V8 /etc files, installed unchanged.
$(ROOTFS)/etc/%: $(SRC)/etc/%
	@mkdir -p $(@D) && cp $< $@

# passwd is SYNTHESIZED, and it is the one file here that has to be.
#
# V8's own /etc/passwd names Bell Labs people with 1985 uids and encrypted
# passwords, and installing it would be worse than useless: getpwuid() would
# fail to find the person actually running the system, so `ls -l` would print a
# bare numeric uid for every file they own, and `whoami` would have nothing to
# say.  The V8 world has to know who you are.
#
# So this is generated from the real user at build time -- name, uid and gid
# from the host, home as V8 would have spelled it, and V8's own shell.  The
# password field is `*`: nothing authenticates against this file, and a
# plausible-looking hash in a checked-out tree would be a lie of a kind worth
# avoiding.  root is included because file listings and `chown` reference uid 0.
$(ROOTFS)/etc/passwd:
	@mkdir -p $(@D)
	@{ echo "root:*:0:0:Superuser:/:/bin/sh"; \
	   echo "$$(id -un):*:$$(id -u):$$(id -g):$$(id -F 2>/dev/null || id -un):/usr/$$(id -un):/bin/sh"; \
	 } > $@
	@echo "built $@ (synthesized for $$(id -un))"
$(ROOTFS)/usr/lib/term/tab.%: $(TROFFSRC)/term/tab.%
	@mkdir -p $(@D) && cp $< $@

# crt0 and the libraries, installed into the rootfs so that `cc -o prog prog.c`
# links the V8 world rather than clang's startup and the host libc.  See the
# link step in src/cmd/cc.c for why linking the host libc breaks variadic calls.
#
# SEPARATE from rootfs on purpose: the libraries are compiled BY v8cc, which
# needs the rootfs to exist first (headers, cpp, ccom).  Folding this into
# rootfs makes the dependency circular.
# Real file targets, not a phony copy step.  A phony one only refreshes the
# rootfs when it happens to be invoked, so `make libv8c` left the copy the
# driver actually links STALE -- which cost a debugging round when spell's
# sscanf kept reaching the host's __svfscanf_l after V8's had been added.
# ROOTFS_LIBS itself is defined at the top of this file, beside ROOTFS.
rootfs-libs: $(ROOTFS_LIBS)
$(ROOTFS)/lib/crt0.o: $(BUILD)/crt0.o
	@mkdir -p $(@D) && cp $< $@
$(ROOTFS)/lib/libv8c.a: $(BUILD)/libc/libv8c.a
	@mkdir -p $(@D) && cp $< $@
$(ROOTFS)/lib/libv8stubs.a: $(BUILD)/v8sys/libv8stubs.a
	@mkdir -p $(@D) && cp $< $@
$(ROOTFS)/lib/libv8sys.a: $(BUILD)/v8sys/libv8sys.a
	@mkdir -p $(@D) && cp $< $@

# libm, and it is a stub because V8's was one.  See shim/libm/dummy.c: upstream's
# libm.a holds a single 62-byte object defining nothing, because the math is in
# libc.  Built with the HOST compiler, like crt0's neighbours in spirit -- there
# is no code in it to be authentic about, and making it a v8cc target would put
# a chicken-and-egg between the driver's -l resolution and the library it
# resolves to.
$(BUILD)/libm/dummy.o: shim/libm/dummy.c
	@mkdir -p $(@D)
	@$(HOSTCC) -fcommon -c -o $@ $<
$(ROOTFS)/usr/lib/libm.a: $(BUILD)/libm/dummy.o
	@mkdir -p $(@D)
	@rm -f $@ && ar rc $@ $< && ranlib $@

# ---------------------------------------------------------------------------
# libtermlib -- the termcap library, and the first thing imported out of
# usr/src/lib rather than usr/src/cmd.
#
# WHICH OF THE TWO UPSTREAM COPIES THIS IS, because there are two and they
# differ.  usr/src/lib/libtermlib and usr/src/cmd/ex/termlib hold the same
# termcap.c apart from eleven lines, and CLAUDE.md's rule is to find out which
# one the BUILD reads.  Measured rather than guessed: `ioctl' appears in the
# whole library only inside those eleven lines, and the shipped
# usr/lib/libtermcap.a carries an undefined _ioctl -- so the shipped library
# was built from lib/libtermlib, jerq block and all.  The ex copy installs to
# $(UCB)/lib with UCB=/lusr/ucb, a directory that is not in the shipped tree
# at all, so it is the Berkeley import staging area rather than the answer.
#
# THE FOUR -D FLAGS ARE UPSTREAM'S AND ARE NOT DECORATION.  Its makefile opens
# `CFLAGS= -DCM_N -DCM_GT -DCM_B -DCM_D', and each one guards a case in
# tgoto's cursor-addressing interpreter: %n, %>, %B (BCD) and %D (Delta Data).
# Dropping one does not fail to build -- it makes tgoto fall into `default:'
# and return the string "OOPS" for every terminal whose cm= uses that escape.
# /etc/termcap has entries using all four.
TERMLIBSRC   = $(SRC)/lib/libtermlib
TERMLIB_CM   = -DCM_N -DCM_GT -DCM_B -DCM_D
TERMLIB_NAME = termcap tgoto tputs
TERMLIB_OBJ  = $(patsubst %,$(BUILD)/termlib/%.o,$(TERMLIB_NAME))

$(BUILD)/termlib/%.o: $(TERMLIBSRC)/%.c $(V8CC_DEPS)
	@mkdir -p $(BUILD)/termlib
	$(V8CCRUN) $(TERMLIB_CM) -c -o $@ $<

$(BUILD)/termlib/libtermcap.a: $(TERMLIB_OBJ)
	@rm -f $@ && ar rcs $@ $(TERMLIB_OBJ)
	@echo "built $@"

$(ROOTFS)/usr/lib/libtermcap.a: $(BUILD)/termlib/libtermcap.a
	@mkdir -p $(@D) && cp $< $@
# A HARD LINK, because that is what upstream's install does -- `ln
# libtermcap.a libtermlib.a' -- and the shipped tree has both names at 6556
# bytes.  Keyed on the file it links to rather than on the archive, so a
# rebuilt libtermcap.a re-links this one instead of leaving a stale inode.
$(ROOTFS)/usr/lib/libtermlib.a: $(ROOTFS)/usr/lib/libtermcap.a
	@rm -f $@ && ln $< $@

# troff's device tables.  makedev compiles the plain-text description in
# dev202/ into the binary DESC.out and per-font .out files troff opens at
# startup; the original makefile does the same.  It is a BUILD tool, like yacc,
# so it is built with the host compiler and never ends up in the rootfs.
#
# Three steps, three targets.  Rolling them into one rule keyed on DESC.out
# meant that deleting the INSTALLED tables under the rootfs did not restore
# them: DESC.out was still there, so the recipe -- and with it the install --
# never ran.
devtables: $(ROOTFS_FONT)
$(BUILD)/troff/makedev: $(TROFFSRC)/makedev.c
	@mkdir -p $(BUILD)/troff
	@$(HOSTCC) -std=gnu89 -fcommon -w -I$(TROFFSRC) -o $@ $<
$(BUILD)/troff/dev202/DESC.out: $(BUILD)/troff/makedev $(wildcard $(TROFFSRC)/dev202/*)
	@mkdir -p $(BUILD)/troff/dev202
	@cp -R $(TROFFSRC)/dev202/. $(BUILD)/troff/dev202/
	@cd $(BUILD)/troff/dev202 && ../makedev DESC > /dev/null

# Each installed table is its own target, so deleting one restores it.  makedev
# writes the whole set in a single run, so the prerequisite is DESC.out for all
# of them -- one rule, no multi-target ambiguity, which make 3.81 would
# otherwise turn into twelve makedev runs.
#
# THE RECIPE COPIES ONE FILE, NOT THE SET, and that is a -j fix rather than
# tidiness.  It used to be `cp $(BUILD)/troff/dev202/*.out $(@D)/', so make ran
# twelve concurrent instances of this recipe and every one of them wrote all
# twelve files -- 144 overlapping copies into one directory.  macOS cp clones
# through a temporary and rename, so one job could stat a name another had
# momentarily moved aside, and it failed with
#
#	cp: .../dev202/BI.out: No such file or directory
#
# Intermittent, which is why it survived: reproduced here in three runs out of
# six after `rm -rf rootfs/usr/lib/font', and it took down CI only once the
# runner happened to lose the race.  $* is the stem, so each invocation now
# touches exactly the file its own target names.
$(ROOTFS)/usr/lib/font/dev202/%.out: $(BUILD)/troff/dev202/DESC.out
	@mkdir -p $(@D) && cp $(BUILD)/troff/dev202/$*.out $@

clean:
	rm -rf $(BUILD) $(ROOTFS)

distclean: clean
	rm -rf $(ROOT)build

# ---------------------------------------------------------------------------
# Header dependencies.
#
# Host-compiled objects get theirs from clang (-MMD, see DEPFLAGS).  Objects
# built by v8cc cannot: V8's driver has no dependency-generation flag and is not
# getting one, since inventing options the original never had is the sort of
# convenience that erodes the thing being preserved.  They get a coarse
# dependency instead -- overbuilds sometimes, never underbuilds.
#
# The V8 system headers are handled for EVERY v8cc object by $(ROOTFS_INC),
# which is in $(V8CC_DEPS).  What is left here is the one include directory
# that is not in the rootfs: libc's own stdio headers, which reach the compiler
# through the -I in V8CCRUN rather than through /usr/include.
# ---------------------------------------------------------------------------
$(LIBC_OBJ): $(wildcard $(LIBCSRC)/stdio/*.h)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

# ---------------------------------------------------------------------------
# install -- put the world somewhere you can live in it.
#
#	make install                      -> /usr/local/v8, launcher in /usr/local/bin
#	make install PREFIX=~/.local/v8 BINDIR_HOST=~/.local/bin
#
# THE REBUILD IS NOT OPTIONAL.  $(V8ROOT_DEFAULT) is compiled into every V8
# binary (v8root(), shim/v8sys/syscall.c) and into the driver, so binaries built
# for the build tree carry the build tree's path.  Installing those would give a
# world whose programs quietly reach back into the source directory -- working,
# until that directory moves, and then failing the way this port has learned to
# distrust most: silently, somewhere else.  So install rebuilds with the prefix
# stamped in.
#
# The launcher is a THIN SHELL SCRIPT, not a copy of $(ROOTFS)/usr/bin/v8.  The V8
# binary is what runs once you are inside; the script is what a Mac user types,
# and it exists to set PATH so the shell finds V8's tools before the host's.
PREFIX      ?= /usr/local/v8
BINDIR_HOST ?= /usr/local/bin

.PHONY: install
install:
	@echo "installing to $(PREFIX) (launcher: $(BINDIR_HOST)/v8)"
	@# clean first, and not out of caution.  make does not track a change to
	@# V8ROOT_DEFAULT -- it is a recipe flag, not a prerequisite -- so an
	@# incremental build here would relink nothing and install binaries still
	@# carrying the build tree's path.  Exactly the staleness class this
	@# repo has been bitten by four times; a clean -j8 build is 4 seconds.
	@#
	@# It has a second reason now, learned the hard way when $(call v8dest,..)
	@# moved forty-one commands out of /bin: make INSTALLS files and never
	@# removes them, so a change of destination leaves the old copy behind and
	@# `cp -R $(ROOTFS)/.' would ship both.  A rootfs is a build output and the
	@# only honest way to change its shape is to rebuild it.
	@$(MAKE) clean
	@$(MAKE) V8ROOT_DEFAULT=$(PREFIX)/golden all
	@mkdir -p $(PREFIX) $(BINDIR_HOST)
	@rm -rf $(PREFIX)/golden && mkdir -p $(PREFIX)/golden
	@cp -R $(ROOTFS)/. $(PREFIX)/golden/
	@cp $(REPO)tools/v8launch.sh $(BINDIR_HOST)/v8
	@sed -i '' 's|@GOLDEN@|$(PREFIX)/golden|' $(BINDIR_HOST)/v8
	@chmod 755 $(BINDIR_HOST)/v8
	@echo "installed.  run: $(BINDIR_HOST)/v8"

# ---------------------------------------------------------------------------
# man -- and man is mostly a shell script.
#
# man.c is 19 lines: it stats a page and prints -rd/-rm/-ry, the troff number
# registers that put a date in the footer.  man.sh is the actual program, and
# mancache.sh maintains the formatted cache.  Upstream's own Makefile says
# where each goes -- /usr/lib/man, /usr/bin/man, /usr/lib/mancache -- and that
# layout is followed rather than invented, because man.sh reaches for its
# helper by that path.
#
# The pages themselves are upstream's, installed unchanged.  man without pages
# is not man.
MANSRC = $(SRC)/cmd/man
MANPAGESRC = $(REPO)third_party/Research-Unix-v8/v8/usr/man

man: $(ROOTFS)/usr/lib/man $(ROOTFS)/usr/bin/man $(ROOTFS)/usr/lib/mancache \
     $(ROOTFS)/usr/man/.stamp

$(BUILD)/man/man: $(MANSRC)/man.c $(V8CC_DEPS) $(V8DEPS)
	@mkdir -p $(BUILD)/man
	@$(V8CCRUN) -c -o $(BUILD)/man/man.o $<
	@$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(BUILD)/man/man.o $(V8LIBS)
	@echo "built $@"

$(ROOTFS)/usr/lib/man: $(BUILD)/man/man
	@mkdir -p $(@D) && cp $< $@
$(ROOTFS)/usr/bin/man: $(MANSRC)/man.sh
	@mkdir -p $(@D) && cp $< $@ && chmod 755 $@
$(ROOTFS)/usr/lib/mancache: $(MANSRC)/mancache.sh
	@mkdir -p $(@D) && cp $< $@ && chmod 755 $@

# One stamp for ~1200 pages: the unit that matters is "are the pages there",
# and per-file rules would buy nothing but a slower read of this file.
$(ROOTFS)/usr/man/.stamp: $(MANPAGESRC)/man1/cat.1
	@mkdir -p $(ROOTFS)/usr/man
	@cp -R $(MANPAGESRC)/. $(ROOTFS)/usr/man/
	@touch $@

# ---------------------------------------------------------------------------
# The Wave C programs, INSTALLED.  They were built and tested for months and
# never put where the V8 world could reach them -- the same gap that hid yacc
# and lex until a program built from its own makefile asked for one.  man is
# what surfaced it here: man.sh runs `nroff`, and nroff was in build/ only.
WAVEC_INSTALL = nroff troff eqn tbl pic refer grap
WAVEC_TARGETS = $(patsubst %,$(ROOTFS)/usr/bin/%,$(WAVEC_INSTALL))

wavec-install: $(WAVEC_TARGETS) $(ROOTFS)/usr/lib/tmac/.stamp \
               $(ROOTFS)/usr/lib/grap.defines

# The macro packages nroff and troff read by absolute path.  tmac.an is what
# man formats with; without it nroff says "cannot open file /usr/lib/tmac/
# tmac.an" and man produces nothing.  Upstream's files, installed unchanged.
TMACSRC = $(REPO)third_party/Research-Unix-v8/v8/usr/lib/tmac
# tmac.an is a two-line stub that sources /usr/lib/macros/an -- the actual
# macro definitions -- so both directories go in or neither is any use.
MACROSRC = $(REPO)third_party/Research-Unix-v8/v8/usr/lib/macros
$(ROOTFS)/usr/lib/tmac/.stamp: $(TMACSRC)/tmac.an $(MACROSRC)/an
	@mkdir -p $(ROOTFS)/usr/lib/tmac $(ROOTFS)/usr/lib/macros
	@cp -R $(TMACSRC)/. $(ROOTFS)/usr/lib/tmac/
	@cp -R $(MACROSRC)/. $(ROOTFS)/usr/lib/macros/
	@touch $@

$(ROOTFS)/usr/bin/nroff: $(BUILD)/nroff/nroff
	@mkdir -p $(@D) && cp $< $@
$(ROOTFS)/usr/bin/troff: $(BUILD)/troff/troff
	@mkdir -p $(@D) && cp $< $@
$(ROOTFS)/usr/bin/eqn: $(BUILD)/eqn/eqn
	@mkdir -p $(@D) && cp $< $@
$(ROOTFS)/usr/bin/tbl: $(BUILD)/tbl/tbl
	@mkdir -p $(@D) && cp $< $@
$(ROOTFS)/usr/bin/pic: $(BUILD)/pic/pic
	@mkdir -p $(@D) && cp $< $@
$(ROOTFS)/usr/bin/refer: $(BUILD)/refer/refer
	@mkdir -p $(@D) && cp $< $@

# ---------------------------------------------------------------------------
# spell -- four programs and three data files, laid out as upstream says.
#
# `spell` is a shell script; spellprog is the engine it execs from /usr/lib, and
# the hashed word lists live in /usr/dict.  src/cmd/spell/Makefile's own install
# rule is followed literally rather than invented:
#
#	mv spellprog /usr/lib/spell
#	cp hlista hlistb hstop /usr/dict
#	cp spell.sh /usr/bin/spell
#
# The lists are upstream's prebuilt ones.  They CAN be regenerated here --
# hashmake and spellin are built and the recipe is in that makefile -- but the
# shipped files are the data V8 actually ran with, and regenerating them would
# substitute our own hash of a dictionary for theirs without being asked to.
DICTSRC = $(REPO)third_party/Research-Unix-v8/v8/usr/dict
SPELL_INSTALL = $(ROOTFS)/usr/lib/spell $(ROOTFS)/usr/bin/spell \
                $(ROOTFS)/usr/dict/hlista $(ROOTFS)/usr/dict/hlistb \
                $(ROOTFS)/usr/dict/hstop $(ROOTFS)/usr/dict/words

spell-install: $(SPELL_INSTALL)

$(ROOTFS)/usr/lib/spell: $(BUILD)/spell/spellprog
	@mkdir -p $(@D) && cp $< $@
$(ROOTFS)/usr/bin/spell: $(SPELLSRC)/spell.sh
	@mkdir -p $(@D) && cp $< $@ && chmod 755 $@
$(ROOTFS)/usr/dict/%: $(DICTSRC)/%
	@mkdir -p $(@D) && cp $< $@

# ---------------------------------------------------------------------------
# grap -- the program that made v8cc implement passing a STRUCT BY VALUE.  It
# used to stop at
#
#	compiler error: gencode: unimplemented operator 99 (STARG)
#
# because grap's Point is {struct obj *; double x, y} -- 24 bytes -- and
# `line(type, p1, p2, desc)` takes two of them.  156 Wave A programs and all of
# Wave B and C had never asked.  placeargs() in the back end now copies the
# aggregate into consecutive argument slots, the V8/VAX convention; PLAN.md S4f
# has the reasoning and what it was measured against.
#
# grap -- pic's sibling: a preprocessor for graphs, and pic-shaped in every way
# that matters here.  Its own makefile builds grap.o and grapl.o from the yacc
# and lex sources plus ten ordinary objects, and names prevy.tab.h as a
# prerequisite of all of them -- the same "y.tab.h under another name" trick pic
# uses, and not reproduced here for the same reason (see the pic block above).
#
# Upstream links -lm.  V8's libc carries the maths it needs, so the link line is
# the standard V8 one; if something turns out to be missing it will fail loudly
# at link rather than resolving from -lSystem, because -nostdlib means there is
# no host libm on the line to resolve from.
# ---------------------------------------------------------------------------
GRAPSRC = $(SRC)/cmd/grap
GRAP_NAMES = main input print frame for coord ticks plot label misc
GRAP_OBJ = $(patsubst %,$(BUILD)/grap/%.o,$(GRAP_NAMES)) \
           $(BUILD)/grap/grapy.o $(BUILD)/grap/grapl.o

grap: $(BUILD)/grap/grap
$(BUILD)/grap/grap: $(GRAP_OBJ) $(V8DEPS)
	$(HOSTCC) $(V8LDFLAGS) -o $@ $(BUILD)/crt0.o $(GRAP_OBJ) $(V8LIBS)
	@echo "built $@"

$(BUILD)/grap/y.tab.c: $(GRAPSRC)/grap.y $(YACC) $(ROOTFS_YACCPAR)
	@mkdir -p $(BUILD)/grap
	cd $(BUILD)/grap && V8ROOT=$(ROOTFS) $(YACC) -d $(GRAPSRC)/grap.y

$(BUILD)/grap/lex.yy.c: $(GRAPSRC)/grapl.l $(LEX) $(ROOTFS_NCFORM) $(BUILD)/grap/y.tab.c
	@mkdir -p $(BUILD)/grap
	cd $(BUILD)/grap && V8ROOT=$(ROOTFS) $(LEX) $(GRAPSRC)/grapl.l

$(BUILD)/grap/grapy.o: $(BUILD)/grap/y.tab.c $(V8CC_DEPS)
	$(V8CCRUN) -I$(BUILD)/grap -I$(GRAPSRC) -c -o $@ $(BUILD)/grap/y.tab.c
$(BUILD)/grap/grapl.o: $(BUILD)/grap/lex.yy.c $(V8CC_DEPS)
	$(V8CCRUN) -I$(BUILD)/grap -I$(GRAPSRC) -c -o $@ $(BUILD)/grap/lex.yy.c
$(BUILD)/grap/%.o: $(GRAPSRC)/%.c $(BUILD)/grap/y.tab.c $(V8CC_DEPS)
	@mkdir -p $(BUILD)/grap
	$(V8CCRUN) -I$(BUILD)/grap -I$(GRAPSRC) -c -o $@ $<
$(GRAP_OBJ): $(GRAPSRC)/grap.h

# grap.defines is a RUNTIME file, not a build input: main.c opens
# "/usr/lib/grap.defines" by absolute path and warns if it is missing, the same
# shape as troff's tmac and spell's word lists.  A previous version of this
# block called it an #included non-header and made every object depend on it --
# wrong on both counts, and wrong about the one bug class this tree is most
# careful about.  There is no #included non-header in grap; step 4 of the
# porting checklist finds nothing here:
#	grep -rnE '#[ \t]*include[ \t]*"[^"]*"' src/cmd/grap | grep -v '\.h"'
$(ROOTFS)/usr/lib/grap.defines: $(GRAPSRC)/grap.defines
	@mkdir -p $(@D) && cp $< $@

$(ROOTFS)/usr/bin/grap: $(BUILD)/grap/grap
	@mkdir -p $(@D) && cp $< $@
