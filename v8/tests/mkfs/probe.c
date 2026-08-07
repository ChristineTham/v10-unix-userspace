/*
 * The on-disk formats, measured by v8cc through this port's own headers.
 *
 * WHY A PROBE AND NOT A _Static_assert.  These structures are read by three
 * compilers, not one: v8cc here, clang in the shim, and -- if the SIMH
 * cross-check in PLAN.md section 8a step 6 ever runs -- V8's own pcc on a VAX.
 * A static assertion sees whichever compiler contains it and can say nothing
 * about the others, which is the same reason tests/kmemu measures struct proc
 * from the V8 side rather than trusting procfs.c's assertions.
 *
 * Every number this prints has a source that is not this port: sizeof(dinode)
 * is 64 because <sys/param.h> hardcodes INOPB 16 for a 1024-byte block, and
 * NINDIR is 256 because the same file hardcodes NMASK 0377 and NSHIFT 8.  The
 * runner compares them against each other for that reason -- a self-consistency
 * check catches a drifting type without anyone having to remember the value.
 */
#include <sys/types.h>
#include <sys/param.h>
#include <sys/ino.h>
#include <sys/filsys.h>
#include <sys/fblk.h>
#include <sys/dir.h>

main()
{
	daddr_t a[4];
	daddr_t back[4];
	char packed[12];
	int i;

	printf("daddr %d\n",	(int)sizeof(daddr_t));
	printf("dinode %d\n",	(int)sizeof(struct dinode));
	printf("filsys %d\n",	(int)sizeof(struct filsys));
	printf("fblk %d\n",	(int)sizeof(struct fblk));
	printf("direct %d\n",	(int)sizeof(struct direct));
	printf("dirsiz %d\n",	DIRSIZ);
	printf("bsize %d\n",	BSIZE(0));
	printf("inopb %d\n",	INOPB(0));
	printf("nindir %d\n",	(int)NINDIR(0));
	printf("nmask %d\n",	NMASK(0));
	printf("nshift %d\n",	NSHIFT(0));
	printf("nicfree %d\n",	NICFREE);

	/* what INOPB would be if it were computed rather than hardcoded */
	printf("inopbcalc %d\n", (int)(BSIZE(0)/sizeof(struct dinode)));
	/* and what NINDIR would be if NMASK were the authority */
	printf("nmaskplus %d\n", NMASK(0)+1);

	/*
	 * ltol3 and l3tol, round tripped.  l3tol has no caller in this port --
	 * fsck and icheck are its customers and neither is imported -- so this
	 * is the only thing that will notice if the two stop agreeing, and
	 * they were made to disagree once already by a change to daddr_t.
	 * 0xffffff is the largest address three bytes hold; 0x123456 uses all
	 * three and would survive a two-byte stride, which 0xffffff would too,
	 * so the fourth element is a zero that a wrong stride turns nonzero.
	 */
	a[0] = 0x123456; a[1] = 1; a[2] = 0xffffff; a[3] = 0;
	for (i = 0; i < 12; i++)
		packed[i] = 0125;
	ltol3(packed, a, 4);
	for (i = 0; i < 4; i++)
		back[i] = -1;
	l3tol(back, packed, 4);
	printf("l3back %d %d %d %d\n", back[0], back[1], back[2], back[3]);
	printf("l3bytes %d %d %d\n",
	    packed[0]&0377, packed[1]&0377, packed[2]&0377);
	exit(0);
}
