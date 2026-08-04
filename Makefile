# Research Unix V8 -> macOS/ARM64.  See PLAN.md.
#
# Stage 0 builds the V8 toolchain with the *host* compiler, just far enough to
# get a working v8cc.  Nothing built here is authentic-by-construction; it is
# scaffolding.  Once the ARM64 backend lands (Phase 1b) the world is rebuilt by
# v8cc itself, and the stage-0 binaries are only kept to prove the fixpoint.

ROOT    := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
BUILD   := $(ROOT)build/stage0
SRC     := $(ROOT)src
# Defined up here, not beside the rootfs target: make expands variables in a
# TARGET name as soon as it reads the rule, so any rule building something
# under the rootfs must come after this line.
ROOTFS  := $(ROOT)rootfs

HOSTCC  ?= clang

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
          -Wno-parentheses -Wno-unused-value

# 1978 yacc spelled actions '={ ... }'; modern yacc/bison wants '{ ... }'.
# This is a host-tool incompatibility, not a defect, so the .y files stay
# pristine and the fixup lives here.  V8's own yacc (once built) needs no fixup.
YACCFIX = sed 's/={/{/g'

# V8 libc internals that host libc lacks.  Scaffolding; gone in Phase 2b.
STAGE0_COMPAT = $(ROOT)tools/stage0-compat.c

# stubs.c and onestub.c are EXCLUDED here on purpose.  Between them they define
# open/read/write with their V8 names, which collide with the host functions the
# rest of the shim calls -- see shim/NOTES.md.  They are compiled only into
# libv8stubs.a, which the V8 world links with -nostdlib, never into anything
# that also uses host libc.  onestub.c additionally needs -DV8_NAME and friends
# and does not compile without them.
SHIM_SRC = $(filter-out $(ROOT)shim/v8sys/stubs.c $(ROOT)shim/v8sys/onestub.c, \
                        $(wildcard $(ROOT)shim/v8sys/*.c))

.PHONY: all stage0 cpp ccom-pass1 ccom-vax v8ccom v8cc rootfs rootfs-libs libv8sys libv8c crt0 sh nroff troff tbl v8yacc v8lex pic spell refer eqn devtables test test-cpp test-v8ccom test-v8cc test-v8sys test-freestanding test-libv8c test-wavea test-waveb test-sh test-wavec clean distclean
all: stage0
# libv8c belongs here.  Without it a plain `make` rebuilt the compiler but left
# libv8c.a compiled by the PREVIOUS one, so a back-end fix looked like it had
# not worked -- which cost a full debugging round on the indirect-call bug.
stage0: cpp v8ccom v8cc libv8sys crt0 rootfs libv8c rootfs-libs sh nroff troff tbl v8yacc v8lex pic spell refer eqn

test: test-cpp test-v8ccom test-v8cc test-v8sys test-freestanding test-libv8c test-wavea test-waveb test-sh test-wavec
test-cpp: cpp
	@$(ROOT)tests/cpp/run.sh $(BUILD)/cpp/cpp
test-v8ccom: v8ccom
	@$(ROOT)tests/v8ccom/run.sh $(A64BUILD)/v8ccom
test-v8cc: rootfs-libs
	@$(ROOT)tests/v8cc/run.sh
test-v8sys: $(BUILD)/v8sys/test
	@$(BUILD)/v8sys/test
test-freestanding: rootfs libv8sys crt0 rootfs-libs
	@$(ROOT)tests/freestanding/run.sh
test-libv8c: rootfs libv8sys crt0 libv8c rootfs-libs
	@$(ROOT)tests/libv8c/run.sh
test-wavea: rootfs libv8sys crt0 libv8c rootfs-libs
	@$(ROOT)tests/wavea/run.sh
test-waveb: rootfs libv8sys crt0 libv8c rootfs-libs
	@$(ROOT)tests/waveb/run.sh
test-sh: sh
	@$(ROOT)tests/sh/run.sh
test-wavec: nroff troff tbl eqn pic spell
	@$(ROOT)tests/wavec/run.sh

$(BUILD)/v8sys/test: $(ROOT)tests/v8sys/test.c $(SHIM_SRC)
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
A64BUILD = $(BUILD)/ccom-arm64
A64INC   = -I$(A64) -I$(CCOM_M)

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
SHIM_OBJ = $(patsubst $(ROOT)shim/v8sys/%.c,$(BUILD)/v8sys/%.o,$(SHIM_SRC))

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

$(BUILD)/v8sys/libv8stubs.a: $(STUB_OBJ)
	@ar rcs $@ $(STUB_OBJ)
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
	@ar rcs $@ $(SHIM_OBJ)
	@echo "built $@"

# -fno-stack-protector and -fno-stack-check: both emit calls to libc helpers
# (___stack_chk_fail, ___chkstk_darwin), and the whole point of the shim is that
# it names no libc symbol -- see shim/v8sys/rawsys.h.
SHIMFLAGS = -std=gnu99 -Wall -Wno-unused-function \
            -fno-stack-protector -fno-stack-check

$(BUILD)/v8sys/%.o: $(ROOT)shim/v8sys/%.c
	@mkdir -p $(BUILD)/v8sys
	$(HOSTCC) $(SHIMFLAGS) $(DEPFLAGS) -c $< -o $@

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
LIBC_GEN = malloc ecvt ieeefp errlst perror memops \
           ctype atoi atol abs max min sgn gcd lcm \
           index rindex strrchr strdup strtok strcatn strcmpn strcpyn \
           calloc getenv qsort swab mktemp abort rand getopt stty \
           execvp exec getwd ftw valloc tell iread l3tol ltol3 nlist \
           opendir readdir closedir seekdir telldir \
           ctime timezone ttyname cttyname getlogin ttyslot
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
          $(LIBCSRC)/stdio/doscan.c $(LIBCSRC)/stdio/scanf.c \
          $(LIBCSRC)/stdio/popen.c $(LIBCSRC)/stdio/system.c
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
$(BUILD)/libc/libv8c.a: $(LIBC_OBJ)
	@ar rcs $@ $(LIBC_OBJ)
	@echo "built $@"

# Compiled by v8cc itself -- this is the point.  V8ROOT has to be set for the
# driver to find its passes.
V8CCRUN = V8ROOT=$(ROOTFS) $(ROOTFS)/bin/cc -I$(LIBCSRC)/stdio

$(BUILD)/libc/gen/%.o: $(LIBCSRC)/gen/%.c $(A64BUILD)/v8ccom $(BUILD)/cpp/cpp | rootfs
	@mkdir -p $(BUILD)/libc/gen
	$(V8CCRUN) -c -o $@ $<

$(BUILD)/libc/gen/%.o: $(LIBCSRC)/gen/%.C $(A64BUILD)/v8ccom $(BUILD)/cpp/cpp | rootfs
	@mkdir -p $(BUILD)/libc/gen
	cp $< $(BUILD)/libc/gen/$*.c
	$(V8CCRUN) -c -o $@ $(BUILD)/libc/gen/$*.c

$(BUILD)/libc/stdio/%.o: $(LIBCSRC)/stdio/%.c $(A64BUILD)/v8ccom $(BUILD)/cpp/cpp | rootfs
	@mkdir -p $(BUILD)/libc/stdio
	$(V8CCRUN) -c -o $@ $<

# ---------------------------------------------------------------------------
# v8cc -- V8's cc(1) driver, retargeted.
#
# Paths resolve from $V8ROOT; the assembler and link editor are the host's,
# reached through clang.  Everything else -- the flag surface, the temp-file
# dance, the order of the passes -- is V8's own driver.
# ---------------------------------------------------------------------------
V8CC_INC =

v8cc: $(BUILD)/cc/v8cc
$(BUILD)/cc/v8cc: $(SRC)/cmd/cc.c
	@mkdir -p $(BUILD)/cc
	$(HOSTCC) $(KRFLAGS) $(V8CC_INC) -o $@ $< $(STAGE0_COMPAT)
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

$(BUILD)/sh/%.o: $(SHSRC)/%.c $(A64BUILD)/v8ccom $(BUILD)/cpp/cpp | rootfs
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

$(BUILD)/nroff/%.o: $(TROFFSRC)/%.c $(A64BUILD)/v8ccom $(BUILD)/cpp/cpp | rootfs
	@mkdir -p $(BUILD)/nroff
	$(V8CCRUN) -DINCORE -DSMALLER -DNROFF -I$(TROFFSRC) -c -o $@ $<
$(BUILD)/troff/%.o: $(TROFFSRC)/%.c $(A64BUILD)/v8ccom $(BUILD)/cpp/cpp | rootfs
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

$(BUILD)/tbl/%.o: $(TBLSRC)/%.c $(A64BUILD)/v8ccom $(BUILD)/cpp/cpp | rootfs
	@mkdir -p $(BUILD)/tbl
	$(V8CCRUN) -I$(TBLSRC) -c -o $@ $<
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
$(BUILD)/yacc/%.o: $(YACCSRC)/%.c $(A64BUILD)/v8ccom $(BUILD)/cpp/cpp | rootfs
	@mkdir -p $(BUILD)/yacc
	$(V8CCRUN) -I$(YACCSRC) -c -o $@ $<

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

$(BUILD)/lex/y.tab.c: $(LEXSRC)/parser.y v8yacc | rootfs
	@mkdir -p $(BUILD)/lex
	cd $(BUILD)/lex && V8ROOT=$(ROOTFS) $(BUILD)/yacc/yacc $(LEXSRC)/parser.y

$(BUILD)/lex/y.tab.o: $(BUILD)/lex/y.tab.c $(A64BUILD)/v8ccom | rootfs
	$(V8CCRUN) -I$(LEXSRC) -c -o $@ $(BUILD)/lex/y.tab.c
$(BUILD)/lex/%.o: $(LEXSRC)/%.c $(A64BUILD)/v8ccom $(BUILD)/cpp/cpp | rootfs
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
# The V8 link line.  Archive order matters: libv8stubs.a is one object per
# syscall so a program defining its own rmdir still wins, and -lSystem is last
# because the shim is the one place the two worlds are meant to meet.
V8DEPS = $(BUILD)/crt0.o $(BUILD)/libc/libv8c.a $(BUILD)/v8sys/libv8stubs.a \
         $(BUILD)/v8sys/libv8sys.a
V8LIBS = $(BUILD)/libc/libv8c.a $(BUILD)/v8sys/libv8stubs.a \
         $(BUILD)/v8sys/libv8sys.a -lSystem
V8LDFLAGS = -nostdlib -e _v8start

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

$(BUILD)/pic/y.tab.c $(BUILD)/pic/pic.ydef: $(PICSRC)/picy.y v8yacc | rootfs
	@mkdir -p $(BUILD)/pic
	cd $(BUILD)/pic && V8ROOT=$(ROOTFS) $(BUILD)/yacc/yacc -d $(PICSRC)/picy.y
	@cp $(BUILD)/pic/y.tab.h $(BUILD)/pic/pic.ydef

$(BUILD)/pic/lex.yy.c: $(PICSRC)/picl.l v8lex $(BUILD)/pic/pic.ydef | rootfs
	@mkdir -p $(BUILD)/pic
	cd $(BUILD)/pic && V8ROOT=$(ROOTFS) $(BUILD)/lex/lex $(PICSRC)/picl.l

$(BUILD)/pic/picy.o: $(BUILD)/pic/y.tab.c $(A64BUILD)/v8ccom | rootfs
	$(V8CCRUN) -I$(BUILD)/pic -I$(PICSRC) -c -o $@ $(BUILD)/pic/y.tab.c
$(BUILD)/pic/picl.o: $(BUILD)/pic/lex.yy.c $(A64BUILD)/v8ccom | rootfs
	$(V8CCRUN) -I$(BUILD)/pic -I$(PICSRC) -c -o $@ $(BUILD)/pic/lex.yy.c
$(BUILD)/pic/%.o: $(PICSRC)/%.c $(BUILD)/pic/pic.ydef $(A64BUILD)/v8ccom | rootfs
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

$(BUILD)/spell/%.o: $(SPELLSRC)/%.c $(SPELLSRC)/hash.h $(A64BUILD)/v8ccom | rootfs
	@mkdir -p $(BUILD)/spell
	$(V8CCRUN) -I$(SPELLSRC) -c -o $@ $<

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
$(ROOTFS)/usr/lib/refer/%: $(BUILD)/refer/% | rootfs
	@mkdir -p $(ROOTFS)/usr/lib/refer
	@cp $< $@

$(BUILD)/refer/%.o: $(REFERSRC)/%.c $(A64BUILD)/v8ccom | rootfs
	@mkdir -p $(BUILD)/refer
	$(V8CCRUN) -I$(REFERSRC) -c -o $@ $<

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
# include for the token numbers.
$(BUILD)/eqn/y.tab.c $(BUILD)/eqn/e.def: $(EQNSRC)/eqn.y v8yacc
	@mkdir -p $(BUILD)/eqn
	cd $(BUILD)/eqn && V8ROOT=$(ROOTFS) $(BUILD)/yacc/yacc -d $(EQNSRC)/eqn.y
	@cp $(BUILD)/eqn/y.tab.h $(BUILD)/eqn/e.def

$(BUILD)/eqn/eqn.o: $(BUILD)/eqn/y.tab.c
	$(V8CCRUN) -I$(BUILD)/eqn -I$(EQNSRC) -c -o $@ $(BUILD)/eqn/y.tab.c
$(BUILD)/eqn/%.o: $(EQNSRC)/%.c $(BUILD)/eqn/e.def $(A64BUILD)/v8ccom | rootfs
	@mkdir -p $(BUILD)/eqn
	$(V8CCRUN) -I$(BUILD)/eqn -I$(EQNSRC) -c -o $@ $<
$(EQN_OBJ): $(EQNSRC)/e.h

# ---------------------------------------------------------------------------
# rootfs -- the V8-shaped tree v8cc runs out of.  $V8ROOT points here.
# ---------------------------------------------------------------------------

rootfs: cpp v8ccom v8cc
	@mkdir -p $(ROOTFS)/lib $(ROOTFS)/bin $(ROOTFS)/usr/include
	@cp $(BUILD)/cpp/cpp $(ROOTFS)/lib/cpp
	@cp $(A64BUILD)/v8ccom $(ROOTFS)/lib/ccom
	@cp $(BUILD)/cc/v8cc $(ROOTFS)/bin/cc
	@cp -R $(ROOT)third_party/Research-Unix-v8/v8/usr/include/. $(ROOTFS)/usr/include/
	@# Patched headers go on LAST, over the pristine ones.  Only headers the
	@# port genuinely had to change live in src/include -- so far setjmp.h,
	@# whose jmp_buf is 40 VAX bytes and cannot hold AAPCS64's callee-saved
	@# set.  src/include/PROVENANCE records the upstream hash of each, so the
	@# diff against pristine V8 stays reconstructible.
	@cp -R $(SRC)/include/. $(ROOTFS)/usr/include/
	@# The jerq headers, which ls(1) and mc(1) reach for by absolute path.
	@# Unchanged upstream files; only their location moves.
	@cp $(ROOT)third_party/Research-Unix-v8/jerq/include/jioctl.h $(ROOTFS)/usr/include/
	@# The data files V8 programs open by absolute path.  The shim resolves
	@# /usr/lib/... inside $V8ROOT -- see rootpath() in shim/v8sys/syscall.c.
	@mkdir -p $(ROOTFS)/usr/lib/term
	@cp $(SRC)/cmd/troff/term/tab.* $(ROOTFS)/usr/lib/term/ 2>/dev/null || true
	@# yaccpar, which V8's yacc opens as /usr/lib/yaccpar.
	@cp $(SRC)/cmd/yacc/yaccpar $(ROOTFS)/usr/lib/ 2>/dev/null || true
	@# ncform, the skeleton lex copies out after its tables.
	@mkdir -p $(ROOTFS)/usr/lib/lex
	@cp $(SRC)/cmd/lex/ncform $(ROOTFS)/usr/lib/lex/ 2>/dev/null || true
	@$(MAKE) --no-print-directory devtables

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
ROOTFS_LIBS = $(ROOTFS)/lib/crt0.o $(ROOTFS)/lib/libv8c.a \
              $(ROOTFS)/lib/libv8stubs.a $(ROOTFS)/lib/libv8sys.a

rootfs-libs: $(ROOTFS_LIBS)
$(ROOTFS)/lib/crt0.o: $(BUILD)/crt0.o | rootfs
	@cp $< $@
$(ROOTFS)/lib/libv8c.a: $(BUILD)/libc/libv8c.a | rootfs
	@cp $< $@
$(ROOTFS)/lib/libv8stubs.a: $(BUILD)/v8sys/libv8stubs.a | rootfs
	@cp $< $@
$(ROOTFS)/lib/libv8sys.a: $(BUILD)/v8sys/libv8sys.a | rootfs
	@cp $< $@

# troff's device tables.  makedev compiles the plain-text description in
# dev202/ into the binary DESC.out and per-font .out files troff opens at
# startup; the original makefile does the same.  It is a BUILD tool, like yacc,
# so it is built with the host compiler and never ends up in the rootfs.
devtables: $(BUILD)/troff/dev202/DESC.out
$(BUILD)/troff/dev202/DESC.out: $(TROFFSRC)/makedev.c $(wildcard $(TROFFSRC)/dev202/*)
	@mkdir -p $(BUILD)/troff/dev202
	@$(HOSTCC) -std=gnu89 -fcommon -w -I$(TROFFSRC) -o $(BUILD)/troff/makedev \
	    $(TROFFSRC)/makedev.c
	@cp -R $(TROFFSRC)/dev202/. $(BUILD)/troff/dev202/
	@cd $(BUILD)/troff/dev202 && ../makedev DESC > /dev/null
	@mkdir -p $(ROOTFS)/usr/lib/font/dev202
	@cp $(BUILD)/troff/dev202/*.out $(ROOTFS)/usr/lib/font/dev202/
	@echo "rootfs ready: V8ROOT=$(ROOTFS) $(ROOTFS)/bin/cc"

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
# dependency on every header they could include instead -- overbuilds sometimes,
# never underbuilds, and libc is small enough that it does not matter.
# ---------------------------------------------------------------------------
V8HDRS := $(wildcard $(LIBCSRC)/stdio/*.h) \
          $(wildcard $(ROOT)third_party/Research-Unix-v8/v8/usr/include/*.h) \
          $(wildcard $(ROOT)third_party/Research-Unix-v8/v8/usr/include/sys/*.h)
$(LIBC_OBJ): $(V8HDRS)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
