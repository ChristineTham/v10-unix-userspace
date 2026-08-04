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

.PHONY: all stage0 cpp ccom-pass1 ccom-vax v8ccom v8cc rootfs test test-cpp test-v8ccom test-v8cc clean distclean
all: stage0
stage0: cpp v8ccom v8cc rootfs

test: test-cpp test-v8ccom test-v8cc
test-cpp: cpp
	@$(ROOT)tests/cpp/run.sh $(BUILD)/cpp/cpp
test-v8ccom: v8ccom
	@$(ROOT)tests/v8ccom/run.sh $(A64BUILD)/v8ccom
test-v8cc: rootfs
	@$(ROOT)tests/v8cc/run.sh

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
