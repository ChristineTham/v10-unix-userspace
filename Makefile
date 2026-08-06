# The repository, which is not one port but a series of them.
#
# V8 is a rung, not the destination -- this repo is named for V10 -- and V9 and
# V10 each get their own sibling of v8/.  What lives HERE is what is not
# per-release: third_party/ (the vendored upstreams, versioned inside
# themselves), tools/, and the prose.
#
# This file dispatches; it builds nothing.  Each release's Makefile derives its
# own root from where it sits (`ROOT := $(dir $(abspath $(lastword
# $(MAKEFILE_LIST))))`), so it needed no change when it moved down a level, and
# a new release needs no change here beyond adding its name to RELEASES.
#
# WHY A DISPATCHER RATHER THAN A cd IN THE INSTRUCTIONS.  `make -j8` and
# `make test` at the repo root are what CI runs, what CLAUDE.md documents, and
# what a decade of muscle memory expects.  Moving the tree should not move the
# command; that is the whole job of this file.

RELEASES = v8

# The one this repo builds by default.  V9 and V10 are ports in progress, and
# `all' meaning "every release" would make a broken V9 break V8's build --
# which is exactly the coupling the split exists to prevent.
CURRENT  = v8

.PHONY: all test clean distclean install $(RELEASES) \
        $(addsuffix -test,$(RELEASES)) $(addsuffix -clean,$(RELEASES))

all: $(CURRENT)

# `$(MAKE) -C' rather than a recipe of its own: the sub-make inherits -j and
# the jobserver, so `make -j8' at the root still parallelises the real build.
$(RELEASES):
	@$(MAKE) -C $@

$(addsuffix -test,$(RELEASES)): %-test:
	@$(MAKE) -C $* test

$(addsuffix -clean,$(RELEASES)): %-clean:
	@$(MAKE) -C $* clean

test: $(CURRENT)-test
clean: $(CURRENT)-clean

# distclean and install belong to a release, not to the repository -- an
# installed world is one release's /bin, /lib and compiler, and two of them
# cannot share a prefix.  Forwarded to CURRENT with its arguments intact so
# `make install PREFIX=...' keeps working from here.
distclean:
	@$(MAKE) -C $(CURRENT) distclean

install:
	@$(MAKE) -C $(CURRENT) install
