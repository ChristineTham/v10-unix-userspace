# Research Unix V8 -> macOS/ARM64.  See PLAN.md.
#
# Stage 0 builds the V8 toolchain with the *host* compiler, just far enough to
# get a working v8cc.  Nothing built here is authentic-by-construction; it is
# scaffolding.  Once the ARM64 backend lands (Phase 1b) the world is rebuilt by
# v8cc itself, and the stage-0 binaries are only kept to prove the fixpoint.

ROOT    := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
BUILD   := $(ROOT)build/stage0
SRC     := $(ROOT)src

HOSTCC  ?= clang

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

# stubs.c is EXCLUDED here on purpose.  It defines open/read/write with their V8
# names, which collide with the host functions the rest of the shim calls -- see
# shim/NOTES.md.  It is compiled only into the copy the V8 world links against
# with -nostartfiles, never into anything that also uses host libc.
SHIM_SRC = $(filter-out $(ROOT)shim/v8sys/stubs.c,$(wildcard $(ROOT)shim/v8sys/*.c))

.PHONY: all stage0 cpp ccom-pass1 ccom-vax v8ccom v8cc rootfs libv8sys libv8c crt0 test test-cpp test-v8ccom test-v8cc test-v8sys test-freestanding test-libv8c test-wavea clean distclean
all: stage0
stage0: cpp v8ccom v8cc libv8sys crt0 rootfs

test: test-cpp test-v8ccom test-v8cc test-v8sys test-freestanding test-libv8c test-wavea
test-cpp: cpp
	@$(ROOT)tests/cpp/run.sh $(BUILD)/cpp/cpp
test-v8ccom: v8ccom
	@$(ROOT)tests/v8ccom/run.sh $(A64BUILD)/v8ccom
test-v8cc: rootfs
	@$(ROOT)tests/v8cc/run.sh
test-v8sys: $(BUILD)/v8sys/test
	@$(BUILD)/v8sys/test
test-freestanding: rootfs libv8sys crt0
	@$(ROOT)tests/freestanding/run.sh
test-libv8c: rootfs libv8sys crt0 libv8c
	@$(ROOT)tests/libv8c/run.sh
test-wavea: rootfs libv8sys crt0 libv8c
	@$(ROOT)tests/wavea/run.sh

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
	$(HOSTCC) $(KRFLAGS) $(CCOM_INC) -c $(ROOT)compiler/ccom-arm64/gencode.c \
		-o $(BUILD)/ccom/gencode-arm64.o
	$(HOSTCC) $(KRFLAGS) -o $@ $(CCOM_P1) $(BUILD)/ccom/gencode-arm64.o
	@echo "built $@ (pass 1 + ARM64 backend stub)"

# The complete VAX compiler.  Expected to fail on gencode.c/genaux.c as above;
# kept as a target so the failure stays visible rather than forgotten.
ccom-vax: $(CCOM_OBJ)
	$(HOSTCC) $(KRFLAGS) -o $(BUILD)/ccom/ccom-vax $(CCOM_OBJ)

$(BUILD)/ccom/%.o: $(CCOM_M)/%.c
	@mkdir -p $(BUILD)/ccom
	$(HOSTCC) $(KRFLAGS) $(CCOM_INC) -DVAX -c $< -o $@

$(BUILD)/ccom/%.o: $(CCOM_V)/%.c
	@mkdir -p $(BUILD)/ccom
	$(HOSTCC) $(KRFLAGS) $(CCOM_INC) -DVAX -c $< -o $@

# cgram.c is the checked-in yacc output, so the 1978 grammar needs no yacc run.
$(BUILD)/ccom/cgram.o: $(CCOM_M)/cgram.c
	@mkdir -p $(BUILD)/ccom
	cp $(CCOM_V)/y.debug.sv $(BUILD)/ccom/y.debug
	$(HOSTCC) $(KRFLAGS) $(CCOM_INC) -I$(BUILD)/ccom -DVAX -DYYDEBUG -c $< -o $@

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
	$(HOSTCC) $(KRFLAGS) $(A64INC) -c $< -o $@

$(A64BUILD)/%.o: $(A64)/%.c
	@mkdir -p $(A64BUILD)
	$(HOSTCC) $(KRFLAGS) $(A64INC) -c $< -o $@

$(A64BUILD)/cgram.o: $(CCOM_M)/cgram.c
	@mkdir -p $(A64BUILD)
	cp $(CCOM_V)/y.debug.sv $(A64BUILD)/y.debug
	$(HOSTCC) $(KRFLAGS) $(A64INC) -I$(A64BUILD) -DYYDEBUG -c $< -o $@

# ---------------------------------------------------------------------------
# libv8sys -- the shim standing in for the VAX kernel.  Modern C, clang-built:
# it is the seam, not authentic V8 code.
# ---------------------------------------------------------------------------
SHIM_OBJ = $(patsubst $(ROOT)shim/v8sys/%.c,$(BUILD)/v8sys/%.o,$(SHIM_SRC))

# crt0 and the V8-named stub layer: the two pieces a freestanding V8 program
# needs on top of the shim.  stubs.c is built here, and ONLY here, because its
# open/read/write collide with the host's -- see shim/NOTES.md.
crt0: $(BUILD)/crt0.o $(BUILD)/v8sys/stubs-freestanding.o
$(BUILD)/crt0.o: $(ROOT)compiler/crt0.s
	@mkdir -p $(BUILD)
	$(HOSTCC) -c $< -o $@
$(BUILD)/v8sys/stubs-freestanding.o: $(ROOT)shim/v8sys/stubs.c
	@mkdir -p $(BUILD)/v8sys
	$(HOSTCC) -std=gnu89 -fcommon -w -fno-stack-protector -c $< -o $@

libv8sys: $(BUILD)/v8sys/libv8sys.a
$(BUILD)/v8sys/libv8sys.a: $(SHIM_OBJ)
	ar rcs $@ $(SHIM_OBJ)
	@echo "built $@"

# -fno-stack-protector and -fno-stack-check: both emit calls to libc helpers
# (___stack_chk_fail, ___chkstk_darwin), and the whole point of the shim is that
# it names no libc symbol -- see shim/v8sys/rawsys.h.
SHIMFLAGS = -std=gnu99 -Wall -Wno-unused-function \
            -fno-stack-protector -fno-stack-check

$(BUILD)/v8sys/%.o: $(ROOT)shim/v8sys/%.c
	@mkdir -p $(BUILD)/v8sys
	$(HOSTCC) $(SHIMFLAGS) -c $< -o $@

# ---------------------------------------------------------------------------
# libv8c -- V8's own libc, compiled by v8cc, on top of the shim.
#
# This is authentic V8 source with three kinds of exception, each marked in the
# file that replaces it: the VAX assembly leaf routines (string ops, doprnt,
# the float primitives), which had to be rewritten; the syscall stubs, which
# are the shim; and crt0.
# ---------------------------------------------------------------------------
LIBCSRC = $(SRC)/libc
LIBC_C  = $(LIBCSRC)/gen/malloc.c $(LIBCSRC)/gen/ecvt.c $(LIBCSRC)/gen/ieeefp.c \
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
          $(LIBCSRC)/stdio/rdwr.c $(LIBCSRC)/stdio/sprintf.c
# The string routines ship as .C -- portable references beside the VAX assembly
# that V8 actually built.  They are what a machine without those instructions
# was meant to use.
LIBC_STR = strlen strcpy strcmp strcat strncpy strncmp strchr

LIBC_OBJ = $(patsubst $(LIBCSRC)/%.c,$(BUILD)/libc/%.o,$(LIBC_C)) \
           $(patsubst %,$(BUILD)/libc/gen/%.o,$(LIBC_STR))

libv8c: $(BUILD)/libc/libv8c.a
$(BUILD)/libc/libv8c.a: $(LIBC_OBJ)
	ar rcs $@ $(LIBC_OBJ)
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
# rootfs -- the V8-shaped tree v8cc runs out of.  $V8ROOT points here.
# ---------------------------------------------------------------------------
ROOTFS = $(ROOT)rootfs

rootfs: cpp v8ccom v8cc
	@mkdir -p $(ROOTFS)/lib $(ROOTFS)/bin $(ROOTFS)/usr/include
	@cp $(BUILD)/cpp/cpp $(ROOTFS)/lib/cpp
	@cp $(A64BUILD)/v8ccom $(ROOTFS)/lib/ccom
	@cp $(BUILD)/cc/v8cc $(ROOTFS)/bin/cc
	@cp -R $(ROOT)third_party/Research-Unix-v8/v8/usr/include/. $(ROOTFS)/usr/include/
	@echo "rootfs ready: V8ROOT=$(ROOTFS) $(ROOTFS)/bin/cc"

clean:
	rm -rf $(BUILD) $(ROOTFS)

distclean: clean
	rm -rf $(ROOT)build
