/*
 * mtpr -- "move to processor register", the VAX instruction for writing a
 * privileged register.  Ours, not Bell Labs'; see shim/kern/h/param.h for why
 * these headers exist and how the include path reaches them.
 *
 * V8's stream.h ends with
 *
 *	#define setqsched()	mtpr(SIRR, 0x1);
 *
 * and that macro is AUTHENTIC and compiles unchanged, which is the point of
 * providing mtpr rather than editing stream.h.  SIRR is the Software Interrupt
 * Request Register: writing n to it requests an interrupt at IPL n.  So
 * `mtpr(SIRR, 1)' means one thing -- "run the queue scheduler as soon as the
 * priority level allows" -- and that meaning survives the move off the VAX
 * intact.  What changes is the mechanism underneath, not the request.
 *
 * Only SIRR is honoured.  Every other privileged register on a VAX describes
 * hardware that is not here (the memory management base registers, the console
 * receive/transmit silos, the interval clock), and a stream module that wrote
 * one would be doing something this port cannot answer for.  The unknown case
 * panics rather than returning quietly, because a silent no-op is how a
 * machine-dependent gap turns into a program that runs and is wrong.
 */

#ifndef V8KERN_MTPR_H
#define V8KERN_MTPR_H

#define SIRR	0x14		/* software interrupt request, VAX pr$_sirr */

void	v8k_mtpr(int reg, long val);
#define mtpr(reg, val)	v8k_mtpr((reg), (long)(val))

#endif /* V8KERN_MTPR_H */
