#!/bin/sh
# PreToolUse: refuse any write under third_party/.
#
# third_party/ is 8317 vendored upstream files whose whole value is that they
# are PRISTINE.  Every file imported into src/ records the upstream git blob
# hash in a PROVENANCE file, which is what makes the diff against real V8
# reconstructible -- the central claim of this port.  Editing an upstream file
# in place breaks that silently and unrecoverably: the hash no longer describes
# anything, and there is no way afterwards to tell what was Bell Labs' and what
# was ours.
#
# Exit 2 blocks the tool call and shows stderr to Claude.

path=$(jq -r '.tool_input.file_path // empty' 2>/dev/null)
[ -n "$path" ] || exit 0

case "$path" in
*/third_party/*|third_party/*)
	cat >&2 <<EOF
BLOCKED: third_party/ is read-only.

  $path

It holds pristine upstream sources; PROVENANCE records each file's upstream git
blob hash so the diff against real V8 stays reconstructible.  Editing in place
destroys that.

To work on this file, import it first:

  tools/import.sh v8/usr/src/cmd/NAME

then edit the copy under src/, and record what changed and why in that
program's PORTING.md.
EOF
	exit 2
	;;
esac
exit 0
