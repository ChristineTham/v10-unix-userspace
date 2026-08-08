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
ROOTFS_ETC     := $(patsubst $(SRC)/v8/etc/%,$(ROOTFS)/etc/%, \
                    $(wildcard $(SRC)/v8/etc/[a-z]*)) $(ROOTFS)/etc/passwd

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
ROOTFS_DEV     := $(ROOTFS)/dev/dk $(ROOTFS)/dev/pt $(ROOTFS)/dev/drum

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
               $(ROOTFS)/usr/lib/libm.a

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

.PHONY: install man grap wavec-install spell-install all stage0 test-deps test-jail test-selfhost v8bin v8make libv8kern cpp ccom-pass1 ccom-vax v8ccom v8cc rootfs rootfs-libs libv8sys libv8c crt0 sh nroff troff tbl v8yacc v8lex pic spell refer eqn devtables test test-cpp test-v8ccom test-v8cc test-v8sys test-freestanding test-libv8c test-wavea test-waveb test-sh test-wavec test-kmemu test-streams test-hooks test-mkfs mkfs imgbin srctree clean distclean
all: stage0
# libv8c belongs here.  Without it a plain `make` rebuilt the compiler but left
# libv8c.a compiled by the PREVIOUS one, so a back-end fix looked like it had
# not worked -- which cost a full debugging round on the indirect-call bug.
stage0: cpp v8ccom v8cc libv8sys libv8kern crt0 rootfs libv8c rootfs-libs sh nroff troff tbl v8yacc v8lex pic spell refer eqn v8make v8bin man wavec-install spell-install $(ROOTFS)/bin/[ $(ROOTFS)/bin/sh $(ROOTFS)/bin/make $(ROOTFS)/usr/bin/yacc $(ROOTFS)/usr/bin/lex srctree

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
test-v8sys: $(BUILD)/v8sys/test
	@$(BUILD)/v8sys/test
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
test-streams: libv8kern libv8c
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
$(BUILD)/v8sys/test: $(ROOT)tests/v8sys/test.c $(SHIM_SRC) $(SHIM_ASM)
	@mkdir -p $(BUILD)/v8sys
	$(HOSTCC) -std=gnu99 -Wall -Wno-unused-variable \
	    -fno-stack-protector -fno-stack-check -o $@ $^

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
            $(BUILD)/v8sys/stub/signal.o

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

KERN_LIB = $(BUILD)/kern/libv8kern.a
KERN_OBJ = $(BUILD)/kern/stream.o $(BUILD)/kern/machdep.o \
           $(BUILD)/kern/streamio.o $(BUILD)/kern/slp.o \
           $(BUILD)/kern/fio.o $(BUILD)/kern/subr.o $(BUILD)/kern/ioconf.o

# The headers streamio.c reaches, split by who wrote them.  Six are AUTHENTIC
# and live in src/sys/h/, so a quoted "../h/x.h" from src/sys/sys/ finds them
# first; five are OURS in shim/kern/h/, reached only because the authentic ones
# are absent and -Ishim/kern/dev turns "../h/" into that directory.  Nothing in
# the include syntax says which is which, which is the property the seam wants.
KERN_AUTH_H = $(SRC)/sys/h/stream.h $(SRC)/sys/h/dir.h $(SRC)/sys/h/inode.h \
              $(SRC)/sys/h/ioctl.h $(SRC)/sys/h/ttyld.h $(SRC)/sys/h/file.h \
              $(SRC)/sys/h/inline.h $(SRC)/sys/research/sparam.h
KERN_SHIM_H = $(ROOT)shim/kern/h/param.h $(ROOT)shim/kern/h/mtpr.h \
              $(ROOT)shim/kern/h/conf.h $(ROOT)shim/kern/h/user.h \
              $(ROOT)shim/kern/h/proc.h $(ROOT)shim/kern/h/buf.h \
              $(SRC)/include/setjmp.h

libv8kern: $(KERN_LIB)
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

$(BUILD)/kern/machdep.o: $(ROOT)shim/kern/dev/machdep.c \
                         $(ROOT)shim/kern/h/param.h $(ROOT)shim/kern/h/mtpr.h \
                         $(ROOT)shim/v8sys/rawsys.h
	@mkdir -p $(BUILD)/kern
	$(HOSTCC) $(SHIMFLAGS) $(DEPFLAGS) -I$(ROOT)shim/kern/dev -c $< -o $@

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
           tolower toupper atof sleep
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
        who

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
V8DIRBIN = cp dc ed factor mkdir mv primes rmdir sed fmt tsort

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

V8DIRBIN_BUILT   = $(patsubst %,$(BINDIR)/%,$(V8DIRBIN))
V8DIRBIN_INSTALL = $(call v8install,$(V8DIRBIN))

# Same reason as $(V8BIN_BUILT) above: without this make treats each of these
# as an intermediate of the $(ROOTFS)/bin/% copy and deletes it, leaving a
# working rootfs and no build tree to compare it against.
.SECONDARY: $(V8DIRBIN_BUILT)

v8bin: $(V8BIN_BUILT) $(V8BIN_INSTALL) $(V8LIBDATA) \
       $(V8DIRBIN_BUILT) $(V8DIRBIN_INSTALL) \
       $(BINDIR)/df $(BINDIR)/load $(BINDIR)/w $(BINDIR)/ps \
       $(call v8install,df load w ps) $(ROOTFS)/usr/bin/uptime \
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
# out of src/v8/etc and has no source for a binary.
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
rootfs: $(V8CC_DEPS) $(ROOTFS_YACCPAR) $(ROOTFS_NCFORM) $(ROOTFS_TERM) $(ROOTFS_FONT) $(ROOTFS_ETC) $(ROOTFS_DEV)

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
$(ROOTFS)/etc/%: $(SRC)/v8/etc/%
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
	@$(MAKE) V8ROOT_DEFAULT=$(PREFIX) all
	@mkdir -p $(PREFIX) $(BINDIR_HOST)
	@cp -R $(ROOTFS)/. $(PREFIX)/
	@{ echo '#!/bin/sh'; \
	   echo '# Enter the Research Unix V8 world.  Generated by `make install`.'; \
	   echo '#'; \
	   echo '# V8ROOT is exported for two reasons: it overrides the root compiled'; \
	   echo '# into the binaries, so a moved tree still works, and V8 make passes'; \
	   echo '# it down to everything it runs.  PATH puts V8 first while keeping the'; \
	   echo '# Mac reachable -- host binaries never call rootpath(), so they see'; \
	   echo '# the real macOS with no special casing.'; \
	   echo 'V8ROOT=$(PREFIX); export V8ROOT'; \
	   echo 'PATH=$$V8ROOT/bin:$$V8ROOT/usr/bin:$$PATH; export PATH'; \
	   echo 'exec "$$V8ROOT/usr/bin/v8" "$$@"'; \
	 } > $(BINDIR_HOST)/v8
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
