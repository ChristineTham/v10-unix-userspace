/*
 * /unix and /dev/kmem -- the two files this library is actually named for.
 *
 * Host libc appears here for sysctl(3) and nothing else, on the same terms as
 * utmp.c and mtab.c.  synth.c has the boundary.
 *
 * WHAT load(1) DOES, and why it needs both.  It does not ask the system for the
 * load average; it looks up the ADDRESS of the kernel's `_avenrun' in the
 * kernel's own symbol table, then seeks to that address in /dev/kmem and reads
 * three doubles out of it.  That is how every groveler on a V7-shaped system
 * worked, and w(1) and ps(1) do the same with more symbols.
 *
 * So the shim manufactures a kernel: a namelist at /unix saying where things
 * are, and a /dev/kmem in which they are there.  The addresses are ours to
 * choose -- nothing else in this world has an opinion about them -- so the
 * table below assigns them, and both files are generated from it.  Get the two
 * out of step and the program reads the wrong bytes and prints them without
 * complaint, which is why one table drives both rather than two lists agreeing
 * by hand.
 *
 * A REAL a.out HEADER, because nlist(3) insists.  src/libc/gen/nlist.c checks
 * N_BADMAG, then reads a symbol table at N_SYMOFF and strings after it.  It is
 * authentic V8 libc and this port compiles it unchanged, so the file it reads
 * has to be the format it expects rather than something convenient.
 */

#include "kmemu.h"
#include <sys/types.h>
#include <sys/sysctl.h>

/*
 * V8's <a.out.h> structures, spelled again for the same reason utmp.c spells
 * struct utmp: this file is clang-compiled for the host and those headers
 * belong to the V8 include tree.
 *
 * UNDER LP64 THESE ARE NOT THEIR 1985 SIZES.  Every field in struct exec is a
 * long, so the header is 64 bytes where the VAX had 32; and struct nlist is 24,
 * its 8-byte union followed by four bytes of type/other/desc, four of padding,
 * and an 8-byte value.  Nothing here has to interoperate with a 1985 file -- the
 * only reader is a V8 program compiled against the same headers by v8cc -- but
 * the two ends must agree, and a disagreement produces a plausible number
 * rather than a failure.  tests/kmemu asserts both sizes from the V8 side.
 */
struct v8exec {
	long		a_magic;
	unsigned long	a_text, a_data, a_bss, a_syms, a_entry;
	unsigned long	a_trsize, a_drsize;
};

struct v8nlist {
	long		n_strx;		/* the n_un union, as a file offset */
	unsigned char	n_type;
	char		n_other;
	short		n_desc;
	short		pad[2];		/* to align n_value; the compiler's */
	unsigned long	n_value;
};

#define V8_OMAGIC	0407
#define V8_N_DATA	0x6
#define V8_N_EXT	01

/*
 * THE TABLE.  Address, size, and how to fill it -- one row per kernel symbol
 * this port can answer for honestly.
 *
 * Addresses start at 0x1000 rather than 0, only so that a wild n_value of 0
 * cannot look like a valid answer while debugging.  Nothing depends on the
 * value; load(1) seeks to whatever the namelist says.
 *
 * When w(1) and ps(1) arrive they add rows here -- _bootime, _nproc, _proc --
 * and get their namelist entries for free.  A symbol with no honest source does
 * NOT get a row: it is then absent from /unix, nlist leaves n_type zero, and
 * the program says so in its own words.  That is PLAN.md section 7's rule about
 * sentinels, applied one level down: better a groveler that reports it cannot
 * find a symbol than one handed a fabricated value.
 */
static int fill_avenrun(char *dst, long len);

static struct ksym {
	const char	*name;
	unsigned long	addr;
	long		len;
	int		(*fill)(char *, long);
} ksyms[] = {
	{ "_avenrun", 0x1000, 3 * (long)sizeof(double), fill_avenrun },
	{ 0, 0, 0, 0 }
};

/*
 * The load average, from the one interface that reports it.  vm.loadavg is a
 * struct loadavg: three fixed-point values and the scale to divide them by.
 */
static int
fill_avenrun(char *dst, long len)
{
	struct loadavg la;
	size_t n = sizeof la;
	double *out = (double *)dst;
	int i;

	if (len < 3 * (long)sizeof(double)) return (-1);
	if (sysctlbyname("vm.loadavg", &la, &n, (void *)0, 0) < 0) return (-1);
	if (la.fscale == 0) return (-1);
	for (i = 0; i < 3; i++)
		out[i] = (double)la.ldavg[i] / (double)la.fscale;
	return (0);
}

/*
 * /unix: a.out header, symbol table, string table.
 *
 * The string table starts with four bytes of filler because nlist(3) SKIPS any
 * symbol whose n_strx is 0 -- it uses zero to mean "no name".  Real linkers put
 * the table's own length there; anything non-empty does, and the four bytes are
 * cheaper than explaining why the first symbol vanished.
 */
int
kmemu_unix(const char *hostpath)
{
	static char buf[4096];
	struct v8exec *e = (struct v8exec *)buf;
	struct v8nlist *sym;
	long nsym = 0, symoff, stroff, n, i, k;

	for (nsym = 0; ksyms[nsym].name; nsym++)
		;
	symoff = (long)sizeof *e;
	stroff = symoff + nsym * (long)sizeof(struct v8nlist);

	for (i = 0; i < (long)sizeof buf; i++) buf[i] = 0;
	e->a_magic = V8_OMAGIC;
	e->a_syms = (unsigned long)(nsym * (long)sizeof(struct v8nlist));
	/* a_text, a_data, a_trsize and a_drsize stay 0, so N_SYMOFF is just
	 * N_TXTOFF -- sizeof(struct exec) for OMAGIC.  There is no text. */

	sym = (struct v8nlist *)(buf + symoff);
	n = stroff + 4;				/* past the string filler */
	for (i = 0; i < nsym; i++) {
		sym[i].n_strx = n - stroff;
		sym[i].n_type = V8_N_DATA | V8_N_EXT;
		sym[i].n_value = ksyms[i].addr;
		for (k = 0; ksyms[i].name[k] && n < (long)sizeof buf - 1; k++)
			buf[n++] = ksyms[i].name[k];
		buf[n++] = '\0';
	}
	return (kmemu_replace(hostpath, buf, n));
}

/*
 * /dev/kmem: the addresses in /unix, made real.
 *
 * The file is as long as the highest address plus its symbol, which for one
 * symbol at 0x1000 is a sparse-looking 4120 bytes of mostly zero.  That is
 * fine and it is also honest -- everything between the symbols is memory this
 * port cannot speak for, and reading it gets zeroes rather than a guess.
 *
 * A SNAPSHOT PER open(2), not a live window.  The shim regenerates this when a
 * reader opens it, so `load' with no interval -- the default -- reads the
 * current average.  `load 5' loops on the SAME descriptor, so every iteration
 * re-reads the same three numbers.  A real /dev/kmem would show them moving.
 * Recorded in src/cmd/load/PORTING.md rather than worked around, because the
 * workaround is a device driver.
 */
int
kmemu_kmem(const char *hostpath)
{
	static char buf[65536];
	long end = 0, i;

	for (i = 0; ksyms[i].name; i++) {
		long top = (long)ksyms[i].addr + ksyms[i].len;
		if (top > end) end = top;
	}
	if (end > (long)sizeof buf) return (-1);
	for (i = 0; i < end; i++) buf[i] = 0;

	for (i = 0; ksyms[i].name; i++)
		if (ksyms[i].fill &&
		    ksyms[i].fill(buf + ksyms[i].addr, ksyms[i].len) < 0)
			return (-1);	/* no honest value: no file at all */

	return (kmemu_replace(hostpath, buf, end));
}
