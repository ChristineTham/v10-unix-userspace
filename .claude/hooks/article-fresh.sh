#!/bin/sh
# PreToolUse (Bash): refuse `git commit' that changes the port without also
# writing up what changed in ARTICLE.md.
#
# WHY A HOOK AND NOT A SENTENCE.  The instruction to keep ARTICLE.md current
# was given ELEVEN times in conversation -- once to create it, twice as "are
# you updating it as you go along?", and eight times as "did you update
# ARTICLE.md?" -- and it lived in no file, so it was not in CLAUDE.md, not in
# PLAN.md, not in .claude/, and a grep for it came back empty.  Every other
# invariant here has a guard; this was the only artefact with none.
#
# AND "MUST INCLUDE ARTICLE.md" WOULD NOT HAVE CAUGHT IT, which is the whole
# reason this hook is shaped the way it is.  Measured from the log: the last
# four commits touching that file changed NOTHING BUT A NUMBER -- +1/-1 each,
# the test count -- while two entire steps went unwritten.  Each one would have
# satisfied a rule that only asked whether the file was in the commit.  A guard
# that a `sed -i' on one digit can satisfy is the same shape as a green suite
# that skipped its case.
#
# So it asks for a SUBSTANTIVE change, and defines that by measurement rather
# than by taste: the real update in that history was +116 lines, the four
# hollow ones were +1.  $ARTICLE_MINLINES is the line between them.
#
# IT READS THE INDEX AS IT IS *BEFORE* THE COMMAND RUNS, which is what a
# PreToolUse hook can see, and that produces one false positive worth knowing
# about: `git add -A && git commit ...' in a SINGLE invocation is inspected
# before the `git add' has happened, so a freshly written ARTICLE.md is not in
# the index yet and this blocks with a message saying it was not updated --
# which is false and reads as a bug in the write-up rather than in the
# spelling.  Stage in one command and commit in the next.
#
# Deliberately NOT fixed by also consulting the working tree.  That would let
# `git add v8/src/foo.c && git commit' pass while a modified ARTICLE.md sits
# unstaged and never enters the commit at all -- exactly the state this hook
# exists to catch.  The `-a' case below is the one place the working tree is
# consulted, and only because `git commit -a' really will stage it.
#
# WHAT IT DOES NOT BLOCK, because a hook that stops the everyday commit is off
# within the hour:
#   - a commit that touches no port source at all (docs, tests, tooling)
#   - a commit that is only ARTICLE.md
#   - a revert or a merge
#   - anything, when ARTICLE_ANYWAY=1 is given -- the same escape ci-green.sh
#     offers, and the way a genuine one-line correction to the article gets in
#
# Exit 2 blocks the call and shows stderr to Claude.

ARTICLE_MINLINES=${ARTICLE_MINLINES:-6}

# Read stdin ONCE.  Two jq calls do not both work -- the first consumes the
# pipe and the second silently reads nothing.  That bug shipped in v8-make.sh
# and made it pass everything; see the note there.
payload=$(cat)
cmd=$(printf '%s' "$payload" | jq -r '.tool_input.command // empty' 2>/dev/null)
[ -n "$cmd" ] || exit 0

# Cheap bail-out first: this runs on EVERY Bash call.
case "$cmd" in
*"git commit"*) ;;
*) exit 0 ;;
esac

# The explicit override, checked before any work.
case "$cmd" in
*ARTICLE_ANYWAY=1*) exit 0 ;;
esac

# A revert or a merge is not a change to write up.
case "$cmd" in
*"git commit"*--amend*) ;;   # an amend still has to say something
esac
case "$cmd" in
*revert*|*merge*) exit 0 ;;
esac

root=${CLAUDE_PROJECT_DIR:-.}
cd "$root" 2>/dev/null || exit 0
[ -d .git ] || exit 0

# WHAT IS ACTUALLY STAGED, not what is modified.  `git commit -a' stages at
# commit time, so both indexes have to be considered: --cached for an explicit
# `git add', and a plain diff for the -a case.
#
# The -a test is a plain `case' and not one nested inside a $( ), which is how
# the first draft was written and which bash rejects outright -- and a hook
# with a syntax error does not fail open, it fails LOUD on every Bash call in
# the session.  That is the better direction to fail in, and it is still worth
# not doing.
allflag=0
case "$cmd" in
*" -a"*|*" -am"*|*" -ma"*) allflag=1 ;;
esac

staged=$(git diff --cached --name-only 2>/dev/null)
if [ "$allflag" = 1 ]; then
	staged="$staged
$(git diff --name-only 2>/dev/null)"
fi
staged=$(printf '%s\n' "$staged" | grep -v '^$' | sort -u)
[ -n "$staged" ] || exit 0

# Does this commit touch the PORT?  Prose, tests and tooling do not need a
# write-up of their own -- the article is about what the system does.
touches_port=$(printf '%s\n' "$staged" | grep -E '^(v8/(src|shim|compiler)/|v8/Makefile$|Makefile$|tools/)' | head -1)
[ -n "$touches_port" ] || exit 0

# Is ARTICLE.md in it at all?
if ! printf '%s\n' "$staged" | grep -qx 'ARTICLE.md'; then
	cat >&2 <<EOF
BLOCKED: this commit changes the port but does not update ARTICLE.md.

  changed: $(printf '%s\n' "$staged" | grep -E '^(v8/(src|shim|compiler)/|v8/Makefile$|Makefile$|tools/)' | head -4 | tr '\n' ' ')

ARTICLE.md is the write-up, and it is the one artefact here with no other
guard.  It has gone stale before by exactly this route: four commits in a row
touched it and changed only the test count, while two whole steps went
unwritten.

Write the finding up, then commit.  If this one genuinely has nothing to say:

  ARTICLE_ANYWAY=1 git commit ...
EOF
	exit 2
fi

# ...and is it a REAL update, or a number moved?
lines=$(git diff --cached --numstat -- ARTICLE.md 2>/dev/null | awk '{a+=$1} END {print a+0}')
if [ "$allflag" = 1 ]; then
	more=$(git diff --numstat -- ARTICLE.md 2>/dev/null | awk '{a+=$1} END {print a+0}')
	lines=$((lines + more))
fi
if [ "${lines:-0}" -lt "$ARTICLE_MINLINES" ]; then
	cat >&2 <<EOF
BLOCKED: ARTICLE.md is in this commit but only $lines line(s) were added.

That is the shape the guard exists for.  The four commits that let this file go
stale each touched it and moved the test count and nothing else -- which looks
like compliance and says nothing.  A real entry in that history was +116.

Write up what this change does and why, then commit.  If the $lines-line change
IS the whole honest update -- a corrected number, a fixed citation:

  ARTICLE_ANYWAY=1 git commit ...
EOF
	exit 2
fi

exit 0
