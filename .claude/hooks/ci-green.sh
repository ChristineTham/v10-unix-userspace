#!/bin/sh
# PreToolUse (Bash): refuse `git push' when the last one is still red, or when
# the local suite has not been run since the tree changed.
#
# WHAT THIS CANNOT DO, said first because the obvious reading of "CI must be
# green before pushing" is circular: CI runs ON the push, so the run for the
# commit you are about to send does not exist yet and cannot be consulted.  What
# is checkable is the two things that actually produce a mailbox full of red:
#
#   1. PILING ONTO A RED BUILD.  Push, fail, push a fix, fail differently, push
#      again -- every one is another notification, and every one after the first
#      was avoidable by fixing before pushing.  Nine of the ten failure mails
#      from the session that prompted this hook were this shape.
#   2. PUSHING UNTESTED.  `make test' is the only local proxy for CI, and a
#      stamp is the only way to know it was run since the last edit.
#
# Neither catches an environment difference between this Mac and the runner --
# nothing local can, and that is what CLAUDE.md's "a test that asserts a
# property of the machine" note is about.  This hook narrows the window; it does
# not close it.
#
# FAILS OPEN on anything it cannot determine -- no gh, not logged in, offline,
# no run found, a branch that has never been pushed.  A push gate that blocks
# when GitHub is unreachable gets switched off within the hour, and then it is
# not protecting anything either.  It fails CLOSED only on a definite red.
#
# Override, for the case this hook is wrong about or when the fix itself has to
# go out:  PUSH_ANYWAY=1 git push
#
# Exit 2 blocks the call and shows stderr to Claude.

# Read stdin ONCE.  Two jq calls do not both work -- the first consumes the pipe
# and the second silently reads nothing.  That bug shipped in v8-make.sh and
# made it pass everything; see the note there.
payload=$(cat)
cmd=$(printf '%s' "$payload" | jq -r '.tool_input.command // empty' 2>/dev/null)
[ -n "$cmd" ] || exit 0

# Cheap bail-out first: this runs on EVERY Bash call.
case "$cmd" in
*push*) ;;
*) exit 0 ;;
esac

# The explicit override, checked before any work.
case "$cmd" in
*PUSH_ANYWAY=1*) exit 0 ;;
esac

root=${CLAUDE_PROJECT_DIR:-.}

# Is there a REAL git push in there?  shlex rather than a substring match, so
# that `git commit -m "explain git push"' is not mistaken for one: quoting keeps
# the message a single token, which never compares equal to `push'.  --dry-run
# contacts the server but starts no workflow, so it is not a push for us.
python3 - "$cmd" <<'PY' || exit 0
import os, re, shlex, sys

cmd = sys.argv[1]
TAKESARG = ('-C', '-c', '--git-dir', '--work-tree', '--namespace', '--exec-path',
            '--super-prefix')

for seg in re.split(r'(?:\n|;|\|\||&&|\|)+', cmd):
    try:
        words = shlex.split(seg)
    except ValueError:
        continue
    # Step over leading VAR=value assignments.
    while words and re.match(r'^[A-Za-z_][A-Za-z_0-9]*=', words[0]):
        words.pop(0)
    if not words or os.path.basename(words[0]) != 'git':
        continue
    args = words[1:]
    i = 0
    while i < len(args):                     # skip git's own options
        a = args[i]
        if a in TAKESARG:
            i += 2; continue
        if a.startswith('-'):
            i += 1; continue
        break
    if i >= len(args) or args[i] != 'push':
        continue
    rest = args[i + 1:]
    if '--dry-run' in rest or '-n' in rest:  # talks to the server, runs no CI
        continue
    sys.exit(0)                              # a real push
sys.exit(1)
PY

fail() { printf '%s\n' "$1" >&2; exit 2; }

# ---------------------------------------------------------------- 1. tested?
#
# `make test' writes this stamp, and only after every suite has passed -- make
# runs a target's recipe only when all its prerequisites succeeded, so the
# stamp's existence IS the green run.  Staleness is any source file newer than
# it; build/ and rootfs/ are excluded because they are outputs.
stamp=$root/build/stage0/.tests-passed
if [ ! -f "$stamp" ]; then
	fail "BLOCKED: the local suite has not passed since the last \`make clean'.

  make test

CI runs the same suites on macos-14, so a red one here is a red one there --
with the delay and the notification mail in between.  The stamp lives at
build/stage0/.tests-passed and \`make test' writes it only when all sixteen
suites pass.

  PUSH_ANYWAY=1 git push     to push regardless"
fi

# -name '*.md' is excluded because a PORTING.md cannot break a build, and this
# project rewrites them constantly -- a gate that demands a three-minute test
# run after a documentation edit is a gate that gets switched off.
newer=$(find "$root/src" "$root/shim" "$root/compiler" "$root/tests" \
             "$root/.github" "$root/Makefile" \
             -newer "$stamp" -type f ! -name '*.md' -print 2>/dev/null | head -3)
if [ -n "$newer" ]; then
	fail "BLOCKED: files have changed since the suite last passed.

$(printf '%s\n' "$newer" | sed 's|^'"$root"'/|  |')

  make test

CI runs the same sixteen suites, so this is the cheap half of the same answer.

  PUSH_ANYWAY=1 git push     to push regardless"
fi

# --------------------------------------------------------- 2. last push red?
#
# Everything below fails OPEN.  gh may be absent, logged out, rate-limited or
# offline, the branch may never have been pushed, and none of those is a reason
# to refuse work.
command -v gh >/dev/null 2>&1 || exit 0

branch=$(git -C "$root" rev-parse --abbrev-ref HEAD 2>/dev/null)
[ -n "$branch" ] || exit 0
sha=$(git -C "$root" rev-parse "origin/$branch" 2>/dev/null) || exit 0
[ -n "$sha" ] || exit 0

# Nothing new to push: whatever CI says, this push is a no-op.
[ "$sha" = "$(git -C "$root" rev-parse HEAD 2>/dev/null)" ] && exit 0

# One call, not two: status, conclusion and url together.  gh takes about a
# second and a half, which is affordable once per push and not twice.
run=$(gh run list --limit 20 --json headSha,status,conclusion,url 2>/dev/null |
      jq -r --arg sha "$sha" \
        '[.[] | select(.headSha==$sha)] | .[0]
         | "\(.status) \(.conclusion) \(.url)"' 2>/dev/null)
case "$run" in
''|null*|'null null null') exit 0 ;;       # no run for that commit yet
esac

status=${run%% *}
rest=${run#* }
conclusion=${rest%% *}
url=${rest#* }
[ "$status" = completed ] || exit 0        # still running; nothing decided yet

case "$conclusion" in
success|neutral|skipped) exit 0 ;;
esac

short=$(printf '%s' "$sha" | cut -c1-8)
subject=$(git -C "$root" log -1 --format=%s "$sha" 2>/dev/null)
fail "BLOCKED: CI is $conclusion on what is already pushed.

  $short  $subject
  $url

Pushing now adds a commit on top of a red build and sends another failure
mail. Fix that run first -- \`gh run view --log-failed' names the case.

This is the check that pays for itself: of the ten failure mails from the
session that added this hook, nine were commits pushed onto an already-red
build, each one failing again for the same reason as the last.

  PUSH_ANYWAY=1 git push     when the push IS the fix"
exit 0
