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
	sub	sp, sp, #16
	sub	sp, sp, #448
	.data
	.p2align	3
L14:
	.quad	0x4000000000000000	// 2
	.text
	adrp	x9, L14@PAGE
	add	x9, x9, L14@PAGEOFF
	ldr	d16, [x9]
	str	d16, [sp, #384]
	ldr	x0, [sp, #384]
	bl	_sqrt
	fmov	d16, d0
	str	d16, [x29, #-8]
	mov	x9, #0
	mov	x0, x9
	b	L13
L13:
	add	sp, sp, #448
	add	sp, sp, #16
	ldp	x29, x30, [sp], #16
	add	sp, sp, #64
	ret
	.data

.subsections_via_symbols
