	.data
	.text
	.p2align	2
	.globl	_main
_main:
	sub	sp, sp, #64
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x29, x30, [sp, #-16]!
	mov	x29, sp
	sub	sp, sp, #80
	mov	x9, #118
	strb	w9, [x29, #-64]
	mov	x9, #56
	strb	w9, [x29, #-63]
	mov	x9, #32
	strb	w9, [x29, #-62]
	mov	x9, #108
	strb	w9, [x29, #-61]
	mov	x9, #105
	strb	w9, [x29, #-60]
	mov	x9, #118
	strb	w9, [x29, #-59]
	mov	x9, #101
	strb	w9, [x29, #-58]
	mov	x9, #10
	strb	w9, [x29, #-57]
	sub	sp, sp, #32
	mov	x9, #1
	str	x9, [sp, #0]
	sub	x9, x29, #64
	str	x9, [sp, #8]
	mov	x9, #8
	str	x9, [sp, #16]
	ldr	x0, [sp, #0]
	ldr	x1, [sp, #8]
	ldr	x2, [sp, #16]
	bl	_write
	add	sp, sp, #32
	ldrsw	x9, [x29, #16]
	add	x9, x9, #48
	str	w9, [x29, #-72]
	ldrsb	x9, [x29, #-72]
	strb	w9, [x29, #-64]
	mov	x9, #10
	strb	w9, [x29, #-63]
	sub	sp, sp, #32
	mov	x9, #1
	str	x9, [sp, #0]
	sub	x9, x29, #64
	str	x9, [sp, #8]
	mov	x9, #2
	str	x9, [sp, #16]
	ldr	x0, [sp, #0]
	ldr	x1, [sp, #8]
	ldr	x2, [sp, #16]
	bl	_write
	add	sp, sp, #32
	mov	x9, #0
	mov	x0, x9
	b	L12
L12:
	add	sp, sp, #80
	ldp	x29, x30, [sp], #16
	add	sp, sp, #64
	ret
	.data
