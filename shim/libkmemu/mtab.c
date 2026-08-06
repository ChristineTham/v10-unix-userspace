/*
 * /etc/mtab, and the numbers df(1) would have found in a superblock.
 *
 * Host libc appears here for getfsstat(2) and statfs(2) and nothing else, on
 * the same terms as utmp.c.  synth.c has the boundary.
 *
 * WHY df IS NOT DONE THE WAY who IS, which is the interesting part.  who(1)
 * needed no changes because the shim manufactures the FILE it reads.  The same
 * trick applied to df would mean manufacturing a fake DISK: df opens
 * /dev/<device> and reads block 1 as a struct filsys, so the shim would have to
 * produce a device file with a superblock at offset 1024, plus make
 * stat("/dev/x").st_rdev equal stat(mountpoint).st_dev so df's mount-point
 * lookup matches, plus put /dev/ in the jail's redirect list.  And `df -l'
 * walks the free-block LIST out of that superblock, so it would need a
 * fabricated free list too -- inventing data rather than reporting it, which is
 * the one thing this port will not do to make output look right.
 *
 * PLAN.md section 7 already decided this: "df via statfs backend, V8 output
 * format".  So df keeps its struct filsys, its arithmetic and its printf, and
 * only the line that filled the struct changes.  src/cmd/df/PORTING.md records
 * it as the deviation it is.
 *
 * ONE BSIZE, and it is chosen rather than inherited.  V8's BSIZE(dev) is
 * (dev & 64) ? 4096 : 1024, so a device number with bit 64 clear means 1024 --
 * which is also the unit df's own "kbytes" heading claims.  Every number below
 * is in those units.
 */

#include "kmemu.h"
#include "../v8sys/rawsys.h"	/* the overflow report; libc is for FACTS only */
#include <sys/param.h>
#include <sys/mount.h>

/*
 * <fstab.h>: the width of both mtab fields, and this port raises it from V7's
 * 32 to 1024, matching the host's own f_mntonname.  Spelled again here because this file is clang-compiled and
 * <fstab.h> belongs to the V8 include tree; src/include/PORTING.md carries the
 * reason, and the other three spellings are named there.
 */
#define FSNMLG	1024

/*
 * df's own struct, which is NOT <mtab.h>'s -- it declares
 *
 *	struct mtab { char path[FSNMLG]; char spec[FSNMLG]; }
 *
 * mount point first, device second, where the header has m_path then m_dname.
 * Same shape and same order; spelled here to match what df actually reads.
 */
struct v8mtab {
	char	path[FSNMLG];
	char	spec[FSNMLG];
};

#define MAXFS	32		/* df's own NFS: it reads no more than this */

/*
 * df hunts /etc/mtab for a device by gluing "/dev/" onto spec, so spec is
 * stored WITHOUT the prefix -- "disk3s1s1", not "/dev/disk3s1s1".  A mount
 * whose source is not a device at all (devfs, an automounter map, a share)
 * keeps its name as given; it will not resolve under /dev, which is correct,
 * because there is nothing there to open.
 */
/*
 * MTAB FIELDS ARE NUL-TERMINATED, and utmp's are not.  Same width, same
 * fixed-size array, opposite rule, and the difference is not decoration: df
 * does strcpy(&specbuf[5], mtab[i].spec) into a 38-byte buffer.  Fill the
 * 32-byte field to the brim with no terminator and that strcpy runs on through
 * the NEXT entry until it finds one, overflowing specbuf and smashing whatever
 * static follows it -- which here was the digit buffer ecvt hands to printf, so
 * df's %use column started emitting hundred-digit strings several rows AFTER
 * the row that caused it.
 *
 * Measured, by printing what df was about to convert: the integers were right
 * and only the conversion was wrong, and `file' had become
 * "/dev//Library/Developer/CoreSimulatordisk5s1" -- two fields run together,
 * which is what named the bug.
 *
 * So the rule is per-file rather than per-format: who(1) reads utmp with
 * %-8.8s and strncmp and must NOT see a terminator on a full field; df reads
 * mtab with strcpy and must.  Copy one byte less and terminate.
 */
static void
field0(char *dst, long dlen, const char *from)
{
	kmemu_field(dst, dlen, from, dlen - 1);
	dst[dlen - 1] = '\0';
}

static void
devname(char *dst, long dlen, const char *from)
{
	const char *p = from;

	if (p[0] == '/' && p[1] == 'd' && p[2] == 'e' && p[3] == 'v' && p[4] == '/')
		p += 5;
	field0(dst, dlen, p);
}

/*
 * Too long for the field, reported and refused.  Used by BOTH files, and the
 * "both" is the point: df's devlen() merges any fstab entry whose device is not
 * already in mtab, so dropping a mount from one file and not the other would
 * hand it straight back through the merge.  The two ends have to agree about
 * which filesystems exist -- they already disagreed about this very mount, by
 * one character, because field0 terminates and puts0 does not.
 */
static int
toolong(const char *mp, int say)
{
	long len = 0;

	while (mp[len]) len++;
	if (len < FSNMLG) return (0);
	if (say) {
		static const char msg[] =
		    "df: mount point too long for /etc/mtab, not listed: ";
		rawsys3(SYS_write, 2, (long)msg, (long)sizeof msg - 1);
		rawsys3(SYS_write, 2, (long)mp, len);
		rawsys3(SYS_write, 2, (long)"\n", 1L);
	}
	return (1);
}

int
kmemu_mtab(const char *hostpath)
{
	static struct v8mtab rec[MAXFS];
	struct statfs sb[MAXFS];
	int n, i;

	n = getfsstat(sb, (int)sizeof sb, MNT_NOWAIT);
	if (n < 0) return (-1);
	if (n > MAXFS) n = MAXFS;

	/*
	 * A MOUNT POINT THAT DOES NOT FIT IS DROPPED AND SAID SO, not truncated.
	 *
	 * This comment used to read "same loss as dir.c's 14-character names and
	 * utmp's 8: the field is the field", and that premise was wrong -- which
	 * is why the bug survived being documented.  A truncated NAME is a wrong
	 * name and still just a name.  A truncated PATH stops resolving, and
	 * df's dfree() branches on stat(file) succeeding: when it fails it takes
	 * the arm that assumes the string is a device name, and the row comes out
	 * with an empty dir column and the path's first nine characters sitting
	 * in the dev column.  Measured, on two CoreSimulator volumes.  A V8
	 * machine could never reach that branch with a mount point.
	 *
	 * So the field was widened to MAXPATHLEN (<fstab.h>, and see
	 * src/include/PORTING.md), which is the host's own width for this field
	 * and so covers anything it can report.  Widening MOVES the boundary rather
	 * than removing it, so what still will not fit is reported on stderr and
	 * left out, the way /proc reports a process table overflow rather than
	 * silently listing fewer processes.  An entry whose path cannot be stored
	 * cannot be described truthfully, and a garbled row is worse than an
	 * absent one that says it is absent.
	 */
	{
		int kept = 0;

		for (i = 0; i < n; i++) {
			if (toolong(sb[i].f_mntonname, 1)) continue;
			field0(rec[kept].path, (long)sizeof rec[kept].path,
			    sb[i].f_mntonname);
			devname(rec[kept].spec, (long)sizeof rec[kept].spec,
			    sb[i].f_mntfromname);
			kept++;
		}
		n = kept;
	}
	return (kmemu_replace(hostpath, (const char *)rec,
	    (long)n * (long)sizeof rec[0]));
}

/*
 * /etc/fstab, which has to be manufactured too, and the reason is not obvious.
 *
 * The rootfs carries UPSTREAM'S OWN /etc/fstab -- Bell Labs' real one, listing
 * /dev/ra00 through /dev/ra23.  Phase 6c installed it so /etc reads as genuine,
 * and as a museum piece it is exactly right.  For df it is a problem, because
 * df does not just read mtab: devlen() walks fstab, MERGES any entry not
 * already mounted into the mtab array, and takes both column widths from it.
 * So with the authentic file, df printed rows for /usr1, /fsave, /v8 and /sys
 * -- disks belonging to a VAX in New Jersey in 1985 -- statfs'd whatever those
 * paths happen to resolve to here, and cut every real mount point to ten
 * characters because "/usr/spool" was the longest name it had seen.
 *
 * An fstab describes the filesystems THIS machine has.  Keeping Bell Labs'
 * makes df report a machine that is not this one, which is the same kind of
 * plausible lie as a fabricated WCHAN.  The original stays in third_party, and
 * in the tree at v8/etc/fstab, for anyone who wants to read it.
 *
 * FSTABFMT is "%32s:%32s:%2s:%d:%d\n" -- spec, mount point, type, freq,
 * passno.  fs_spec keeps its /dev/ prefix here where mtab's spec drops it,
 * because devlen() skips five characters by hand.
 */
static void
puts0(char *buf, long *n, long cap, const char *s, long max, char end)
{
	long k = 0;

	while (s[k] && k < max && *n < cap - 1) buf[(*n)++] = s[k++];
	if (*n < cap - 1) buf[(*n)++] = end;
}

int
kmemu_fstab(const char *hostpath)
{
	/* Sized from FSNMLG rather than a round number: a line is two fields
	 * plus four separators and two digits, and at a widened FSNMLG the old
	 * MAXFS*80 would have held a fraction of the table and truncated the rest
	 * inside puts0's guard -- silently, because that guard stops writing
	 * rather than complaining. */
	static char buf[MAXFS * (2 * FSNMLG + 16)];
	struct statfs sb[MAXFS];
	long n = 0;
	int cnt, i;

	cnt = getfsstat(sb, (int)sizeof sb, MNT_NOWAIT);
	if (cnt < 0) return (-1);
	if (cnt > MAXFS) cnt = MAXFS;

	for (i = 0; i < cnt; i++) {
		const char *spec = sb[i].f_mntfromname;
		const char *type = (sb[i].f_flags & MNT_RDONLY) ? "ro" : "rw";

		/* Silent here: kmemu_mtab already said it, and df reads both. */
		if (toolong(sb[i].f_mntonname, 0)) continue;

		/*
		 * Only a real device can be opened under /dev, and only "rw" or
		 * "ro" is a type devlen() will look at.  A share, a devfs or an
		 * automounter map is neither, so it is written with type "xx"
		 * -- FSTAB_XX, which upstream's own header defines as "ignore
		 * totally".  Its own word for it, not one invented here.
		 */
		if (spec[0] != '/' || spec[1] != 'd' || spec[2] != 'e' ||
		    spec[3] != 'v' || spec[4] != '/')
			type = "xx";

		puts0(buf, &n, (long)sizeof buf, spec, FSNMLG, ':');
		puts0(buf, &n, (long)sizeof buf, sb[i].f_mntonname, FSNMLG, ':');
		puts0(buf, &n, (long)sizeof buf, type, 2, ':');
		puts0(buf, &n, (long)sizeof buf, "1", 1, ':');
		puts0(buf, &n, (long)sizeof buf, "1", 1, '\n');
	}
	return (kmemu_replace(hostpath, buf, n));
}

/*
 * The numbers themselves.
 *
 * f_bavail rather than f_bfree, because that is what a user can actually have
 * and what the host's own df prints as "Available".
 *
 * WHAT THIS CANNOT FIX, and it is a property of APFS rather than of the port:
 * every volume in a container reports the CONTAINER's f_blocks, so "total" is
 * the same 482G for the root volume, Preboot, VM and Data alike, and df's
 * used = blocks - free is then container-wide rather than the volume's own.
 * The host's df gets per-volume usage from getattrlist instead, which is not a
 * number any superblock ever held.  df's arithmetic is authentic and its inputs
 * are honest; the two together say something APFS does not mean.  Recorded in
 * src/cmd/df/PORTING.md rather than papered over by inventing a per-volume
 * total that statfs did not report.
 */
int
kmemu_fsstat(const char *path, struct kmemu_fs *out)
{
	struct statfs sb;
	long unit;

	if (statfs(path, &sb) < 0) return (-1);
	unit = (long)sb.f_bsize / 1024;
	if (unit < 1) unit = 1;			/* a 512-byte devfs, say */

	out->blocks = (long)sb.f_blocks * unit;
	out->bfree  = (long)sb.f_bavail * unit;
	out->files  = (long)sb.f_files;
	out->ffree  = (long)sb.f_ffree;
	return (0);
}
