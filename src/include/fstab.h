/*
 * File system table, see fstab (5)
 *
 * Used by dump, mount, umount, swapon, fsck, df, ...
 *
 * The fs_spec field is the block special name.
 * Programs that want to use the character special name must
 * create that name by prepending a 'r' after the right most slash.
 */

#define	FSTAB		"/etc/fstab"
/*
 * PORT: 1024, not V7's 32, and it is NOT the same kind of change as widening a
 * name would be.  fs_file and mtab's path hold a PATH, and a truncated path
 * stops resolving -- df's dfree() branches on stat(file) succeeding, and takes
 * its "this string is a device name" arm when it does not.  So a mount point
 * that overflowed did not print short, it printed with an empty dir column and
 * the first nine characters of the path in the dev column.  A V8 machine could
 * not reach that branch, because mount points were short.
 *
 * WHY 1024 RATHER THAN A ROUND NUMBER THAT LOOKS BIG ENOUGH.  The host's own
 * field is `char f_mntonname[MAXPATHLEN]' (<sys/mount.h>), and MAXPATHLEN is
 * 1024 -- so that is exactly the set of mount points the host can report, and
 * any smaller choice is a boundary that has to be defended against the next
 * machine.  128 was tried first, on the reasoning that the longest mount point
 * on the development Mac is 52 and 128 makes the record a tidy 256 bytes.  CI
 * refuted it within the hour: a GitHub runner mounts
 *
 *   /System/Library/AssetsV2/com_apple_MobileAsset_UAF_Siri_Understanding/...
 *
 * at 154 characters.  Matching the host's field is the only number that does
 * not invite that.  src/include/PORTING.md has the account.
 *
 * FOUR PLACES SPELL THIS NUMBER and they must agree: here, FSTABFMT below,
 * <mtab.h>'s two literals, and shim/libkmemu/mtab.c's own copy.  V8's cpp is
 * 1985 and has no # stringification, so FSTABFMT cannot be built from FSNMLG
 * and carries the digits by hand.
 */
#define	FSNMLG		1024

#define	FSTABFMT	"%1024s:%1024s:%2s:%d:%d\n"
#define	FSTABARG(p)	(p)->fs_spec, (p)->fs_file, \
			(p)->fs_type, &(p)->fs_freq, &(p)->fs_passno
#define FSTABNARGS	5

#define	FSTAB_RW	"rw"	/* read write device */
#define	FSTAB_RO	"ro"	/* read only device */
#define	FSTAB_SW	"sw"	/* swap device */
#define	FSTAB_XX	"xx"	/* ignore totally */

struct	fstab{
	char	fs_spec[FSNMLG];	/* block special device name */
	char	fs_file[FSNMLG];	/* file system path prefix */
	char	fs_type[3];		/* rw,ro,sw or xx */
	int	fs_freq;		/* dump frequency, in days */
	int	fs_passno;		/* pass number on parallel dump */
};

struct	fstab *getfsent();
struct	fstab *getfsspec();
struct	fstab *getfsfile();
int	setfsent();
int	endfsent();
