#!/bin/sh
# Wave C: the document tools.

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NROFF=$ROOT/build/stage0/nroff/nroff
V8ROOT=$ROOT/rootfs; export V8ROOT
TMP=${TMPDIR:-/tmp}/wavec.$$
mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT; cd "$TMP" || exit 1

pass=0 fail=0
check() {
	if [ "$2" = "$3" ]; then pass=$((pass+1))
	else fail=$((fail+1)); echo "FAIL $1"; echo "  want [$2]"; echo "  got  [$3]"; fi
}
[ -x "$NROFF" ] || { echo "missing $NROFF -- run make nroff"; exit 1; }

# nroff reads its terminal table from /usr/lib/term/tab.37, which only exists
# because the shim resolves V8 data paths inside $V8ROOT -- see rootpath() in
# shim/v8sys/syscall.c.  Running it at all proves that seam works.
printf 'hello nroff world\n.br\nsecond line here\n' > t1
check 'nroff formats text' 'hello nroff world second line here' \
    "$("$NROFF" t1 | tr -s ' \n' ' ' | sed 's/^ //;s/ $//')"

printf '.ll 20\nalpha beta gamma delta epsilon\n' > t2
check 'nroff honours .ll' '2' "$("$NROFF" t2 | grep -c .)"

printf '.ce\ncentred\n' > t3
check 'nroff centres' 'centred' "$("$NROFF" t3 | tr -d ' \n')"

printf 'a\n.sp\nb\n' > t4
check 'nroff spaces' '2' "$("$NROFF" t4 | grep -c .)"

printf '.na\none two\n' > t5
check 'nroff no-adjust' 'one two' "$("$NROFF" t5 | tr -s ' \n' ' ' | sed 's/^ //;s/ $//')"


# troff, whose device tables `make rootfs` compiles with makedev.  Its output is
# the device-independent stream a typesetter driver consumes, not text, so the
# test looks at the header it must always emit.
TROFF=$ROOT/build/stage0/troff/troff
if [ -x "$TROFF" ]; then
	"$TROFF" t1 > tr.out 2>tr.err
	check 'troff names the device' 'x T 202' "$(head -1 tr.out)"
	check 'troff states resolution' 'x res 972 1 2' "$(sed -n 2p tr.out)"
	check 'troff initialises' 'x init' "$(sed -n 3p tr.out)"
	check 'troff emits no errors' '' "$(cat tr.err)"
else
	fail=$((fail+4)); echo "FAIL troff not built"
fi

echo "wavec: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
