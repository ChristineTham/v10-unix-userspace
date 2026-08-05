#!/bin/sh
# The .claude hooks, which are code and rot like code.
#
# WHY THIS EXISTS.  A hook fails in the direction that is hardest to notice: it
# lets something through and says nothing, so the tripwire is simply not there
# and everything looks fine.  v8-make.sh had two such bugs in its first draft --
# it called jq twice on the same stdin, so `cwd' came back empty and every
# command looked like it was at the project root; and it split the command on
# `&&', which threw away the `cd' in `cd src/cmd/lex && make', the single most
# likely way to make the mistake it exists to catch.  Both versions passed a
# casual look, and both let the real case through.
#
# So the negative cases matter as much as the positive ones.  A hook that blocks
# the everyday `make -j8` gets switched off within the hour, and then it is not
# protecting anything either.

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1
pass=0 fail=0

# hook <script> <cwd> <command> -> prints "block" or "pass"
hook() {
	printf '{"cwd":%s,"tool_input":{"command":%s}}' \
	    "$(python3 -c 'import json,sys;print(json.dumps(sys.argv[1]))' "$2")" \
	    "$(python3 -c 'import json,sys;print(json.dumps(sys.argv[1]))' "$3")" |
	CLAUDE_PROJECT_DIR=$ROOT "$ROOT/.claude/hooks/$1" >/dev/null 2>&1
	[ $? -eq 2 ] && echo block || echo pass
}

mk() {	# mk <label> <pass|block> <cwd> <command>
	got=$(hook v8-make.sh "$3" "$4")
	if [ "$got" = "$2" ]; then pass=$((pass+1))
	else fail=$((fail+1)); echo "FAIL v8-make: $1"; echo "  want $2, got $got"
	     echo "  cwd=$3"; echo "  cmd=$4"; fi
}

# --- v8-make.sh: the everyday workflow must be untouched --------------------
# Rungs 0-3 are GNU make driving our own top-level Makefile, by design.  These
# are the commands CLAUDE.md tells the next reader to run.
mk 'top-level build'          pass "$ROOT" 'make -j8'
mk 'the test suite'           pass "$ROOT" 'make test'
mk 'one suite'                pass "$ROOT" 'make test-kmemu'
mk 'an absolute target'       pass "$ROOT" "make $ROOT/build/stage0/bin/cat"
mk 'the check-makefile hook'  pass "$ROOT" 'make -n --warn-undefined-variables'
mk 'the deps suite pattern'   pass "$ROOT" 'make -q build/stage0/sh/main.o'
mk 'a dir with no makefile'   pass "$ROOT" 'cd shim/libkmemu && make'
mk 'cd in and back out'       pass "$ROOT" 'cd src/cmd/lex && cd ../../.. && make'
mk 'no make at all'           pass "$ROOT" 'ls src/cmd/lex'
mk 'the word make in a path'  pass "$ROOT" 'cat src/cmd/make/PORTING.md'

# --- v8-make.sh: V8's own make is the point, and must never be blocked ------
mk 'V8 make, jailed'          pass "$ROOT" 'cd src/cmd/lex && V8JAIL=strict $V8ROOT/bin/make'
mk 'V8 make by rootfs path'   pass "$ROOT" 'cd src/cmd/tbl && rootfs/bin/make'
mk 'V8 make with -C'          pass "$ROOT" "$ROOT/rootfs/bin/make -C src/cmd/sed"

# --- v8-make.sh: the host's make on Bell Labs' makefiles is refused ---------
# Each of these would WORK, which is the problem: the objects come out and
# nothing says rung 4 did not happen.
mk 'cd into an authentic dir' block "$ROOT" 'cd src/cmd/lex && make'
mk '-C an authentic dir'      block "$ROOT" 'make -C src/cmd/tbl'
mk '-f an authentic makefile' block "$ROOT" 'make -f src/cmd/yacc/Makefile'
mk 'lowercase makefile'       block "$ROOT" 'make -C src/cmd/tbl all'
mk 'already in the directory'  block "$ROOT/src/cmd/sed" 'make'
mk 'gmake counts too'         block "$ROOT" 'cd src/cmd/spell && gmake'
mk 'a cd in an earlier stage' block "$ROOT" 'echo hi; cd src/cmd/fmt && make'
mk 'the attached -C form'     block "$ROOT" 'make -Csrc/cmd/troff'

# --- v8-make.sh keys on PROVENANCE, not on a list of names ------------------
# The check is "tools/import.sh recorded this makefile's upstream blob hash",
# which is the same fact that makes the diff against pristine V8 reconstructible
# -- so a program is covered the day its makefile is imported, with nothing in
# the hook to update.  Assert that rather than trusting the comment: every
# src/cmd directory whose PROVENANCE names a makefile must be refused.
covered=0 missed=""
for d in src/cmd/*/; do
	grep -qiE '/(makefile)$' "$d/PROVENANCE" 2>/dev/null || continue
	covered=$((covered+1))
	[ "$(hook v8-make.sh "$ROOT" "make -C $d")" = "block" ] || missed="$missed ${d}"
done
if [ "$covered" -ge 10 ] && [ -z "$missed" ]; then pass=$((pass+1))
else fail=$((fail+1)); echo "FAIL v8-make: covers every authentic makefile"
     echo "  found $covered, not refused:$missed"; fi

# --- the day mk arrives, this hook goes quiet on its own --------------------
#
# `mk' is Andrew Hume's successor to make, not a nickname for it. V8 has no
# trace of one -- no mk, no mk.1, no mkfile, and the only mk.c upstream belongs
# to efl -- because mk is from 1987 and arrives with V9/V10, which is where this
# project is headed.
#
# v8-make.sh decides a build description is authentic by looking for a
# PROVENANCE line naming a *makefile*. An imported `mkfile' matches nothing, is
# waved through, and the hook goes on reporting success while covering none of
# the new tree. A guard that stops guarding without saying so is the failure
# this suite exists for, so the arrival of the first mkfile is itself the alarm.
#
# WHEN THIS FAILS, it is not a bug: it means V9/V10 sources have landed. Teach
# v8-make.sh to recognise mkfile and to name `mk' rather than V8's make in its
# message, extend the PROVENANCE-coverage case above, then replace this one.
# PLAN.md section 4a has the reasoning and what else the step costs.
mkfiles=$(find src third_party -name mkfile -o -name 'mkfile.*' 2>/dev/null | head -5)
if [ -z "$mkfiles" ]; then pass=$((pass+1))
else
	fail=$((fail+1))
	echo "FAIL v8-make: mkfiles exist now, and the hook does not know about mk"
	echo "$mkfiles" | sed 's/^/    /'
	echo "    see PLAN.md 4a -- mk is a port, not a rename"
fi

# --- block-third-party.sh ---------------------------------------------------
tp() {	# tp <label> <pass|block> <path>
	got=$(printf '{"tool_input":{"file_path":%s}}' \
	      "$(python3 -c 'import json,sys;print(json.dumps(sys.argv[1]))' "$3")" |
	      CLAUDE_PROJECT_DIR=$ROOT "$ROOT/.claude/hooks/block-third-party.sh" >/dev/null 2>&1
	      [ $? -eq 2 ] && echo block || echo pass)
	if [ "$got" = "$2" ]; then pass=$((pass+1))
	else fail=$((fail+1)); echo "FAIL third-party: $1 (want $2, got $got)"; fi
}
tp 'an upstream source'  block "$ROOT/third_party/Research-Unix-v8/v8/usr/src/cmd/who.c"
tp 'a relative path'     block 'third_party/Research-Unix-v8/README'
tp 'our own source'      pass  "$ROOT/src/cmd/who.c"
tp 'a name merely alike' pass  "$ROOT/src/third_party_notes.md"

echo "hooks: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
