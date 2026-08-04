/*
 * setjmp/longjmp for ARM64 -- replaces libc/sys/setjmp.s.
 *
 * 82 longjmp and 68 setjmp calls across the V8 command tree, so this is not
 * optional furniture: sh, ed, awk and troff all unwind through it.
 *
 * The VAX original does something that cannot be ported. It saves only the
 * caller's frame pointer and PC, and longjmp then WALKS the frame chain --
 * popping one frame at a time by rewriting the saved PC to point back at its
 * own loop, and depositing the return value into each frame's saved-r0 slot on
 * the way past, guided by the VAX call frame's register-save mask. That is a
 * property of the VAX `calls` instruction, not of C.
 *
 * ARM64 has no such structure and needs none. The portable formulation of the
 * same contract is to save the callee-saved state at the point of setjmp and
 * restore it at longjmp:
 *
 *	x19-x28	 ten callee-saved general registers
 *	x29, x30 frame pointer and return address
 *	sp	 stack pointer (via a scratch register; str cannot name sp)
 *	d8-d15	 low 64 bits, which AAPCS64 also makes callee-saved
 *
 * Layout matches jmp_buf in src/include/setjmp.h -- long[24], of which 21 are
 * used. It must be 16-byte aligned; the array's `long` element type gives 8,
 * and every caller in the tree declares it as a plain automatic or static, both
 * of which this back end aligns to 16 for a type this size.
 *
 * Two behaviours are kept from the original because programs depend on them:
 *
 *   - longjmp(env, 0) returns 1 from setjmp, not 0. The VAX code does this with
 *     `bneq L1; movzbl $1,r0`.
 *   - an unset buffer is "longjmp botch": a message on fd 2 and death. The VAX
 *     halts; here it exits, since a user process cannot halt the machine.
 */

	.text
	.p2align 2

	.globl _setjmp
_setjmp:
	stp	x19, x20, [x0, #0]
	stp	x21, x22, [x0, #16]
	stp	x23, x24, [x0, #32]
	stp	x25, x26, [x0, #48]
	stp	x27, x28, [x0, #64]
	stp	x29, x30, [x0, #80]
	mov	x1, sp			/* sp cannot be an operand of str */
	str	x1, [x0, #96]
	stp	d8,  d9,  [x0, #104]
	stp	d10, d11, [x0, #120]
	stp	d12, d13, [x0, #136]
	stp	d14, d15, [x0, #152]
	mov	x0, #0
	ret

	.globl _longjmp
_longjmp:
	ldr	x2, [x0, #96]		/* saved sp: zero means never set */
	cbz	x2, Lbotch

	mov	x3, x1			/* the value setjmp will return */
	cbnz	x3, 1f
	mov	x3, #1			/* longjmp(env,0) returns 1 */
1:
	ldp	x19, x20, [x0, #0]
	ldp	x21, x22, [x0, #16]
	ldp	x23, x24, [x0, #32]
	ldp	x25, x26, [x0, #48]
	ldp	x27, x28, [x0, #64]
	ldp	x29, x30, [x0, #80]
	ldp	d8,  d9,  [x0, #104]
	ldp	d10, d11, [x0, #120]
	ldp	d12, d13, [x0, #136]
	ldp	d14, d15, [x0, #152]
	mov	sp, x2
	mov	x0, x3
	ret				/* returns to setjmp's caller */

Lbotch:
	/*
	 * write(2, msg, 14) then exit. Reached through the V8 names, so this
	 * goes through the shim like any other call rather than issuing a raw
	 * syscall -- one place decides how a syscall is made.
	 */
	mov	x0, #2
	adrp	x1, Lmsg@PAGE
	add	x1, x1, Lmsg@PAGEOFF
	mov	x2, #14
	bl	_write
	mov	x0, #1
	b	__exit

	.section __TEXT,__cstring
Lmsg:
	.ascii	"longjmp botch\n"
