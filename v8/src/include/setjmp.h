/*
 * V8's setjmp.h says
 *
 *	typedef int jmp_buf[10];
 *
 * which is 40 bytes: enough for the VAX, whose setjmp.s saves the frame pointer,
 * the stack pointer, the PC and a handful of registers, all 32 bits wide.
 *
 * AAPCS64 needs more.  A longjmp must restore x19-x28 (ten callee-saved general
 * registers), x29, x30 and sp, and the low 64 bits of d8-d15, which are
 * callee-saved too -- 21 doublewords.  24 leaves room and keeps the buffer
 * 16-byte aligned, which storing sp requires.
 *
 * The type stays an array, so `jmp_buf env` still decays to a pointer when
 * passed to setjmp/longjmp.  That is the only property V8 code relies on --
 * nothing in the tree inspects the contents.
 */
typedef long jmp_buf[24];
