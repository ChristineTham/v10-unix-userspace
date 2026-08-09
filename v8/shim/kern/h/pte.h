/*
 * pte.h -- the VAX page table entry, reduced to the one thing bio.c asks of it.
 *
 * THIS FILE IS OURS, not Bell Labs'; see shim/kern/h/param.h for why these
 * headers exist and how "../h/pte.h" reaches this one.  src/sys/dev/bio.c:11
 * includes it, which is the only reason it exists.
 *
 * WHY A STAND-IN AND NOT AN IMPORT.  Upstream's h/pte.h is 78 lines and is a
 * description of VAX address translation: a 21-bit page frame number packed
 * against a protection field and a valid bit, plus PG_V/PG_PROT/PG_FOD masks,
 * plus the kernel/user/system page-table base registers.  Every one of those is
 * a fact about hardware that does not exist here.  Importing it would put a
 * bitfield layout in the tree that nothing can ever be checked against, which
 * is the `unconsumed component' failure CLAUDE.md records for /dev/fd: a
 * component with no caller invents a difference the kernel does not have.
 *
 * SO WHAT IS ACTUALLY NEEDED IS ONE DECLARATION, AND ITS RETURN TYPE IS THE
 * WHOLE POINT.  bio.c:555 is
 *
 *	vpte = vtopte(p, btop(addr));
 *
 * and upstream declares the function at h/pte.h:67 as
 *
 *	struct	pte *vtopte();
 *
 * -- inside `#ifndef LOCORE' / `#ifdef KERNEL'.  Without that declaration
 * vtopte is an implicit function returning int, KERNFLAGS has
 * -Wno-implicit-function-declaration so NOTHING IS SAID, and the pointer comes
 * back through a 32-bit int.  That is CLAUDE.md's dominant bug class arriving
 * in its quietest form: the declaration lies about a type, the compiler is
 * silent by our own flag, and the corruption lands far from the cause.  It is
 * the same shape as `extern float atof()' and as the yylval.p token bug.
 *
 * THE FIRST DRAFT LEFT THE STRUCT INCOMPLETE AND ARGUED THAT THIS WAS RIGHT,
 * AND THE COMPILER SAID OTHERWISE.  The argument was that a `struct pte *' can
 * be returned, assigned and compared, so an incomplete type would allow
 * everything legitimate and turn any field access into a compile error naming
 * this file -- "which is the correct outcome, because there is no page table
 * under it to read."
 *
 * bio.c:557 reads two fields:
 *
 *	if (vpte->pg_pfnum == 0 || vpte->pg_fod)
 *		panic("swap bad pte");
 *	*dpte++ = *vpte++;
 *
 * -- and :559 does pointer arithmetic on the type, which also needs a size.
 * So the layout IS consumed, by authentic source, and the honest thing is
 * upstream's bitfields verbatim.  Recorded rather than quietly fixed because
 * the reasoning was the good kind and still wrong: it is this port's own rule
 * that a stand-in should offer the least it can, and the way to find out how
 * little that is turns out to be the build rather than the survey.
 *
 * WHAT IS STILL TRUE is that nothing here can produce a valid one.  vtopte()
 * panics (shim/kern/sys/v8fs.c), so `vpte' at :556 is never assigned and the
 * fields below are never read at run time.  The bitfields are a LAYOUT with no
 * instance, which is a weaker thing than a working page table and is exactly
 * what bio.c needs to compile.  A 21-bit pg_pfnum is also the same width here
 * as on a VAX -- `unsigned int' is 32 bits under LP64 -- so the declaration is
 * not merely plausible, it is the same object.
 */

#ifndef V8KERN_PTE_H
#define V8KERN_PTE_H

/*
 * h/pte.h:12-22, verbatim including the unnamed :2 gap.  Only pg_pfnum and
 * pg_fod are read by anything imported; the rest are present because a
 * bitfield's position depends on every field before it, so a subset would be
 * a DIFFERENT layout wearing the same names -- the on-disk-struct lesson of
 * §8a step 4a arriving in a register-level struct.
 */
struct pte
{
unsigned int	pg_pfnum:21,		/* core page frame number or 0 */
		:2,
		pg_vreadm:1,		/* modified since vread (or with _m) */
		pg_swapm:1,		/* have to write back to swap */
		pg_fod:1,		/* is fill on demand (=0) */
		pg_m:1,			/* hardware maintained modified bit */
		pg_prot:4,		/* access control */
		pg_v:1;			/* valid bit */
};

struct	pte *vtopte();		/* h/pte.h:67, spelled as upstream spells it */

#endif /* V8KERN_PTE_H */
