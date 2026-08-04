	.data
	.comm	_errno,4
	.comm	_sys_nerr,4
	.comm	_sys_errlist,0
	.text
	.p2align	2
	.globl	_perror
_perror:
	sub	sp, sp, #64
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x29, x30, [sp, #-16]!
	mov	x29, sp
	sub	sp, sp, #512
	str	x27, [sp, #-16]!
	str	x28, [sp, #-16]!
	.section	__TEXT,__cstring
L16:

	.byte	0x55,0x6e,0x6b,0x6e,0x6f,0x77,0x6e,0x20
	.byte	0x65,0x72,0x72,0x6f,0x72,0x0
	.text
	adrp	x9, L16@PAGE
	add	x9, x9, L16@PAGEOFF
	mov	x28, x9
	adrp	x9, _errno@PAGE
	add	x9, x9, _errno@PAGEOFF
	ldr	w9, [x9]
	adrp	x10, _sys_nerr@PAGE
	add	x10, x10, _sys_nerr@PAGEOFF
	ldr	w10, [x10]
	cmp	x9, x10
	b.hs	L17
	adrp	x9, _errno@PAGE
	add	x9, x9, _errno@PAGEOFF
	ldrsw	x9, [x9]
	mov	x10, #3
	lsl	x9, x9, x10
	adrp	x10, _sys_errlist@PAGE
	add	x10, x10, _sys_errlist@PAGEOFF
	add	x9, x9, x10
	ldr	x9, [x9]
	mov	x28, x9
L17:
	ldr	x9, [x29, #16]
	str	x9, [sp, #320]
	ldr	x0, [sp, #320]
	bl	_strlen
	mov	x9, x0
	sxtw	x9, w9
	mov	x27, x9
	mov	x9, x27
	cmp	x9, #0
	b.eq	L19
	mov	x9, #2
	str	x9, [sp, #320]
	ldr	x9, [x29, #16]
	str	x9, [sp, #328]
	mov	x9, x27
	str	x9, [sp, #336]
	ldr	x0, [sp, #320]
	ldr	x1, [sp, #328]
	ldr	x2, [sp, #336]
	bl	_write
	.section	__TEXT,__cstring
L21:

	.byte	0x3a,0x20,0x0
	.text
	mov	x9, #2
	str	x9, [sp, #320]
	adrp	x9, L21@PAGE
	add	x9, x9, L21@PAGEOFF
	str	x9, [sp, #328]
	mov	x9, #2
	str	x9, [sp, #336]
	ldr	x0, [sp, #320]
	ldr	x1, [sp, #328]
	ldr	x2, [sp, #336]
	bl	_write
L19:
	mov	x9, #2
	str	x9, [sp, #320]
	mov	x9, x28
	str	x9, [sp, #328]
	mov	x9, x28
	str	x9, [sp, #448]
	ldr	x0, [sp, #448]
	bl	_strlen
	mov	x9, x0
	sxtw	x9, w9
	str	x9, [sp, #336]
	ldr	x0, [sp, #320]
	ldr	x1, [sp, #328]
	ldr	x2, [sp, #336]
	bl	_write
	.section	__TEXT,__cstring
L22:

	.byte	0xa,0x0
	.text
	mov	x9, #2
	str	x9, [sp, #320]
	adrp	x9, L22@PAGE
	add	x9, x9, L22@PAGEOFF
	str	x9, [sp, #328]
	mov	x9, #1
	str	x9, [sp, #336]
	ldr	x0, [sp, #320]
	ldr	x1, [sp, #328]
	ldr	x2, [sp, #336]
	bl	_write
L15:
	ldr	x28, [sp], #16
	ldr	x27, [sp], #16
	add	sp, sp, #512
	ldp	x29, x30, [sp], #16
	add	sp, sp, #64
	ret
	.data
