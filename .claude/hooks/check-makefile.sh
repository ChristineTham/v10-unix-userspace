#!/bin/sh
# PostToolUse: after any Makefile edit, check the two mistakes that have
# actually caused silent staleness in this repo.
#
# WHY THE FIRST ONE EXISTS.  Make expands a variable in a target name OR in a
# prerequisite at the moment it READS the rule, so a variable defined further
# down the file expands to nothing right there.  Nothing fails.  The build still
# succeeds.  The dependency simply is not there.
#
# Three times now:
#   $(ROOTFS) in a target name    -> the rule built /lib/... at the real root
#   $(A64BUILD) in a test prereq  -> test-v8ccom depended on `/v8ccom`; that one
#                                    at least failed loudly
#   $(V8DEPS) in the /bin rules   -> 38 binaries and make itself with NO
#                                    dependency on the libraries.  They linked
#                                    correctly, because a RECIPE expands when it
#                                    runs, and never relinked when the shim
#                                    changed -- leaving 38 stale binaries and a
#                                    jail that silently was not one.
#
# ~60ms, cheap enough to pay on every Makefile edit rather than finding out at
# the next full test run.  Exit 2 feeds stderr back to Claude.

path=$(jq -r '.tool_input.file_path // empty' 2>/dev/null)
[ -n "$path" ] || exit 0

case "$path" in
*Makefile|*.mk) ;;
*) exit 0 ;;
esac

cd "${CLAUDE_PROJECT_DIR:-.}" || exit 0
[ -f Makefile ] || exit 0

warnings=$(make -n --warn-undefined-variables 2>&1 |
           grep 'warning: undefined variable' | sort -u)

if [ -n "$warnings" ]; then
	cat >&2 <<EOF
Makefile: undefined variables.

$warnings

A variable used above its own definition expands to NOTHING in a target name or
a prerequisite, because make expands those when it reads the rule.  The build
will still succeed; the dependency just will not exist.  Three separate
staleness bugs here came from this -- see the comment at the top of the
Makefile.

Fix: hoist the definition above its first use.
EOF
	exit 2
fi

# Second check: a multi-target rule THAT CARRIES A RECIPE.  make 3.81 reads
# `a b: dep` as two rules sharing one recipe, so the recipe may run twice and
# races under -j; grouped targets (&:) need make 4.3.  pic's
# `y.tab.c pic.ydef:` was exactly this.
#
# Two things must be excluded or the check is noise -- and noise is worse than
# no check, because it teaches you to skip the output:
#
#   * `$(NROFF_OBJ) $(TROFF_OBJ): $(wildcard ...)` -- many targets, NO recipe.
#     Not a rule; a dependency declaration, and the way this Makefile attaches
#     shared headers to a whole object list.  Legitimate and common here.
#   * `$(BUILD)/v8sys/stub/$(word 1,$(subst :, ,$(1))).o:` -- ONE target whose
#     name contains spaces inside $(subst :, ,...).  Text, not separators.
#
# So: blank out $(...) and ${...} first, then count targets, then require that
# the next line is a recipe.
multi=$(python3 - <<'PY'
import re, sys
try:
	lines = open('Makefile').read().split('\n')
except OSError:
	sys.exit(0)
# Collapse innermost $(...) first and repeat, so ARBITRARY nesting flattens.
# One pass is not enough: $(word 1,$(subst :, ,$(1))) is three deep, and a
# single-level pattern leaves the outer spaces behind and reports one target as
# two.  That was this check's first false positive.
inner = re.compile(r'\$\([^()]*\)|\$\{[^{}]*\}')
def flatten(s):
	prev = None
	while prev != s:
		prev = s
		s = inner.sub('V', s)
	return s

hits = []
for i, line in enumerate(lines):
	if not line or line[0] in ' \t#':
		continue
	flat = flatten(line)
	if ':' not in flat or ':=' in flat:
		continue
	head = flat.split(':', 1)[0]
	if head.startswith('.') or len(head.split()) < 2:
		continue
	nxt = lines[i + 1] if i + 1 < len(lines) else ''
	if nxt.startswith('\t'):
		hits.append('%d:%s' % (i + 1, line))
print('\n'.join(hits[:3]))
PY
)

if [ -n "$multi" ]; then
	cat >&2 <<EOF
Makefile: multi-target rule with a recipe.

$multi

GNU make 3.81 -- what macOS ships and what this builds under -- reads
\`a b: dep\` as TWO rules sharing one recipe, not a grouped target, so the
recipe is eligible to run twice and races under -j.  Grouped targets (&:) need
make 4.3.

Split into one rule per target, deriving the second from the first.  The pic
y.tab.c / pic.ydef rules are the worked example.
EOF
	exit 2
fi

exit 0
