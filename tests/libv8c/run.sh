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
