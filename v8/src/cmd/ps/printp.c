#include "ps.h"

char *
printp(cp, fd, pp, up)
register char *cp; register struct proc *pp; register struct user *up;
{
	char sstr[64], *args; int nargs = 56;
	register time;

	if (fd < 0)
		return cp;

	if (pp->p_flag & SPAGE)
		sstr[0] = 'P';
	else if (pp->p_stat == SSLEEP)
		sstr[0] = "IS"[pp->p_clktim && pp->p_clktim < 20];
	else
		sstr[0] = "?swRLZT?"[minmax(pp->p_stat, 0, 7)];
	sstr[1] = (pp->p_flag&SLOAD) ? ' ' : 'W';
	sstr[2] = " N"[pp->p_nice > NZERO];
	sstr[3] = 0;
	if (Tflag) {
		sstr[3] = ' ';
		strcpy(sstr+4,ctime(&up->u_start)+4);
		sstr[16] = 0;
	}
	time = (up->u_vm.vm_utime+up->u_vm.vm_stime)/60;

	if (uflag) {
		cp += sprintf(cp, "%-7.7s %4.1f ",
			getuname(pp->p_uid), 100.0*pp->p_pctcpu);
		nargs -= 13;
	}

	if (lflag) {
		/*
		 * PORT: %4D, not %4d, for the two sizes.  p_dsize, p_ssize and
		 * p_rssize are size_t, which is 8 bytes here and was 4 on the
		 * VAX, so %d truncates them to 32 bits -- and the largest size
		 * this prints on the development machine is 1,959,395,120 KB
		 * against an INT_MAX of 2,147,483,647.  A process reserving 2 TB
		 * of address space, which is routine on macOS and impossible on
		 * a VAX, prints NEGATIVE.  %D is V8's own long conversion
		 * (doprnt.c:189), so this is the port's spelling of the same
		 * intent rather than an ANSI import.
		 */
		cp += sprintf(cp, "%4D %4D %5d %5x ",
			(pp->p_dsize+pp->p_ssize)*NBPG/1024,
			pp->p_rssize*NBPG/1024,
			pp->p_ppid, (int)pp->p_wchan & 0xfffff);
		nargs -= 22;
	}
	args = getargs(fd, pp, up);
	cp += sprintf(cp, "%5d %-5.5s %s %3d:%02d %.*s\n",
		pp->p_pid, gettty(up->u_ttyino), sstr,
		time/60, time%60, nargs, args);

	if (fflag)
		cp = fdprint(cp, pp, up);

	close(fd);
	return cp;
}
