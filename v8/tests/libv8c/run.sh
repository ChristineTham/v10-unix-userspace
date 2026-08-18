#!/bin/sh
# V8's own libc, compiled by v8cc, linked freestanding.

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CC=$ROOT/rootfs/bin/cc
V8ROOT=$ROOT/rootfs
export V8ROOT
TMP=${TMPDIR:-/tmp}/libv8c.$$
mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT; cd "$TMP" || exit 1

pass=0 fail=0
LIBC=$ROOT/build/stage0/libc/libv8c.a
CRT=$ROOT/build/stage0/crt0.o
STUBS=$ROOT/build/stage0/v8sys/libv8stubs.a
# Link the ARCHIVES, never a glob of loose .o files: a stale object left
# behind by a rename once shadowed libv8stubs.a entirely, and the suites
# passed while testing the old code.
SHIM=$ROOT/build/stage0/v8sys/libv8sys.a

run() {	# run <name> <expected>; program source on stdin
	name=$1; want=$2; cat > t.c
	if ! "$CC" -c t.c 2>e.log; then
		fail=$((fail+1)); echo "FAIL $name (compile)"; head -2 e.log; return
	fi
	if ! clang -nostdlib -e _v8start -o t "$CRT" t.o "$LIBC" "$STUBS" "$SHIM" -lSystem 2>>e.log; then
		fail=$((fail+1)); echo "FAIL $name (link)"; head -2 e.log; return
	fi
	got=$(./t 2>&1)
	if [ "$got" = "$want" ]; then pass=$((pass+1))
	else fail=$((fail+1)); echo "FAIL $name"; echo "  want [$want]"; echo "  got  [$got]"; fi
}

run 'fputs' 'fputs works' <<'EOF'
#include <stdio.h>
main() { fputs("fputs works\n", stdout); fflush(stdout); return 0; }
EOF

run 'printf literal' 'hello' <<'EOF'
#include <stdio.h>
main() { printf("hello\n"); fflush(stdout); return 0; }
EOF

run 'printf integers' 'd=42 n=-17 x=ff o=100 u=7' <<'EOF'
#include <stdio.h>
main() { printf("d=%d n=%d x=%x o=%o u=%u\n", 42, -17, 255, 64, 7);
         fflush(stdout); return 0; }
EOF

run 'printf strings' 's=[v8] c=X' <<'EOF'
#include <stdio.h>
main() { printf("s=[%s] c=%c\n", "v8", 'X'); fflush(stdout); return 0; }
EOF

run 'printf width and padding' '[   42][42   ][00042]' <<'EOF'
#include <stdio.h>
main() { printf("[%5d][%-5d][%05d]\n", 42, 42, 42); fflush(stdout); return 0; }
EOF

# A NEGATIVE long through the unsigned conversions.  doprnt.c's convert() builds
# its digits with digits[val % base], and the top bit is still set on the FIRST
# iteration only -- so a signed remainder there indexed off the FRONT of the
# digit string, picking up a NUL, and %lx printed 15 digits and a stray byte
# where it should have printed 16.  Every later digit was right, because by then
# `val /= base' had shifted the top bit away.
#
# The cause was in the compiler rather than here: see the three sconvert() cases
# in tests/v8ccom.  These guard the symptom, which is what a caller sees.
#
# The values are the atof fixtures below, for a reason worth keeping.
# bff4000000000000 is the one that HID this: its low nibble is 0, a signed
# remainder of 0 is still 0, so the only visibly negative case in the tree
# looked perfectly correct.  Any replacement must keep a negative value whose
# low digit is significant.
run 'printf %lx of negative longs' \
'bf1a36e2eb1c432d bdf80d43de9cc603 bff4000000000000 419d6f34547e6b75 BF1A36E2EB1C432D' <<'EOF'
#include <stdio.h>
static long v[] = { 0xbf1a36e2eb1c432dL, 0xbdf80d43de9cc603L,
		    0xbff4000000000000L, 0x419d6f34547e6b75L };
main()
{
	int i;

	for (i = 0; i < 4; i++) printf("%lx ", v[i]);
	printf("%lX\n", v[0]);
	fflush(stdout); return 0;
}
EOF

# %lo takes the same path with base 8, where the top bit lands in a digit of its
# own; and %ld / %lu are here so that a fix which simply forced the conversion
# unsigned would be caught -- -1 must still print as -1 under %ld.
run 'printf %lo of negative longs' \
'1374321556135307041455 1377640000000000000000 1777777777777777777777 -1 18446744073709551615' <<'EOF'
#include <stdio.h>
main()
{
	printf("%lo %lo %lo %ld %lu\n",
	    0xbf1a36e2eb1c432dL, 0xbff4000000000000L, -1L, -1L, -1L);
	fflush(stdout); return 0;
}
EOF

run 'malloc' 'malloc ok' <<'EOF'
#include <stdio.h>
char *malloc();
main() {
	char *p; int i;
	p = malloc(100);
	if (p == 0) { printf("malloc failed\n"); fflush(stdout); return 1; }
	for (i = 0; i < 100; i++) p[i] = 'x';
	if (p[99] != 'x') { printf("bad\n"); fflush(stdout); return 1; }
	printf("malloc ok\n"); fflush(stdout); return 0;
}
EOF

run 'malloc many' 'many ok' <<'EOF'
#include <stdio.h>
char *malloc();
main() {
	char *v[64]; int i;
	for (i = 0; i < 64; i++) {
		v[i] = malloc(64 + i * 7);
		if (v[i] == 0) { printf("fail %d\n", i); fflush(stdout); return 1; }
		v[i][0] = i;
	}
	for (i = 0; i < 64; i++) if (v[i][0] != i) { printf("clobber\n"); fflush(stdout); return 1; }
	for (i = 0; i < 64; i += 2) free(v[i]);
	printf("many ok\n"); fflush(stdout); return 0;
}
EOF

run 'printf %f' 'f=3.141590' <<'EOF'
#include <stdio.h>
main() { printf("f=%f\n", 3.14159); fflush(stdout); return 0; }
EOF

# THE COUNT ON strncat IS A BOUND ON THE READ, NOT ONLY ON THE WRITE.
#
# Upstream's strncat.C copied s2[n] and only then noticed --n < 0, overwriting
# the byte it had just copied with the NUL -- so the OUTPUT was always right
# and only the read went one past.  That is why nothing had ever noticed, and
# why the case below has to hand it an unreadable pointer rather than compare
# a string: a correct answer is not evidence.
#
# The authority is upstream's own assembler.  libc/gen/strncat.s -- the code a
# VAX actually ran -- opens `movl 12(ap),r8 / bleq L6', returning without
# touching s2 when n <= 0, and scans with a bounded `locc'.  The .C beside it
# calls itself "the `standard' for the C-library" and disagrees.  strcatn is
# the V7-named twin with the same body and no .s of its own.
#
# Address 1 is unmapped, so a program that reads s2 at all dies here; n == 0
# means "append nothing", so a correct strncat never looks.
run 'strncat with n==0 does not read s2' 'x|x' <<'EOF'
#include <stdio.h>
char b1[32], b2[32];
main() {
	strcpy(b1, "x");
	strcpy(b2, "x");
	strncat(b1, (char *)1, 0);
	strcatn(b2, (char *)1, 0);
	printf("%s|%s\n", b1, b2);
	fflush(stdout); return 0;
}
EOF

# ...and the bound must still be a bound: n shorter than s2 truncates, n longer
# stops at the NUL, and the result is terminated either way.
run 'strncat still truncates and terminates' 'abcdef|abcXYZ' <<'EOF'
#include <stdio.h>
char b1[32], b2[32];
main() {
	strcpy(b1, "abc");
	strncat(b1, "defghi", 3);	/* n shorter than the source */
	strcpy(b2, "abc");
	strncat(b2, "XYZ", 10);		/* n longer -- stop at the NUL */
	printf("%s|%s\n", b1, b2);
	fflush(stdout); return 0;
}
EOF

run 'string routines' 'len=5 cmp=0 cat=abcdef' <<'EOF'
#include <stdio.h>
main() {
	char b[32];
	strcpy(b, "abc");
	strcat(b, "def");
	printf("len=%d cmp=%d cat=%s\n", strlen("hello"), strcmp("a","a"), b);
	fflush(stdout); return 0;
}
EOF

run 'printf %e' 'e=3.141590e+04' <<'EOF'
#include <stdio.h>
main() { printf("e=%e\n", 31415.9); fflush(stdout); return 0; }
EOF

# --- %g, which had TWO defects and no case at all --------------------------
# Found by porting awk: tran.c:271 prints an integral value with "%.20g" and
# this port answered `3.0000000000000000000'.  Both halves are ours, and both
# have a VAX answer to restore in doprnt.S rather than a decision to make:
#
#   trailing zeros were never stripped   doprnt.S:625-631 (g1/g3), and
#                                        Berkeley's gcvt.c in the same directory
#   the e-style precision was not reduced   %g's precision counts SIGNIFICANT
#                                        digits, %e's counts digits after the
#                                        point, so %.Pg in e style is %.(P-1)e.
#                                        `scien' does `incl ndigit' on the way
#                                        in and `general' jumps past it.
#
# Neither is visible unless the value's last significant digit is a zero or the
# exponent form is reached, which is why 38 cases and every Wave C program had
# gone past them.  Measured against the host's printf over 40 combinations; the
# nine here are the ones that discriminate.
run 'printf %g strips trailing zeros' '3|3|15|0.5|0' <<'EOF'
#include <stdio.h>
main() { printf("%g|%.20g|%.20g|%.20g|%g\n", 3.0, 3.0, 15.0, 0.5, 0.0);
         fflush(stdout); return 0; }
EOF

# The e-style arm strips in the MANTISSA and keeps the exponent, which is why
# doprnt.S splits gfmte -> eedit -> g1 -> eexp rather than stripping at the end.
run 'printf %g strips before the exponent' '1e+20|1e-07|1e+06' <<'EOF'
#include <stdio.h>
main() { printf("%.20g|%g|%g\n", 1e20, 1e-7, 1000000.0);
         fflush(stdout); return 0; }
EOF

# %g's precision is significant digits.  Before the fix this line was
# `1.234567e+06' -- seven of them from a conversion that asked for six.
run 'printf %g precision counts significant digits' '1.23457e+06|1e+06|1.2e+06|123.456' <<'EOF'
#include <stdio.h>
main() { printf("%g|%.1g|%.2g|%.10g\n", 1234567.0, 1234567.0, 1234567.0, 123.456);
         fflush(stdout); return 0; }
EOF

# `#' is doprnt.S's numsgn, and g1 skips the strip when it is set -- ANSI's rule
# four years before ANSI.  It is the NEGATIVE CONTROL for the two cases above:
# a fix that stripped unconditionally passes both of them and fails this.
# `alt' was parsed at doprnt.c:128 and never passed to fmtfloat(), so before
# this work %#g and %g could not have differed whatever either printed.
run 'printf %#g keeps them' '3.00000|3.0000000000000000000|1.00000e+20' <<'EOF'
#include <stdio.h>
main() { printf("%#g|%#.20g|%#g\n", 3.0, 3.0, 1e20); fflush(stdout); return 0; }
EOF

# Rounding that carries into a new digit, and the width/sign flags over a
# stripped result -- the strip must not run before the padding is computed.
run 'printf %g rounds and pads' '10|10|[         3][3         ][+3][ 3]' <<'EOF'
#include <stdio.h>
main() { printf("%.3g|%.2g|[%10g][%-10g][%+g][% g]\n",
                9.999, 9.99, 3.0, 3.0, 3.0, 3.0); fflush(stdout); return 0; }
EOF

# --- string literals are WRITABLE -----------------------------------------
# 1985 C had no `const` and the tree relies on it: tr(1)'s nextc() ends with
# `if(c==0) *--s->p = 0;`, pushing the NUL back after reading past it, and
# `tr -d b` supplies only one string so the other is the literal "".  With
# literals in __TEXT,__cstring that was a SIGBUS.  See locnames[] in
# compiler/ccom-arm64/emit.c -- V8's own VAX back end puts them in .data too.
run 'writable string literals' 'Xbc/'  <<'EOF'
#include <stdio.h>
char *g = "abc";
main()
{
	char *p = "/";
	*g = 'X';		/* through a global initialiser */
	*p = *p;		/* and a local one: a store either way */
	printf("%s%s\n", g, p);
	fflush(stdout); return 0;
}
EOF

# --- setjmp/longjmp --------------------------------------------------------
# 82 longjmp and 68 setjmp calls across the command tree.  The VAX version
# walks call frames and cannot be ported; compiler/setjmp.s saves the AAPCS64
# callee-saved set instead.  What is checked here is what V8 code depends on:
# the value comes back from longjmp, longjmp(env,0) yields 1 not 0, and
# register variables live across the jump.
run 'setjmp/longjmp' 'first=0 back=7 zero=1 rv=99' <<'EOF'
#include <stdio.h>
#include <setjmp.h>
jmp_buf env;
deep(n) { longjmp(env, n); }
main()
{
	register int rv;
	int first, back, zero;

	rv = 99;			/* must survive the jump */
	first = setjmp(env);
	if (first == 0) deep(7);
	back = first;
	if ((zero = setjmp(env)) == 0) deep(0);	/* longjmp(env,0) -> 1 */
	printf("first=0 back=%d zero=%d rv=%d\n", back, zero, rv);
	fflush(stdout); return 0;
}
EOF

# --- ctype, the table cmp(1) needed ---------------------------------------
run 'ctype' 'alpha=1 digit=1 upper=0 space=1 punct=1' <<'EOF'
#include <stdio.h>
#include <ctype.h>
main()
{
	printf("alpha=%d digit=%d upper=%d space=%d punct=%d\n",
	    isalpha('q') != 0, isdigit('7') != 0, isupper('q') != 0,
	    isspace(' ') != 0, ispunct(',') != 0);
	fflush(stdout); return 0;
}
EOF

run 'atoi/atol/abs' 'i=-123 l=100000 a=5' <<'EOF'
#include <stdio.h>
long atol();
main() { printf("i=%d l=%ld a=%d\n", atoi("-123"), atol("100000"), abs(-5));
         fflush(stdout); return 0; }
EOF

# Note the local `s`: two occurrences of the same literal are two distinct
# objects now that literals live in writable data (see locnames[] in emit.c),
# so `p - "xxcxxc"` would subtract pointers into different objects.
run 'index/rindex/strtok' 'i=2 r=5 t1=ab t2=cd' <<'EOF'
#include <stdio.h>
char *index(), *rindex(), *strtok();
char buf[] = "ab:cd";
char s[] = "xxcxxc";
main()
{
	char *p, *q;
	p = index(s, 'c');
	q = rindex(s, 'c');
	printf("i=%d r=%d ", (int)(p - s), (int)(q - s));
	p = strtok(buf, ":");
	q = strtok((char *)0, ":");
	printf("t1=%s t2=%s\n", p, q);
	fflush(stdout); return 0;
}
EOF

# qsort is the one that would show a pointer-width bug: it does its own
# element-sized swapping through char pointers.
run 'qsort' '1 2 3 5 8 9' <<'EOF'
#include <stdio.h>
int cmpi(a, b) int *a, *b; { return *a - *b; }
main()
{
	int v[6], i;
	v[0]=9; v[1]=3; v[2]=8; v[3]=1; v[4]=5; v[5]=2;
	qsort((char *)v, 6, sizeof(int), cmpi);
	for (i = 0; i < 6; i++) printf(i ? " %d" : "%d", v[i]);
	printf("\n"); fflush(stdout); return 0;
}
EOF

V8TESTVAR=v8; export V8TESTVAR	# read back by the program below
run 'getenv' 'found=v8 missing=1' <<'EOF'
#include <stdio.h>
char *getenv();
main()
{
	char *p = getenv("V8TESTVAR");
	printf("found=%s missing=%d\n", p ? p : "(null)",
	    getenv("V8_NO_SUCH_VAR_HERE") == 0);
	fflush(stdout); return 0;
}
EOF

# --- opendir/readdir, V8's own, over the shim's V7 records -----------------
# V8's readdir is plain C doing read(2) of 16-byte records; the shim
# manufactures those from getdirentries64.  This is the two halves meeting.
mkdir -p dtest && : > dtest/alpha && : > dtest/beta
run 'opendir/readdir' 'found=2' <<'EOF'
#include <stdio.h>
#include <sys/types.h>
#include <ndir.h>
main()
{
	DIR *d;
	struct direct *e;
	int n = 0;

	if ((d = opendir("dtest")) == NULL) { printf("openfail\n"); return 1; }
	while ((e = readdir(d)) != NULL)
		if (e->d_name[0] == 'a' || e->d_name[0] == 'b') n++;
	closedir(d);
	printf("found=%d\n", n);
	fflush(stdout); return 0;
}
EOF

# --- THE THREE DIRSIZ, AND THE ONE THAT ACTUALLY DECIDES -------------------
# V8 spells the directory-name limit in three headers, each guarded by
# `#ifndef DIRSIZ' -- so whichever a program reaches FIRST wins, and the other
# two are silently ignored:
#
#	<dir.h>        struct dir      readdir(3)'s view
#	<sys/dir.h>    struct direct   the on-disk record, read(2) directly
#	<sys/param.h>  (no struct)     and this is the one that decides
#
# This port raises it from 14 to 254 (src/include/dir.h says why).  Only the
# first two were patched at first, which changed nothing at all for the seven
# commands that read directories raw -- w.c and ps.h both include <sys/param.h>
# BEFORE <sys/dir.h>, so they kept 14 while the shim wrote 256-byte records.
#
# The include order below is deliberately the one that used to lose.
run 'DIRSIZ agrees whichever header is reached first' 'param=254 rec=256' <<'EOF'
#include <stdio.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/dir.h>
main()
{
	struct direct d;
	printf("param=%d rec=%d\n", DIRSIZ, sizeof d);
	fflush(stdout); return 0;
}
EOF

# --- a directory's size is the size of what read(2) gives ------------------
# ps(1)'s getdir() is the pattern: open, fstat, size an array as
# st_size/sizeof(struct direct), then require read(fd, dp, st_size) to return
# exactly st_size.  The host's st_size is unrelated to the record stream the
# shim builds -- an APFS directory of nine entries reports 288 while the shim
# produces 2304 -- so this read the first 288 bytes and called it the answer.
#
# Every OTHER reader in the tree loops until EOF and never noticed, which is
# why the mismatch survived: it is only visible to code that trusts the size.
mkdir -p sztest && : > sztest/one && : > sztest/two && : > sztest/three
run 'fstat sizes a directory as read(2) will fill it' 'entries=5 exact=1' <<'EOF'
#include <stdio.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/dir.h>
main()
{
	struct stat sb;
	static char buf[65536];
	int fd, n;
	long got;

	if ((fd = open("sztest", 0)) < 0) { printf("openfail\n"); return 1; }
	fstat(fd, &sb);
	n = sb.st_size / sizeof(struct direct);	/* what getdir computes */
	got = read(fd, buf, sb.st_size);	/* what getdir demands */
	close(fd);
	printf("entries=%d exact=%d\n", n, got == sb.st_size);
	fflush(stdout); return 0;
}
EOF

run 'memops' 'cpy=abc set=zzz cmp=0 lt=-1 chr=1 ccpy=3 miss=1' <<'EOF'
#include <stdio.h>
char *memcpy(), *memset(), *memchr(), *memccpy();
char src[] = "xay";
main()
{
	char a[8], b[8], c[8], *p;
	int chr, ccpy, miss;

	memcpy(a, "abc", 4);
	memset(b, 'z', 3); b[3] = 0;
	p = memchr(src, 'a', 3);
	chr = p - src;
	/* the result must be read out before printf, since memccpy writes c */
	ccpy = memccpy(c, "qrs", 's', 8) - c;
	miss = memccpy(c, "qrs", 'Z', 3) == 0;
	printf("cpy=%s set=%s cmp=%d lt=%d chr=%d ccpy=%d miss=%d\n", a, b,
	    memcmp("abc", "abc", 3), memcmp("abc", "abd", 3) < 0 ? -1 : 0,
	    chr, ccpy, miss);
	fflush(stdout); return 0;
}
EOF


# --- a ternary whose value is a DOUBLE --------------------------------------
# condit() lowers `a ? b : c` into branches that rendezvous in one register.
# lvstore() always wrote a floating value to d0 correctly, but both readers --
# the QNODE case and the GENLAB join -- took it out of x0 regardless, so a
# double-valued ternary came back as whatever integer happened to be there.
#
# eqn found it. `max(x,y)` is a ternary, and
#
#	printf(".nr 10 %gm\n", max(REL(...), 0))
#
# handed printf 0xfff0000000000000 -- negative infinity -- whose digits ecvt
# then tried to generate until it ran off the end of its buffer.
run 'double-valued ternary' 'max=7.25 min=2.5 mixed=1.5' <<'EOF'
#include <stdio.h>
#define max(x,y) (((x) >= (y)) ? (x) : (y))
double dv(x) double x; { return x; }
main()
{
	double a, b;

	a = 2.5; b = 7.25;
	printf("max=%.3g min=%.2g mixed=%.2g\n",
	    max(a,b), max(a,b) == b ? a : b, max(dv(1.5), 0));
	fflush(stdout); return 0;
}
EOF


# --- more arguments than there are registers --------------------------------
# The whole varargs design rests on this: printf takes &args and walks forward,
# so the eight spilled argument registers must read as one contiguous array with
# the caller's stack arguments above them.  The prologue allocates the spill
# block BEFORE pushing x29/x30 for exactly that reason (see the frame diagram in
# compiler/ccom-arm64/local.c).  Nine arguments is the first call that crosses
# the boundary -- fmt in x0, eight varargs in x1-x7 and then the stack.
run 'printf across the register boundary' '1 a 2 b 3 c 4 d 999' <<'EOF'
#include <stdio.h>
main()
{
	printf("%d %s %d %s %d %s %d %s %d\n",
	    1, "a", 2, "b", 3, "c", 4, "d", 999);
	fflush(stdout); return 0;
}
EOF

# and the same with a double in the spilled position
run 'a double past the boundary' '1 2 3 4 5 6 7 2.5' <<'EOF'
#include <stdio.h>
main()
{
	printf("%d %d %d %d %d %d %d %.2g\n", 1, 2, 3, 4, 5, 6, 7, 2.5);
	fflush(stdout); return 0;
}
EOF


# --- a call with several live FP registers ----------------------------------
# The caller-save area and the eight scratch slots that arguments 0-7 pass
# through share one block per call-nesting depth, and the block was 56 bytes too
# small: argslot() puts argument 0 at `sbase + SAVEBLK - 64`, which with the old
# SAVEBLK of 128 landed on the ninth save onward.  So a call made with two or
# more live FP registers wrote a saved register over its own first argument.
#
# eqn found it.  REL() calls EFFPS(ps) with two doubles live, got a bit pattern
# where an int should have been, divided by it, and handed printf an infinity.
run 'call with live FP registers' 'r=2 d=1.5 e=2.5 f=3.5' <<'EOF'
#include <stdio.h>
int takesint(n) int n; { return n * 2; }
double f3(a, b, c) double a, b, c; { return a + b + c; }
main()
{
	double d, e, g;
	int r;

	d = 1.5; e = 2.5; g = 3.5;
	/* three doubles live across a call taking an int */
	r = takesint(1);
	printf("r=%d d=%.2g e=%.2g f=%.2g\n", r, d, e, g);
	fflush(stdout); return 0;
}
EOF

# --- no V8 program may resolve a VARIADIC function from the host -----------
#
# This has bitten three times, each time looking like something else:
#
#   scanf   spellin's scanf("%lo") faulted inside __svfscanf_l
#   execl   system() started an INTERACTIVE /bin/sh, so refer looked hung
#   printf  cc -o linked the host libc entirely, printing 1839618368 for 42
#
# The mechanism is always the same.  A function missing from libv8c.a does not
# fail the link: it is resolved from -lSystem.  For a NON-variadic function that
# is usually harmless and the gap stays hidden.  For a variadic one it is an ABI
# mismatch -- v8cc passes every argument in x0-x7, Apple's ARM64 ABI passes the
# variadic arguments of a call on the stack -- so the callee reads rubbish.
#
# Checking the shape rather than waiting to trip over it again.  Anything on
# this list must come from V8's own libc; if one is undefined in a linked
# program, it will be satisfied by the host at run time.
VARIADIC="printf fprintf sprintf scanf fscanf sscanf execl execle execlp"

cat > vt.c <<'EOF'
main()
{
	char b[64];
	sprintf(b, "%d", 1);
	printf("%s\n", b);
	sscanf("2", "%d", &b[0]);
	system("true");		/* pulls execl */
	exit(0);
}
EOF
if "$CC" -c vt.c 2>vt.err && \
   clang -nostdlib -e _v8start -o vt "$CRT" vt.o "$LIBC" "$STUBS" "$SHIM" -lSystem 2>>vt.err; then
	leaked=
	for f in $VARIADIC; do
		if nm -u vt 2>/dev/null | grep -q "^_$f\$"; then
			leaked="$leaked $f"
		fi
	done
	if [ -z "$leaked" ]; then
		pass=$((pass+1))
	else
		fail=$((fail+1))
		echo "FAIL variadic libc functions left for the host:$leaked"
	fi
else
	fail=$((fail+1)); echo "FAIL variadic-leak probe did not build"
	cat vt.err
fi

# --- atof, which is new code rather than ported ----------------------------
#
# libc/gen/atof.s is 319 lines of VAX D-format assembly, so src/libc/gen/atof.c
# replaces it the way doprnt.c and ieeefp.c replace theirs.  New code needs its
# own guard: the ones below are compared against the host's atof BY BIT PATTERN,
# with integer equality, so that nothing about the comparison depends on the
# formatting code.  That was arrived at the hard way -- the first attempt to
# verify atof looked like two wrong answers, and they turned out to be %lx
# dropping a digit.  %lx is fixed now (see the two cases above, and PLAN.md
# S4g), but the reason to compare bit patterns rather than printed strings did
# not depend on that bug and does not go away with it.
#
# Correctly rounded in the exact window (17 significant digits or fewer, decimal
# exponent within +-22), which covers everything in this tree.  Two of these are
# the boundary itself, so a change to that window shows up here.
run 'atof exact cases' 'exact exact exact exact exact exact exact exact' <<'EOF'
#include <stdio.h>
extern double atof();
struct { char *s; long bits; } t[] = {
	{ "0.5",       0x3fe0000000000000L },
	{ "-1.25",     0xbff4000000000000L },
	{ "0.1",       0x3fb999999999999aL },
	{ "-0.0001",   0xbf1a36e2eb1c432dL },
	{ "1.5e-5",    0x3eef75104d551d69L },
	{ "  +42.75",  0x4045600000000000L },	/* leading space and a sign */
	{ "1e22",      0x4480f0cf064dd592L },	/* last exactly representable */
	{ "3.14159265358979", 0x400921fb54442d11L },
	{ 0, 0 }
};
main()
{
	int i; double d; long *lp;

	for (i = 0; t[i].s; i++) {
		d = atof(t[i].s);
		lp = (long *)&d;
		printf("%s%s", i ? " " : "", *lp == t[i].bits ? "exact" : "WRONG");
	}
	printf("\n"); fflush(stdout); return 0;
}
EOF

# Overflow must reach infinity and underflow must reach zero.  An earlier
# version clamped the exponent to bound its loop, which turned atof("1e400")
# into 1.0000000000000007e+308 -- finite, plausible, and wrong.
run 'atof overflows rather than saturating' 'inf 0 0' <<'EOF'
#include <stdio.h>
extern double atof();
main()
{
	double big = atof("1e400"), small = atof("1e-400"), none = atof("abc");
	long *b = (long *)&big, *s = (long *)&small, *n = (long *)&none;

	printf("%s %s %s\n",
	    *b == 0x7ff0000000000000L ? "inf" : "NOTINF",
	    *s == 0 ? "0" : "NOTZERO",
	    *n == 0 ? "0" : "NOTZERO");
	fflush(stdout); return 0;
}
EOF

# --- toupper and tolower, which are functions here and macros elsewhere ----
# Both were resolving from libSystem until tests/kmemu noticed; V8 has them in
# C, so nothing needed writing, only building.
run 'toupper/tolower' 'ABC abc 9 - 9 -' <<'EOF'
#include <stdio.h>
main()
{
	printf("%c%c%c %c%c%c %c %c %c %c\n",
	    toupper('a'), toupper('b'), toupper('C'),
	    tolower('A'), tolower('B'), tolower('c'),
	    toupper('9'), toupper('-'), tolower('9'), tolower('-'));
	fflush(stdout); return 0;
}
EOF

# --- sleep, the function that found the signal bug -------------------------
#
# V8's libc/gen/sleep.c was imported alongside the others and taken straight
# back out, because it hung: it is alarm + a handler + `for(;;) pause()', and
# no V8 program in this port could catch a signal.  v8s_signal handed the raw
# sigaction syscall a userland `struct sigaction' where the kernel wants
# `struct __sigaction' -- 24 bytes, with a signal-trampoline pointer at offset
# 8, exactly where the userland struct keeps sa_mask -- so every handler was
# installed with a null trampoline and delivery went to address 0.  It is here
# now because that is fixed; shim/v8sys/sigtramp.s is the missing piece.
#
# The case has its own deadline because its failure mode is a HANG, and it is
# the whole of sleep.c that is under test: setjmp, a longjmp out of a signal
# handler, pause(), and alarm's return value.  macOS has no timeout(1), hence
# the four lines.
runlimit() {
	_lim=$1; shift
	"$@" & _rpid=$!
	( sleep "$_lim"; kill -9 $_rpid ) >/dev/null 2>&1 & _wpid=$!
	wait $_rpid; _rc=$?
	kill $_wpid >/dev/null 2>&1
	return $_rc
}

# A PENDING ALARM SURVIVES THE SLEEP, which is the reason sleep.c needs alarm
# to report the time remaining: `altime = alarm(1000)' saves what the caller
# had running and `alarm(altime)' puts it back.  While v8s_alarm returned a
# constant 0 this printed "lost" -- sleep silently cancelled its caller's
# alarm -- and no amount of watching sleep(1) take a second would have said so.
cat > t.c <<'EOF'
#include <stdio.h>
main()
{
	int left;

	alarm(30);
	sleep(1);
	left = alarm(0);
	printf("%s\n", left >= 27 && left <= 30 ? "kept" : "lost");
	fflush(stdout);
	return 0;
}
EOF
if "$CC" -c t.c 2>e.log &&
   clang -nostdlib -e _v8start -o t "$CRT" t.o "$LIBC" "$STUBS" "$SHIM" -lSystem 2>>e.log; then
	runlimit 10 ./t >out.txt 2>&1; rc=$?
	if [ "$(cat out.txt)" = "kept" ]; then pass=$((pass+1))
	else
		fail=$((fail+1)); echo "FAIL sleep returns, and keeps the caller's alarm"
		echo "  exit $rc, output [$(cat out.txt)] -- exit 137 is the deadline killing a hang"
	fi
else
	fail=$((fail+1)); echo "FAIL sleep (build)"; head -2 e.log
fi

# ---------------------------------------------------------------------------
# %.Ns MUST NOT READ s[N], and this file is where that bug lived.
#
# doprnt.c is OUR C rewrite of doprnt.S, so this one is the port's rather than
# V8's.  It was written
#
#	for (len = 0; s[len]; len++)
#		if (haveprec && len >= prec) break;
#
# -- the loop CONDITION runs before the body, so the byte at s[prec] is read
# and then discarded.  Harmless for a NUL-terminated string and not harmless
# for the case %.Ns exists to serve: a FIXED-WIDTH FIELD THAT NEED NOT BE
# TERMINATED.  V8's ncheck prints directory entries with "%.14s" over a d_name
# that fills its record, so the byte read is the next entry's d_ino, and at the
# end of a mapped page it is a fault.  Found by auditing ncheck before building
# it; nothing in the tree had reached it before, which is why 32 cases of
# printf did not.
#
# The diagnostic is prec = 0, where the pointer can be made unmapped without
# any page arrangement: page 0 is unmapped on macOS, so the old loop faults on
# *(char *)1 and the fixed one never dereferences at all.  Same defect, same
# line, and deterministic on every host rather than dependent on where the
# linker put a buffer.
# AND %s OF A NULL POINTER IS THE EMPTY STRING, WHICH IS THE VAX'S ANSWER
# RATHER THAN THE ABSENCE OF A FAULT.  This file printed "(null)", which V8
# never produced: doprnt.S has NO null guard -- its six occurrences of the word
# are comments about NUL bytes in the format scan -- so %s read virtual address
# 0 and stopped on the byte there.  That byte is 0x00, measured on the shipped
# binaries: V8 a.out is ZMAGIC, so a.out.h makes N_TXTOFF 1024, the header is
# never mapped, and bin/cat, bin/ls and usr/bin/egrep all begin
# `00 00 c2 08 5e d0 ae 08' at virtual 0.  The convention is real and belongs
# to other programs' PRIVATE copies of 4BSD's doprnt (cmd/ex/ovdoprnt.s has
# `nulstr: <(null)\0>', cmd/csh/doprnt.c has its own) rather than to libc.
#
# THE WIDTH CASE IS THE DISCRIMINATOR.  A "fix" that skipped the whole %s arm
# for a null pointer prints [] for the first and [] for the second; the VAX
# padded the field, because it found a zero-length string rather than no
# string at all.
run 'printf %s of a null pointer is empty' '[]|[]' <<'EOF'
#include <stdio.h>
main() { printf("[%s]|[%.3s]\n", (char *)0, (char *)0); fflush(stdout); return 0; }
EOF
run 'and a width still pads it' '[     ][ab   ]' <<'EOF'
#include <stdio.h>
main() { printf("[%5s][%-5s]\n", (char *)0, "ab"); fflush(stdout); return 0; }
EOF

run 'printf %.0s does not dereference' 'ok' <<'EOF'
#include <stdio.h>
main() { printf("%.0s", (char *)1); printf("ok\n"); fflush(stdout); return 0; }
EOF

# And the behaviour it must keep while not reading that byte.  `field' is 14
# bytes with NO terminator, followed by a sentinel the format must not reach.
run 'printf %.Ns over an unterminated field' '[abcdefghijklmn] [abc] [xy] [] 14' <<'EOF'
#include <stdio.h>
struct { char field[14]; char sentinel[4]; } b = { {
	'a','b','c','d','e','f','g','h','i','j','k','l','m','n' }, "ZZZ" };
main() {
	printf("[%.14s] [%.3s] [%.9s] [%.0s] %d\n",
	    b.field, "abcdef", "xy", "abc", 14);
	fflush(stdout); return 0;
}
EOF

# ---------------------------------------------------------------------------
# fopen("/dev/tty") -- the call three programs in this tree actually make, and
# the level the shim's own suite cannot reach.  tests/v8sys checks v8s_open;
# pr.c:201, troff/hc.c:766 and dump/dumpoptr.c:36 all go through stdio.
#
# V8's /dev/tty is /dev/fd/3, so the answer depends on fd 3 and on nothing else.
# THE PROGRAM ARRANGES ITS OWN fd 3 rather than inheriting one: whether this
# harness leaves one open is a property of the machine, and every case here has
# to be a relation the port controls.
run 'fopen /dev/tty follows fd 3' 'closed 1 open 1 first A' <<'EOF'
#include <stdio.h>
main()
{
	FILE *f;
	int fd;

	close(3);
	f = fopen("/dev/tty", "r");
	printf("closed %d ", f == 0);		/* fd.4: open returns -1 */

	fd = creat("ttyprobe", 0644);
	write(fd, "ABC", 3);
	close(fd);
	fd = open("ttyprobe", 0);
	dup2(fd, 3);				/* init.c:381's third dup */
	if (fd != 3) close(fd);

	f = fopen("/dev/tty", "r");
	printf("open %d ", f != 0);
	printf("first %c\n", getc(f));
	fflush(stdout);
	unlink("ttyprobe");
	return 0;
}
EOF

# ...and the same through the numeric spelling, which is the node /dev/tty is a
# link to.  Two names, one descriptor: reading one advances the other.
run '/dev/fd/3 is the same node' 'a=A b=B' <<'EOF'
#include <stdio.h>
main()
{
	FILE *x, *y;
	int fd;

	fd = creat("fdprobe", 0644);
	write(fd, "AB", 2);
	close(fd);
	fd = open("fdprobe", 0);
	dup2(fd, 3);
	if (fd != 3) close(fd);

	x = fopen("/dev/tty", "r");
	y = fopen("/dev/fd/3", "r");
	/* Unbuffered, or stdio's own 4096-byte read hides the sharing. */
	setbuf(x, (char *)0);
	setbuf(y, (char *)0);
	printf("a=%c b=%c\n", getc(x), getc(y));
	fflush(stdout);
	unlink("fdprobe");
	return 0;
}
EOF

echo "libv8c: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# FIXED: printf("%f") -- it was lvalues being evaluated twice.
#
# Read-modify-write operators (`x op= y`, `x++`, `--x`) must read the lvalue and
# store back to it. The back end generated the lvalue once for each half, which
# re-ran any side effect inside it. V8's ecvt() rounding loop contains
#
#	if (p1>buf) ++*--p1;
#
# and that decremented p1 twice and incremented through it twice:
#
#	"123" with p1 at [2]  ->  expected "133" p1 at [1]
#	                          got      "323" p1 at [0]
#
# lvaddr()/lvload()/lvstore() in gencode.c now materialise the address once and
# both halves work through it.
#
# How it was found, which is the reusable part: instrumenting cvt() showed BOTH
# compilers entering the rounding tail with identical state --
#
#	TAIL p=8 p1=7 r2=1 dec=1 buf=[31415899]
#
# -- and leaving with different answers, which localised the bug to ten lines.
# Everything else had been eliminated by testing it standalone: the digit loop
# and modf (both give the same 1415899 under clang, so the trailing 899 is just
# binary floating point), 64-bit shifts and masks, `*p1 += 5`, `*p++ = c`, and
# `p1 = &buf[n]; p1 += r2`. What was left was the one construct never tested
# alone -- and it failed the moment it was.
# ---------------------------------------------------------------------------
