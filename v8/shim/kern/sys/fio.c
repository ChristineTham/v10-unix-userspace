/*
 * The u-area, the proc entry, the file table and the inode edge.
 *
 * Named after sys/fio.c, which is where V8 keeps ufalloc and closef; iput is
 * sys/iget.c's.  Ours, not Bell Labs' -- see shim/kern/h/param.h.
 *
 * THESE ARE THE THREE NAMES src/sys/PORTING.md CALLED "THE REAL DESIGN".  Of
 * the fifteen external names streamio.c still needed after the engine and the
 * headers were counted, twelve are mechanical: copyin and copyout are bcopy
 * because there is one address space, min and nulldev are one line each
 * upstream, tsleep and wakeup were the blocker and are settled in slp.c, and
 * the three signal names are delivery, which works here.  closef, ufalloc and
 * iput are the ones with a decision in them, because they are the file table
 * and the inode edge -- the reason strput LOOKS pure and is not.
 *
 * WHAT streamio.c ACTUALLY WANTS FROM A FILE TABLE, measured rather than
 * inferred from the names, because "port the file table" would be a much
 * larger answer than the question:
 *
 *   forceclose (:1042)   walks file[0]..fileNFILE, finds every open file whose
 *                        inode points at this stream, and clears FREAD/FWRITE
 *                        while setting FHUNGUP.  This is what makes a read on
 *                        a hung-up stream fail instead of blocking, and it is
 *                        why strput is not pure: an M_HANGUP arriving in the
 *                        stream head reaches into the process's open files.
 *
 *   usndfile/sndfile     FIOSNDFD: GETF the caller's fd, bump f_count, put the
 *   (:911, :926)         `struct file *' itself into an M_PASS block.
 *
 *   urcvfile (:961)      FIORCVFD: take the M_PASS block, ufalloc() a
 *                        descriptor, u.u_ofile[i] = fp.
 *
 *   stclose (:184)       drain any M_PASS blocks left on the queue and closef
 *                        the files nobody collected.
 *
 * So it is a table of `struct file', an allocator over u.u_ofile[], and a
 * reference count.  It is NOT the open/creat/dup machinery: falloc, getf,
 * closei, openi and the rest are never named, and writing them would be
 * writing rules nothing exercises.  CLAUDE.md's sentence about v8s_mknod
 * applies exactly -- AN UNEXERCISED RULE CANNOT BE SEEN TO BE INCOMPLETE --
 * so this file implements what is called and no more.
 *
 * THE TABLE IS REAL STORAGE AND THAT IS DELIBERATE.  A cheaper answer was
 * available: make forceclose a no-op, since a per-binary shim's V8 program
 * holds its descriptors in libv8sys, not here.  It would have compiled, linked
 * and passed every stream test.  It would also have meant that the single
 * thing streamio.c does to the process's open files -- the whole content of a
 * hangup -- silently did nothing.  A table of NFILE entries is 24 KB of bss in
 * an archive that is already 85 KB, and it makes the hangup path observable,
 * which is the only reason to have imported the path at all.
 */

#include "../../v8sys/rawsys.h"
#include "../../v8id.h"		/* v8_foldid -- the narrowing rule, shared */
#include "../h/param.h"

/*
 * NO #undef printf HERE, unlike dev/machdep.c.  That file DEFINES v8k_printf
 * and so has to see its own name; this one CALLS it, and a diagnostic from the
 * file table belongs on fd 2 with the rest of the kernel's rather than in the
 * V8 program's stdio buffer.  The redirect stays in force.
 */
#include "../../../src/sys/h/stream.h"
#include "../h/proc.h"
#include "../../../src/sys/h/dir.h"	/* struct direct, for user.h's u_dent */
#include "../h/user.h"
#include "../../../src/sys/h/inode.h"
#include "../../../src/sys/h/file.h"

/*
 * THE INCLUDES ABOVE REACH ACROSS THE SEAM BY RELATIVE PATH, and streamio.c's
 * do not.  That asymmetry is not an oversight.
 *
 * streamio.c is authentic and says `#include "../h/inode.h"'; the build gives
 * it -Ishim/kern/dev so the ones we supply are found and the ones upstream
 * supplies win by being in src/sys/h/.  Nothing in that mechanism marks which
 * is which, which is the property worth having THERE.
 *
 * Here it would be the wrong property.  This file is ours, it is modern C, and
 * a reader needs to know at a glance that stream.h, inode.h and file.h are
 * Bell Labs' while proc.h and user.h are stand-ins.  The path says so.
 */

/*
 * NFILE -- upstream's is set by config(8) from the machine's configuration
 * file, and V8's research machine used 500.  Sixty-four here, because the
 * table's only reader is forceclose and its only writers are the two
 * file-passing ioctls, and this is one process.  The number is the shim's to
 * choose in the same way NQUEUE is Bell Labs' to choose in sparam.h.
 */
#define	NFILE	64

struct user	u;
struct proc	v8k_proc0;

static struct file	filetab[NFILE];

/*
 * file.h's #ifdef KERNEL block declares these as TENTATIVE definitions, so
 * streamio.o has a common symbol for each (KERNFLAGS passes -fcommon, which is
 * the 1985 dialect and the reason the V8 linker was happy to see the same
 * three names in forty objects).  These are strong definitions and win.
 */
struct file	*file = filetab;
struct file	*fileNFILE = &filetab[NFILE];
int		nfile = NFILE;

/*
 * NARROWING A HOST ID INTO A VAX-WIDTH FIELD -- three of them, and each has a
 * different value that must not come out by accident.
 *
 * p_pid and p_pgrp are `short' here and that is UPSTREAM's width, not a
 * narrowing this port chose: h/proc.h:28-29 declare both short, so on a VAX
 * they were exactly as wide as a process id.  shim/kern/h/proc.h has the whole
 * argument.  u_uid and u_gid are `short' for the same reason, at user.h:33-34.
 * What changed is not the field but the population of values put in it: a
 * Darwin pid runs to 99998 and a Darwin uid past 100000 on a
 * directory-bound Mac.
 *
 * FOLDED, NOT TRUNCATED, and the difference is always some ONE value that a
 * cast can produce by accident and that the code above reads as meaning
 * something:
 *
 *	pid	`(short)65536' is 0, and pid 0 is the scheduler
 *	pgrp	0 means "no process group" to streamio.c:45
 *	uid	0 means ROOT -- and streamio.c:44 is
 *		`sp->flag & EXCL && u.u_uid!=0', so root bypasses a stream's
 *		exclusive-use lock, while sndfile:951 copies u_uid into the
 *		credentials FIORCVFD hands the receiving program
 *
 * So a host uid congruent to 0 mod 65536 would let an ordinary user through an
 * EXCL stream and tell the far end the descriptor came from root.  pid folds
 * to 1..30000, which is the range V8's own sys/sys1.c wraps mpid to; foldid()
 * keeps every id that fits exactly and guarantees the two properties that
 * matter -- ROOT MAPS TO ROOT, NON-ROOT NEVER MAPS TO ROOT.  Above 32767 two
 * ids can collide, and there is no lossless answer inside a 16-bit signed
 * field; a collision between two non-root users is a far smaller lie than a
 * promotion to root.
 *
 * ALL OF THIS IS LATENT, exactly as the p_pid bug was latent on a freshly
 * booted host -- 501 here, and a CI runner is lower still.
 *
 * TWO OF THE THREE WERE WRITTEN WRONG IN THE FIRST DRAFT OF THIS FILE, BOTH
 * WITHIN FIVE LINES OF THE PARAGRAPH ARGUING AGAINST THEM, and neither was
 * found by the person who wrote both.  u_uid and u_gid were a bare `(short)'
 * cast on the two lines after the pid fold -- CLAUDE.md's own warning that THE
 * FIX LANDS ON ONE LINE AND THE LINE BESIDE IT KEEPS THE ASSUMPTION -- and the
 * host-pid cache below was itself a `short', with a comment claiming kill(2)
 * used it while v8k_hostof() called getpid() again and nothing read it at all.
 * Dead code with a comment saying it is live is worse than no code: the next
 * reader takes the comment at its word and signals a folded pid.
 *
 * AND THE FIX REACHED ONE OF THREE COMPONENTS, WHICH IS THE SAME SHAPE ONE
 * LEVEL UP.  The paragraph above is about two lines in this file; a later
 * sweep for `(short)' against a uid found the identical cast still standing in
 * syscall.c's stat_translate -- the stat(2) path every `ls -l' goes through --
 * and in procfs.c's u-area, which is ps(1)'s uid column.  So foldid() has
 * moved into shim/v8id.h as v8_foldid(), a header rather than a symbol because
 * no two of the three components may share an archive.  The rule and its
 * argument live there now; what stays here is the account of the FIELDS, which
 * is this file's business.
 */
static long	v8k_hostpid;	/* the real one, for kill(2) */

void
v8k_procinit(void)
{
	long pid = rawsys0(SYS_getpid);

	v8k_hostpid = pid;		/* whole, for kill(2) */
	v8k_proc0.p_pid = (short)(pid % 30000 + 1);
	v8k_proc0.p_pgrp = v8k_proc0.p_pid;
	v8k_proc0.p_nice = NZERO;
	v8k_proc0.p_wchan = NULL;
	u.u_procp = &v8k_proc0;
	u.u_uid = v8_foldid(rawsys0(SYS_getuid));
	u.u_gid = v8_foldid(rawsys0(SYS_getgid));
	u.u_segflg = 0;
}

/*
 * v8k_hostof -- the V8 pid or pgrp above, mapped back to the host's.
 *
 * There is one process, so the map has one entry and this is a comparison
 * rather than a table.  It exists so that gsignal/psignal in subr.c deliver to
 * something real instead of to a folded number, and so that the place where
 * the fold is undone is a named function rather than an inline cast somewhere
 * -- the day a second V8 process exists, this is the function that grows a
 * table and nothing else changes.
 *
 * AN UNINITIALISED MAP ANSWERED `0', AND kill(0, sig) IS A BROADCAST.
 * §8a step 5d, found by mutation, and it had killed the test runner and the
 * shell above it before it was understood.
 *
 * v8k_procinit() sets v8k_hostpid from getpid(2), so it is nonzero once that
 * has run -- and it had run in every consumer that existed, because all three
 * of them were stream probes.  A consumer that stands up a FILESYSTEM does not
 * need a process description and does not call it, so p_pid stayed 0 out of
 * bss, `v8pid == p_pid' matched with v8pid 0, and this returned
 * v8k_hostpid == 0.  subr.c's psignal guard is `if (hp < 0) return', which 0
 * passes, and the syscall that came out was kill(0, SIGXFSZ) -- every process
 * in the group, i.e. run.sh and the interactive shell.
 *
 * THE SHAPE IS THIS PORT'S MOST REPEATED ONE AND THE OTHER HALF WAS ALREADY
 * FIXED.  subr.c's gsignal carries `if (pgrp == 0) return' with a comment
 * explaining that group 0 is not a group; foldid() above never returns 0 for a
 * non-root pid for the same family of reason.  The guard landed on gsignal and
 * the line beside it kept the assumption -- exactly what CLAUDE.md records for
 * fio.c's own (short)u_uid cast sitting one line under the paragraph arguing
 * against a bare cast.
 *
 * So the answer is TWO guards, because they are two different claims: this one
 * says "no host process is known", and subr.c's says "0 is not a pid".
 */
long
v8k_hostof(int v8pid)
{
	if (v8k_hostpid <= 0)
		return (-1);		/* v8k_procinit has not run */
	if (v8pid == v8k_proc0.p_pid || v8pid == v8k_proc0.p_pgrp)
		return (v8k_hostpid);
	return (-1);
}

/*
 * ufalloc -- upstream sys/fio.c.  Lowest free descriptor, or -1 with EMFILE.
 *
 * Upstream also clears u_pofile[i] (the close-on-exec and advisory-lock bits).
 * There is no u_pofile here: shim/kern/h/user.h carries the thirteen fields
 * this code touches and no others, and nothing in streamio.c reads or writes
 * that array.
 */
int
ufalloc(void)
{
	int i;

	for (i = 0; i < NOFILE; i++)
		if (u.u_ofile[i] == NULL) {
			u.u_r.r_val1 = i;
			return (i);
		}
	u.u_error = EMFILE;
	return (-1);
}

/*
 * iput -- upstream sys/iget.c, reduced to the reference count.
 *
 * Upstream's iput unlocks the inode, and when the last reference goes it
 * checks i_nlink, truncates the file, frees the inode on disk, writes it back
 * through iupdat and calls the filesystem's t_free.  Every one of those is a
 * disk operation, and there is no disk under libv8kern: an inode here is a
 * handle the shim made, not a record read from a device.
 *
 * So this is the count and nothing else, and the omission is stated rather
 * than hidden.  When PLAN.md section 8a step 5 puts a v8fs server behind the
 * switch, iput acquires the rest of its body -- and it acquires it HERE,
 * because streamio.c will still be calling this name.
 *
 * The two callers are stopen:77 and :133, both on the device-cloning path
 * where a driver's qopen returns a different inode and the original is handed
 * back, and closef below.
 *
 * ---------------------------------------------------------------------------
 * §8a step 5 RETIRED IT, AND THE PARAGRAPH ABOVE PREDICTED THE EVENT AND GOT
 * THE MECHANISM EXACTLY BACKWARDS.
 *
 * It says: "When PLAN.md section 8a step 5 puts a v8fs server behind the
 * switch, iput acquires the rest of its body -- and it acquires it HERE,
 * because streamio.c will still be calling this name."
 *
 * Step 5 arrived and iput did acquire the rest of its body -- the unlock, the
 * i_nlink check, itrunc, ifree, iupdat and the t_free dispatch -- but not
 * here.  src/sys/sys/iget.c:176 is imported byte-identical and IS the full
 * function, so this one was deleted rather than grown.  The premise was right
 * (streamio.c still calls the name) and the conclusion did not follow from it:
 * a name being called here says nothing about where it should be DEFINED, and
 * the authentic file was always going to win that.
 *
 * Worth keeping as written, because the shape recurs: a comment that correctly
 * anticipates a future change and misdescribes it reads, at the moment of the
 * change, exactly like a comment that was right.
 *
 * One consequence is live rather than editorial.  Upstream's iput really does
 * touch the disk -- itrunc and ifree -- so the omission this note recorded is
 * now closed, and closef's `iput(fp->f_inode)' below can reach real
 * filesystem work for the first time.  The declaration in param.h changed
 * return type from void to int to match iget.c:176, which has no type on the
 * line above the name.
 * ---------------------------------------------------------------------------
 */

/*
 * closef -- upstream sys/fio.c, reduced the same way and for the same reason.
 *
 * Upstream decrements f_count, and at zero calls closei() to run the device's
 * close routine and iput the inode.  Here the device close belongs to the
 * stream, which stclose has already done by the time the M_PASS blocks are
 * drained, so what is left is the count and the inode reference.
 *
 * f_count <= 0 is a kernel bug rather than a caller error, so it says so
 * instead of clamping quietly: a file table entry that is closed twice is how
 * a descriptor gets reused underneath its owner.
 */
void
closef(struct file *fp)
{
	if (fp == NULL)
		return;
	if (fp->f_count <= 0) {
		printf("closef: count is %d\n", fp->f_count);
		return;
	}
	if (--fp->f_count > 0)
		return;
	if (fp->f_inode) {
		iput(fp->f_inode);
		fp->f_inode = NULL;
	}
	fp->f_flag = 0;
}

/*
 * v8k_falloc / v8k_ffree -- NOT upstream's falloc, and named apart so that is
 * visible.
 *
 * streamio.c never allocates a file; it only ever passes one that a system
 * call already made.  So there is no authentic name to reproduce, and the two
 * things a caller of this library needs -- put a file in the table, take it
 * out again -- get shim names.  v8s_* will use them when the shim's own
 * descriptors start being visible to a stream; tests/streams uses them now,
 * which is the only reason FIOSNDFD and FIORCVFD can be exercised at all.
 */
struct file *
v8k_falloc(struct inode *ip, int flag)
{
	struct file *fp;

	for (fp = file; fp < fileNFILE; fp++)
		if (fp->f_count == 0) {
			fp->f_count = 1;
			fp->f_flag = (short)flag;
			fp->f_inode = ip;
			fp->f_offset = 0;
			if (ip)
				ip->i_count++;
			return (fp);
		}
	u.u_error = ENFILE;
	return (NULL);
}

void
v8k_ffree(struct file *fp)
{
	if (fp == NULL || fp->f_count == 0)
		return;
	closef(fp);
}
