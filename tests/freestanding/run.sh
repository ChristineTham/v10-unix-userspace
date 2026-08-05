#!/bin/sh
# The end-to-end proof: a program compiled by V8's compiler, linked against our
# crt0 and libv8sys and NOTHING ELSE, running on macOS.
#
# This is what the whole toolchain exists to produce. It is a separate suite
# from tests/v8cc because those programs link the host libc for their printf;
# these link none of it. libSystem.dylib is on the link line only because macOS
# refuses to make a dynamic executable without it -- no symbol is taken from it,
# which is what `nm -u` checks below.

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CC=$ROOT/rootfs/bin/cc
V8ROOT=$ROOT/rootfs
export V8ROOT
TMP=${TMPDIR:-/tmp}/freestanding.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT
cd "$TMP" || exit 1

pass=0 fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL $1"; shift; [ $# -gt 0 ] && echo "    $*"; }

CRT=$ROOT/build/stage0/crt0.o
STUBS=$ROOT/build/stage0/v8sys/libv8stubs.a
for f in "$CRT" "$STUBS"; do
	[ -f "$f" ] || { echo "missing $f -- run make libv8sys crt0"; exit 1; }
done
SHIM=$ROOT/build/stage0/v8sys/libv8sys.a

link() { clang -nostdlib -e _v8start -o "$1" "$CRT" "$2" "$STUBS" "$SHIM" -lSystem; }

# Run with a deadline.  macOS ships no timeout(1), and one case below fails by
# hanging rather than by answering wrongly -- a signal handler the kernel cannot
# enter does not come back.  The watchdog's output goes to /dev/null so it never
# holds a pipe open past the command it is watching.
runlimit() {
	_lim=$1; shift
	"$@" & _rpid=$!
	( sleep "$_lim"; kill -9 $_rpid ) >/dev/null 2>&1 & _wpid=$!
	wait $_rpid; _rc=$?
	kill $_wpid >/dev/null 2>&1
	return $_rc
}

# --- write(2) reaches the kernel through the shim ------------------------
cat > hello.c <<'EOF'
main()
{
	write(1, "hello from v8\n", 14);
	return 0;
}
EOF
if "$CC" -c hello.c 2>err.log && link hello hello.o 2>>err.log &&
   [ "$(./hello)" = "hello from v8" ]; then ok
else bad "write() through the shim" "$(head -3 err.log)"; fi

# --- crt0 delivers argc and argv ----------------------------------------
cat > args.c <<'EOF'
main(argc, argv)
	char **argv;
{
	int i;
	for (i = 0; i < argc; i++) {
		char *p = argv[i];
		int n = 0;
		while (p[n]) n++;
		write(1, p, n);
		write(1, "\n", 1);
	}
	return 0;
}
EOF
if "$CC" -c args.c 2>err.log && link args args.o 2>>err.log &&
   [ "$(./args one two | tr '\n' ',')" = "./args,one,two," ]; then ok
else bad "crt0 hands main its argc and argv" "$(head -3 err.log)"; fi

# --- main's return value becomes the exit status -------------------------
cat > status.c <<'EOF'
main() { return 7; }
EOF
if "$CC" -c status.c 2>err.log && link status status.o 2>>err.log; then
	./status; [ $? -eq 7 ] && ok || bad "exit status is main's return value"
else bad "exit status build" "$(head -3 err.log)"; fi

# --- open/read/close on a real file --------------------------------------
printf 'abcdefghij' > data.txt
cat > readfile.c <<'EOF'
main()
{
	int fd, n;
	char buf[64];

	fd = open("data.txt", 0);
	if (fd < 0) { write(1, "openfail\n", 9); return 1; }
	n = read(fd, buf, 64);
	write(1, buf, n);
	write(1, "\n", 1);
	close(fd);
	return 0;
}
EOF
if "$CC" -c readfile.c 2>err.log && link readfile readfile.o 2>>err.log &&
   [ "$(./readfile)" = "abcdefghij" ]; then ok
else bad "open/read/close" "$(head -3 err.log)"; fi

# --- THE directory test: read(2) on a directory, V7 records --------------
# This is the one that cannot work without the shim, since macOS refuses
# read(2) on a directory outright.
mkdir subdir
: > subdir/alpha
: > subdir/beta
cat > readdir.c <<'EOF'
struct direct { unsigned short d_ino; char d_name[14]; };

main()
{
	int fd, n, count;
	struct direct d;

	fd = open("subdir", 0);
	if (fd < 0) { write(1, "openfail\n", 9); return 1; }
	count = 0;
	while ((n = read(fd, &d, 16)) == 16) {
		if (d.d_ino == 0) continue;	/* V7: an empty slot */
		if (d.d_name[0] == 'a' || d.d_name[0] == 'b') count++;
	}
	close(fd);
	write(1, count == 2 ? "two\n" : "wrong\n", count == 2 ? 4 : 6);
	return 0;
}
EOF
if "$CC" -c readdir.c 2>err.log && link readdir readdir.o 2>>err.log &&
   [ "$(./readdir)" = "two" ]; then ok
else bad "read(2) on a directory yields V7 records" "$(head -3 err.log)"; fi

# --- sbrk, which is what V8's malloc is built on -------------------------
cat > brk.c <<'EOF'
char *sbrk();

main()
{
	char *a, *b;

	a = sbrk(0);
	b = sbrk(1024);
	if (a != b) { write(1, "notold\n", 7); return 1; }
	*b = 'X';			/* must be committed and writable */
	if (sbrk(0) != a + 1024) { write(1, "nomove\n", 7); return 1; }
	write(1, "brk ok\n", 7);
	return 0;
}
EOF
if "$CC" -c brk.c 2>err.log && link brk brk.o 2>>err.log &&
   [ "$(./brk)" = "brk ok" ]; then ok
else bad "sbrk arena" "$(head -3 err.log)"; fi

# --- a signal is delivered to a program the V8 compiler built ------------
# tests/v8sys covers this seam from the other side, but only from the other
# side: that suite is clang-built and links the host libc, so it proves
# v8s_signal's arithmetic and nothing about what crosses the seam.  Here the
# handler is compiled by v8cc and entered by the kernel through the trampoline
# in shim/v8sys/sigtramp.s, using v8cc's own positional argument convention.
# The suite that proved the shim was clean was never the suite that proved the
# world built on it was -- the same gap that hid five libc imports until
# tests/kmemu swept the rootfs.
#
# Deliberately the demonstration program from the bug report, which printed
# "installed; killing self" and then hung forever.
cat > sigcatch.c <<'EOF'
#include <signal.h>

int caught;

onint(sig)
{
	caught = sig;
}

main()
{
	if (signal(SIGINT, onint) == BADSIG) { write(1, "noinstall\n", 10); return 1; }
	kill(getpid(), SIGINT);
	/* reaching this line at all is sigreturn having worked */
	if (caught != SIGINT) { write(1, "nohandler\n", 10); return 1; }
	/* V7 semantics: delivery resets the handler to SIG_DFL */
	if (signal(SIGINT, SIG_IGN) != SIG_DFL) { write(1, "noreset\n", 8); return 1; }
	write(1, "caught\n", 7);
	return 0;
}
EOF
if "$CC" -c sigcatch.c 2>err.log && link sigcatch sigcatch.o 2>>err.log; then
	runlimit 5 ./sigcatch >out.txt 2>&1; rc=$?
	if [ "$(cat out.txt)" = "caught" ]; then ok
	else bad "a v8cc-compiled program catches a signal" \
	         "exit $rc, output [$(cat out.txt)] -- exit 137 is the deadline killing a hang"; fi
else bad "signal catcher build" "$(head -3 err.log)"; fi

# --- nothing is taken from libSystem ------------------------------------
# The point of the raw-syscall layer: the image must not import any libc
# symbol. If this ever fails, something in the shim started calling libc again
# and the recursion in shim/NOTES.md is waiting to come back.
# nm -m distinguishes a real import from a weak reference. __cleanup is weak on
# purpose: stubs.c's exit() calls it to flush stdio when V8 libc is linked, and
# these freestanding programs link none, so it stays unresolved and unused.
# A weak unresolved symbol pulls nothing in and is not a libc dependency.
undef=$(nm -m hello 2>/dev/null | grep 'undefined' | grep -v 'weak' |
        grep -v 'dyld_stub_binder' | wc -l | tr -d ' ')
if [ "$undef" = "0" ]; then ok
else bad "image imports $undef libSystem symbol(s)" "$(nm -u hello | head -5)"; fi

echo "freestanding: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
