#!/bin/sh
# PreToolUse (Bash): refuse the HOST's make where an AUTHENTIC V8 makefile is
# what would be read.
#
# WHAT THIS IS PROTECTING.  There are two makes in this port and they have
# different jobs, recorded in PLAN.md section 4a:
#
#   rungs 0-3   GNU make, driving OUR top-level Makefile -- the seed harness
#               that builds cpp, ccom, cc, libv8sys, crt0, then libv8c, yacc,
#               lex and finally V8's own make.  Legitimate, and the everyday
#               command.  This hook must never get in its way.
#   rungs 4-5   V8's make, reading BELL LABS' makefiles, running Bell Labs'
#               recipes through V8's sh and V8's cc, under V8JAIL=strict.
#
# The whole claim of rung 4 is that the build DESCRIPTION is theirs and not
# ours.  Running `make` in src/cmd/lex hands that description to GNU make
# instead: it works, the objects come out, and the one thing the rung was
# proving quietly did not happen.  Nothing fails, which is why it wants a
# tripwire rather than a test.
#
# HOW IT DECIDES, and why it is keyed on PROVENANCE rather than a list of
# directory names.  A makefile counts as authentic when tools/import.sh recorded
# its upstream blob hash -- that is the same fact that makes the diff against
# pristine V8 reconstructible, and it means this hook covers a program the day
# its makefile is imported, with nothing here to update.  Our own top-level
# Makefile has no PROVENANCE line, so the everyday `make -j8` and `make test`
# are untouched by construction rather than by an exception.
#
# Exit 2 blocks the call and shows stderr to Claude.

# Read stdin ONCE.  Two jq calls do not both work: the first consumes the pipe
# and the second silently reads nothing, which here meant `cwd' came back empty
# and every invocation looked like it was at the project root.  The hook then
# passed everything and looked like it was working.
payload=$(cat)
cmd=$(printf '%s' "$payload" | jq -r '.tool_input.command // empty' 2>/dev/null)
[ -n "$cmd" ] || exit 0

# Bail out before python for the overwhelming majority of commands, which do not
# mention make at all.  This runs on EVERY Bash call, and an interpreter start
# on each one is the kind of tax that gets a hook switched off.
case "$cmd" in
*make*) ;;
*) exit 0 ;;
esac

root=${CLAUDE_PROJECT_DIR:-.}
cwd=$(printf '%s' "$payload" | jq -r '.cwd // empty' 2>/dev/null)
[ -n "$cwd" ] || cwd=$root

python3 - "$root" "$cwd" "$cmd" <<'PY'
import os, re, shlex, sys

root, cwd, cmd = sys.argv[1], sys.argv[2], sys.argv[3]

def authentic(d):
    """The makefile in directory d is Bell Labs', per its PROVENANCE."""
    prov = os.path.join(d, 'PROVENANCE')
    if not os.path.isfile(prov):
        return None
    try:
        text = open(prov, errors='replace').read()
    except OSError:
        return None
    for line in text.splitlines():
        # "<blob>  v8/usr/src/cmd/lex/Makefile"
        name = line.rsplit('/', 1)[-1].strip()
        if name.lower() in ('makefile',) and not line.startswith('#'):
            for spelling in ('Makefile', 'makefile'):
                p = os.path.join(d, spelling)
                if os.path.isfile(p):
                    return p
    return None

# Split into commands, and carry `cd` ACROSS them.  `cd src/cmd/lex && make' is
# two segments, and treating each in isolation loses the cd entirely -- which is
# the single most likely way to make this mistake, so getting it wrong made the
# hook pass the exact case it exists for.
segments = re.split(r'(?:\n|;|\|\||&&|\|)+', cmd)

here = cwd
for seg in segments:
    try:
        words = shlex.split(seg)
    except ValueError:
        continue
    if not words:
        continue

    # A `cd` anywhere in this segment moves us, and stays moved for the next.
    for i, w in enumerate(words):
        if w == 'cd' and i + 1 < len(words):
            nxt = words[i + 1]
            here = nxt if os.path.isabs(nxt) else os.path.join(here, nxt)

    # Find a make invocation, and work out WHICH make.
    for i, w in enumerate(words):
        base = os.path.basename(w)
        if base not in ('make', 'gmake'):
            continue
        # V8's own make is the whole point -- never block it.  It is always
        # reached by a path (rootfs/bin/make, $V8ROOT/bin/make, $MAKE8), never
        # as a bare word, because the bare word is the host's.
        if w != base and ('rootfs' in w or 'V8ROOT' in w or 'build/' in w):
            break

        target_dirs = []
        args = words[i + 1:]
        for j, a in enumerate(args):
            if a == '-C' and j + 1 < len(args):
                d = args[j + 1]
                target_dirs.append(d if os.path.isabs(d) else os.path.join(here, d))
            elif a.startswith('-C') and len(a) > 2:
                d = a[2:]
                target_dirs.append(d if os.path.isabs(d) else os.path.join(here, d))
            elif a == '-f' and j + 1 < len(args):
                f = args[j + 1]
                f = f if os.path.isabs(f) else os.path.join(here, f)
                target_dirs.append(os.path.dirname(f) or here)
            elif a.startswith('-f') and len(a) > 2:
                f = a[2:]
                f = f if os.path.isabs(f) else os.path.join(here, f)
                target_dirs.append(os.path.dirname(f) or here)
        if not target_dirs:
            target_dirs = [here]

        for d in target_dirs:
            mk = authentic(os.path.normpath(d))
            if mk:
                rel = os.path.relpath(mk, root)
                reld = os.path.relpath(os.path.dirname(mk), root)
                sys.stderr.write("""BLOCKED: that is Bell Labs' makefile, so V8's make has to read it.

  %s

%s is authentic upstream V8 -- tools/import.sh recorded its blob hash in
PROVENANCE.  Rungs 4 and 5 of the bootstrap ladder exist to prove that the build
DESCRIPTION is theirs and not ours, and handing it to GNU make is exactly the
thing that makes the proof stop meaning anything.  It would work, which is the
problem: the objects come out and nothing says the rung did not happen.

Use V8's make, in the jail:

  export V8ROOT=$PWD/rootfs
  cd %s && V8JAIL=strict $V8ROOT/bin/make

V8JAIL=strict is the part that matters -- it refuses any escape to a host binary
except the documented as/ld/ar/strip/nm exception, so a green build is a claim
about V8 code and not about what happened to be on PATH.  tests/jail is the
worked example.

If this makefile names the target machine it cannot be used unchanged -- only
src/cmd/cpp/Makefile does, with -Dvax=1.  PLAN.md section 4a covers adapting
one, and every deviation gets recorded.
""" % (rel, reld, reld))
                sys.exit(2)
        break   # one make per segment is enough
sys.exit(0)
PY
