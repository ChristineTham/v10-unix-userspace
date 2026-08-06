/*
 * conf.h -- the kernel's device and stream configuration, reduced to the part
 * that has meaning here.  Ours, not Bell Labs'; see shim/kern/h/param.h.
 *
 * V8's is 76 lines and is mostly `struct cdevsw', `struct bdevsw' and
 * `struct fstypsw' -- the driver switch tables, one row per device on the
 * machine.  stream.c includes this header and, measured, references nothing
 * from it: it is included because every file in dev/ includes it.  So the
 * temptation is an empty file.
 *
 * `struct streamtab' is here instead, because it is the one thing in conf.h
 * that is about streams rather than about VAX peripherals, and it is the type a
 * stream MODULE must define to exist -- the pair of qinits, read side and write
 * side, that stopen() pushes.  streamio.c will want it.  Spelled exactly as
 * upstream spells it, so that importing V8's conf.h later is a substitution
 * rather than a merge.
 *
 * The switch tables are deliberately absent.  There is no cdevsw here because
 * there are no character devices here: the shim answers open(2) itself, and a
 * table of major numbers pointing at drivers for a DZ11 and an RP07 would be a
 * description of furniture this room does not have.
 */

#ifndef V8KERN_CONF_H
#define V8KERN_CONF_H

/*
 * stream processor table
 */
extern	struct streamtab {
	struct	qinit	*rdinit;
	struct	qinit	*wrinit;
} *streamtab[];

#endif /* V8KERN_CONF_H */
