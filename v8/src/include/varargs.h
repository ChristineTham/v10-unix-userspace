/*
 * varargs -- V7's variable-argument mechanism, walking THIS machine's
 * argument slots rather than the VAX's.
 *
 * PORT: two changes, both forced by the target, and both the same fact --
 * v8cc spills x0-x7 into EIGHT-byte argument slots (SZARG is SZLONG), where
 * a VAX packed them four bytes apart.
 *
 *	va_dcl		int va_alist	-> long va_alist
 *	va_arg		strides sizeof(mode) -> strides one SLOT
 *
 * WHY THIS HEADER WAS STILL 1985's.  It is not that the class was unknown --
 * it is this port's dominant one, and the FORWARD form of the same idiom
 * (V8's printf(fmt, args) walking &args) was fixed years ago in seven files:
 * exec.c, doprnt.c, scanf.c, sprintf.c, printf.c, fprintf.c and troff/n1.c,
 * every one of them walking with an eight-byte type.  What was missed is that
 * <varargs.h> says the same thing a third way, and NOTHING IN THE PORT HAD
 * EVER INCLUDED IT.  Measured: src/cmd/ex/printf.c is the only file in src,
 * shim or compiler using va_alist/va_dcl at all.  So the header sat at
 * upstream's definition, byte-identical, invisible -- the sys/fblk.h shape,
 * where a header nobody imported silently stays 1985's.
 *
 * HOW IT FAILED, for the next person who meets it: ex(1) printed the opening
 * `"file.txt"' of its status line and then SIGSEGV'd before the line count.
 * A four-byte stride over eight-byte slots makes every argument after the
 * first a mixture of two halves, so the char * that printf reaches for is a
 * spliced address.  The first argument is always right, which is why the file
 * name appeared and nothing after it did.
 *
 * WHY va_arg IS NOT `((mode *)(list += SLOT))[-1]'.  That indexes back by
 * sizeof(mode), not by SLOT, so for an int it lands four bytes PAST the slot
 * base -- the upper half on a little-endian machine, which is zero for small
 * positive values and therefore right often enough to be misleading.  The
 * comma form below reads from the slot base, which is where the value is.
 *
 * A double is 8 bytes and v8cc passes it positionally like everything else,
 * so one slot covers every type these macros are used with.
 */
typedef char *va_list;
# define va_dcl long va_alist;
# define va_start(list) list = (char *) &va_alist
# define va_end(list)
# define va_arg(list,mode) (list += sizeof(long), *(mode *)(list - sizeof(long)))
