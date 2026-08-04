#!/bin/sh
# Wave B: the file and process tools, plus the multi-file commands.
#
# Wave A's programs are single files that read stdin and write stdout. These
# touch the filesystem, fork, and in ed's and sed's case carry their own
# regular-expression engines, so they exercise a good deal more of the shim.

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CC=$ROOT/rootfs/bin/cc
V8ROOT=$ROOT/rootfs
export V8ROOT
TMP=${TMPDIR:-/tmp}/waveb.$$
mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT

LIBC=$ROOT/build/stage0/libc/libv8c.a
CRT=$ROOT/build/stage0/crt0.o
STUBS=$ROOT/build/stage0/v8sys/libv8stubs.a
SHIM=$ROOT/build/stage0/v8sys/libv8sys.a

pass=0 fail=0
check() {
	if [ "$2" = "$3" ]; then pass=$((pass+1))
	else fail=$((fail+1)); echo "FAIL $1"; echo "  want [$2]"; echo "  got  [$3]"; fi
}

# build <name> <source...> -- one command, however many files it takes
build() {
	name=$1; shift
	objs=""
	for f in "$@"; do
		o="$TMP/$(basename "$f" .c).o"
		if ! "$CC" -I"$(dirname "$f")" -c -o "$o" "$f" >> "$TMP/$name.log" 2>&1; then
			echo "FAIL $name (compile $(basename "$f"))"
			head -2 "$TMP/$name.log"; return 1
		fi
		objs="$objs $o"
	done
	if ! clang -nostdlib -e _v8start -o "$TMP/$name" "$CRT" $objs \
	    "$LIBC" "$STUBS" "$SHIM" -lSystem >> "$TMP/$name.log" 2>&1; then
		echo "FAIL $name (link)"; head -3 "$TMP/$name.log"; return 1
	fi
	return 0
}

try() {	# try <name> <label> <expected> <pipeline>
	name=$1; label=$2; want=$3; shift 3
	check "$label" "$want" "$(eval "$@" 2>&1)"
}

cd "$TMP" || exit 1
printf 'banana\napple\ncherry\n' > in.txt

C=$ROOT/src/cmd
for c in cp mv mkdir rmdir dc factor primes ed; do
	build $c "$C/$c/$c.c" || fail=$((fail+1))
done
build sed   "$C/sed/sed0.c" "$C/sed/sed1.c"                   || fail=$((fail+1))
build tsort "$C/tsort/tsort.c" "$C/tsort/subs.c" "$C/tsort/refstore.c" || fail=$((fail+1))

# ---- cp / mv: the file tools ---------------------------------------------
try cp 'cp copies contents' 'banana apple cherry' \
    "./cp in.txt c.out && tr '\n' ' ' < c.out | sed 's/ \$//'"
try cp 'cp leaves the original' 'banana apple cherry' \
    "tr '\n' ' ' < in.txt | sed 's/ \$//'"
try mv 'mv renames' 'banana apple cherry' \
    "./mv c.out m.out && tr '\n' ' ' < m.out | sed 's/ \$//'"
try mv 'mv removes the source' 'gone' "[ -f c.out ] || echo gone"

# ---- mkdir / rmdir -------------------------------------------------------
# V7 had no mkdir(2): mkdir(1) makes the inode with mknod(2) and links its own
# "." and ".." by hand, which is why it was setuid root.  The shim turns that
# into the host's mkdir(2) and lets the two dot-links succeed as no-ops, since
# the host has already made them -- see v8s_mknod and v8s_link in syscall.c.
try mkdir 'mkdir makes a directory' 'made' "./mkdir d1 && [ -d d1 ] && echo made"
try mkdir 'and it has . and ..'     '. ..' "ls -a d1 | tr '\n' ' ' | sed 's/ \$//'"
try mkdir 'usable as a directory'   'x'    "echo x > d1/f && cat d1/f"
try rmdir 'rmdir removes it'        'gone' "rm -f d1/f && ./rmdir d1 && [ ! -d d1 ] && echo gone"

# ---- sed -----------------------------------------------------------------
try sed 'sed substitutes'  'goodbye'      "printf 'hello\n' | ./sed 's/hello/goodbye/'"
try sed 'sed deletes'      'apple cherry' "./sed '/banana/d' in.txt | tr '\n' ' ' | sed 's/ \$//'"
try sed 'sed prints a range' 'banana apple' "./sed -n '1,2p' in.txt | tr '\n' ' ' | sed 's/ \$//'"
try sed 'sed uses a regexp' 'BANANA'      "./sed -n 's/^ban.*/BANANA/p' in.txt"

# ---- ed: the editor, driven by a script ----------------------------------
try ed 'ed appends and prints' 'one two' \
    "printf 'a\none\ntwo\n.\n1,2p\nQ\n' | ./ed 2>/dev/null | tr '\n' ' ' | sed 's/ \$//'"
try ed 'ed substitutes'        'ONE' \
    "printf 'a\none\n.\ns/one/ONE/\np\nQ\n' | ./ed 2>/dev/null | tail -1"

# ---- dc: the desk calculator (arbitrary precision) -----------------------
try dc 'dc adds'      '5'   "printf '2 3 + p\n' | ./dc"
try dc 'dc multiplies' '12' "printf '3 4 * p\n' | ./dc"
try dc 'dc is arbitrary precision' '1267650600228229401496703205376' \
    "printf '2 100 ^ p\n' | ./dc"

# ---- factor / primes -----------------------------------------------------
try factor 'factor 91'  '91 7 13' "./factor 91 | tr -s ' \n' ' ' | sed 's/ \$//'"
try factor 'factor 97 is prime' '97 97' "./factor 97 | tr -s ' \n' ' ' | sed 's/ \$//'"
try primes 'primes 10 20' '11 13 17 19' "./primes 10 20 | tr '\n' ' ' | sed 's/ \$//'"

# ---- tsort: topological sort --------------------------------------------
try tsort 'tsort orders' 'a b c' "printf 'a b\nb c\n' | ./tsort | tr '\n' ' ' | sed 's/ \$//'"

echo "waveb: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
