	.data
	.text
	.p2align	2
	.globl	_t
_t:
	sub	sp, sp, #64
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x29, x30, [sp, #-16]!
	mov	x29, sp
	ldr	x9, [x29, #16]
	ldrsw	x9, [x9]
	mov	x10, #1
	sub	x9, x9, x10
	ldr	x10, [x29, #16]
	str	w9, [x10]
	cmp	x9, #0
	b.lt	L19
	mov	x9, #1
	mov	x0, x9
	b	L20
L19:
	mov	x9, #2
	mov	x0, x9
L20:
	mov	x0, x9
	b	L18
L18:
	ldp	x29, x30, [sp], #16
	add	sp, sp, #64
	ret
	.data
