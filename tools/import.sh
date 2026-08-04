#!/bin/sh
# Import upstream V8 sources into src/ for patching, recording provenance.
#
#   tools/import.sh v8/usr/src/cmd/cpp        -> src/cmd/cpp/
#   tools/import.sh v8/usr/src/cmd/cat.c      -> src/cmd/cat.c
#   tools/import.sh v8/usr/include/stdio.h    -> src/include/stdio.h
#
# Upstream is never modified. Each destination directory gets a PROVENANCE file
# listing the upstream path and git blob hash of every file imported into it, so
# a diff against pristine V8 is always reconstructible:
#
#   git hash-object src/cmd/cat.c        # compare against the recorded hash
#
# Re-importing a file that already exists locally is refused unless -f is given,
# so local patches are never silently clobbered.

set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
UPSTREAM=$ROOT/third_party/Research-Unix-v8
UPSTREAM_URL=https://github.com/Alhadis/Research-Unix-v8.git
UPSTREAM_COMMIT=389623b76d5b6e195361f0705b1826b00ae14d19

force=0
if [ "$1" = "-f" ]; then force=1; shift; fi

if [ $# -lt 1 ]; then
	echo "usage: tools/import.sh [-f] <upstream-relative-path>..." >&2
	echo "   eg: tools/import.sh v8/usr/src/cmd/cpp" >&2
	exit 2
fi

# Map an upstream path to its destination under src/.
destfor() {
	case "$1" in
	v8/usr/src/*)   echo "src/${1#v8/usr/src/}" ;;
	v8/usr/include/*) echo "src/include/${1#v8/usr/include/}" ;;
	v8/usr/*)       echo "src/${1#v8/usr/}" ;;
	blit/*|jerq/*)  echo "src/$1" ;;
	*)              echo "src/$1" ;;
	esac
}

# Append one "hash  upstream -> local" line to the right PROVENANCE file.
record() {
	up=$1 loc=$2 pdir=$3
	prov=$ROOT/$pdir/PROVENANCE
	if [ ! -f "$prov" ]; then
		{
			echo "# Provenance for $pdir"
			echo "# Upstream: $UPSTREAM_URL"
			echo "# Commit:   $UPSTREAM_COMMIT"
			echo "# Written by tools/import.sh -- do not edit by hand."
			echo "#"
			echo "# <git blob hash of pristine upstream file>  <upstream path>"
		} > "$prov"
	fi
	h=$(git hash-object "$UPSTREAM/$up")
	# Replace any existing line for this path, then append.
	if grep -q "  $up\$" "$prov" 2>/dev/null; then
		grep -v "  $up\$" "$prov" > "$prov.tmp" && mv "$prov.tmp" "$prov"
	fi
	echo "$h  $up" >> "$prov"
}

# Import a single regular file.
one() {
	up=$1
	dst=$(destfor "$up")
	dstdir=$(dirname "$dst")

	if [ -e "$ROOT/$dst" ] && [ $force -eq 0 ]; then
		echo "import: $dst exists (use -f to overwrite local patches)" >&2
		return 1
	fi

	mkdir -p "$ROOT/$dstdir"
	cp -p "$UPSTREAM/$up" "$ROOT/$dst"
	chmod u+w "$ROOT/$dst"
	record "$up" "$dst" "$dstdir"
	echo "  $up -> $dst"
}

for path in "$@"; do
	path=${path#./}
	if [ ! -e "$UPSTREAM/$path" ]; then
		echo "import: no such upstream path: $path" >&2
		exit 1
	fi

	if [ -d "$UPSTREAM/$path" ]; then
		echo "importing directory $path"
		# -type f only: skip subdirectories' recursion into binaries is fine,
		# the caller chooses what to import.
		(cd "$UPSTREAM/$path" && find . -type f) | sed 's|^\./||' | while read -r f; do
			one "$path/$f" || true
		done
	else
		echo "importing file $path"
		one "$path"
	fi
done
