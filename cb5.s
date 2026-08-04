	.data
	.text
	.p2align	2
	.globl	_a
_a:
	sub	sp, sp, #64
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x29, x30, [sp, #-16]!
	mov	x29, sp
	ldr	x9, [x29, #16]
	ldr	x9, [x9]
	mov	x10, #-2
	and	x9, x9, x10
	mov	x0, x9
	b	L12
L12:
	ldp	x29, x30, [sp], #16
	add	sp, sp, #64
	ret
	.data
	.text
	.p2align	2
	.globl	_b
_b:
	sub	sp, sp, #64
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x29, x30, [sp, #-16]!
	mov	x29, sp
	ldr	x9, [x29, #16]
	ldr	x9, [x9]
	mov	x10, #-2
	and	x9, x9, x10
	mov	x0, x9
	b	L14
L14:
	ldp	x29, x30, [sp], #16
	add	sp, sp, #64
	ret
	.data
	.text
	.p2align	2
	.globl	_c
_c:
	sub	sp, sp, #64
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x29, x30, [sp, #-16]!
	mov	x29, sp
	ldr	x9, [x29, #16]
	ldr	x9, [x9]
	mov	x10, #-2
	and	x9, x9, x10
	mov	x0, x9
	b	L16
L16:
	ldp	x29, x30, [sp], #16
	add	sp, sp, #64
	ret
	.data
