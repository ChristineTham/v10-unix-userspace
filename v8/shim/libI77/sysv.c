/*
 * The two System V libc internals libI77 needs and V8 does not have.
 *
 * V8's shipped /usr/lib/libI77.a CANNOT BE LINKED against V8's own libc, and
 * this file is what closes that gap without editing Bell Labs' source.  The
 * measurement, taken across every .a in the distribution:
 *
 *	symbol		found in
 *	_sobuf		lib/libc.a, usr/lib/11libc.a		-- resolves
 *	setvbuf		usr/lib/libI77.a AND NOWHERE ELSE	-- does not
 *	_bufendtab	usr/lib/libI77.a AND NOWHERE ELSE	-- does not
 *
 * So a Fortran program on a real V8 had two undefined symbols at ld time.
 * libI77 is a System V library that was dropped into V8 and never reconciled;
 * err.c:93 is upstream's own comment -- "IOLBUF and setvbuf only in system 5+"
 * -- sitting four lines above an UNGUARDED call to it.
 *
 * This is layer 2, so it is ours to write, and it is a separate file from
 * libv8c on purpose: adding setvbuf to libc would invent a C library V8 never
 * shipped, and every V8 binary would carry it.  The object is archived INTO
 * libI77.a instead, because the gap belongs to libI77's own assumptions and
 * because the f77 driver's liblist is fixed at "-lF77 -lI77 -lm -lc" -- there
 * is no fourth library for it to name.  shim/libm/dummy.c is the precedent for
 * a small port-owned object standing beside an authentic library.
 *
 * WHY THE HEADER SIDE IS SETTLED FIRST.  libI77 also ships its own stdio.h,
 * which is System V's, and it disagrees with V8's about layout: _flag is char
 * where V8's is short, so _file sits at offset 25 rather than 26 and fileno()
 * reads the high byte of _flag; _NFILE is 128 against 120; _IOLBF is 0100
 * against V8's 0200 (which is V8's _IOSTRG); and the field order flips on
 * "#if vax", a macro this port deliberately does not define.  Two archives
 * disagreeing about FILE by one byte is the DIRSIZ trap arriving through a
 * vendored header.  The Makefile therefore keeps libI77's source directory OFF
 * the include path, so <stdio.h> is V8's -- which also means the _IOLBF that
 * err.c passes us below is V8's 0200 and needs no translation.
 */

#include <stdio.h>

/*
 * setvbuf, in libI77's own PRE-ANSI argument order.
 *
 * ANSI spells it (fp, buf, type, size).  err.c:81 and err.c:97 call
 * setvbuf(fp, _IOLBF, 0, 0) -- type second -- and the comment upstream left on
 * the second one is "the buf arg in setvbuf?", so they were unsure at the time.
 * The call sites are the specification here; there is no ANSI caller to serve.
 *
 * Only the buffering-mode bits are honoured.  buf and size are ignored because
 * both calls pass 0 for both, and a setvbuf that allocated on their behalf
 * would be answering a question nobody asked -- err.c:80 does its own
 * setbuf(stderr, malloc(BUFSIZ)) one line before the first call.
 *
 * It is NOT a no-op, and stderr is why.  V8's libc line-buffers a tty stdout
 * by itself (flsbuf.c:38-42 sets _IOLBF on the first write when _base is NULL
 * and isatty), so for err.c:97 this function is redundant.  It does no such
 * thing for stderr, so dropping err.c:81 on the floor would leave diagnostics
 * fully buffered and arriving late -- which is observable, and wrong.
 */
setvbuf(fp, type, buf, size)
FILE *fp;
int type;
char *buf;
int size;
{
	fp->_flag &= ~(_IONBF|_IOLBF);
	fp->_flag |= type & (_IONBF|_IOLBF);
	return (0);
}

/*
 * _bufend(p), reached as v8_bufend by -D_bufend=v8_bufend on the two objects
 * that use it.  A function rather than a macro because V8's cpp does not
 * support a function-like -D: measured, "cc -D'_bufend(p)=...'" leaves the
 * name unexpanded and ccom reports "call of non-function".  The object-like
 * rename into a real call is what does work, and it costs a bl.
 *
 * libI77's stdio.h spells it _bufendtab[(p)->_file] -- a System V table of
 * per-stream buffer ends.  V8 needs no table, because every stdio buffer in
 * V8 is exactly BUFSIZ bytes: flsbuf.c:25 is the literal "base+BUFSIZ".  So
 * the answer is computable from the stream, and being computed it cannot go
 * stale the way a table can.
 *
 * THE NULL CASE IS THE ONE THAT MATTERS, and all three call sites are the same
 * idiom -- wrtfmt.c:48, wrtfmt.c:58, wsfe.c:41 are each
 *
 *	if (cf->_ptr + n < _bufend(cf))  cf->_ptr += n;
 *	else                             fseek(cf, n, 1);
 *
 * i.e. "can I skip forward inside the buffer, or must I seek?", for the T and
 * TL format edit descriptors and for record-end padding.  There is always a
 * correct fallback, so an answer that makes the test FALSE is safe and merely
 * slower.
 *
 * THE NULL ARM IS DEFENSIVE AND NOT LOAD-BEARING, and this paragraph said
 * otherwise until it was measured.  The claim was that returning _base+BUFSIZ
 * unconditionally lets an unbuffered stream -- _base NULL, so the test becomes
 * 0 + n < 4096 -- advance _ptr into low memory.  The first half is true and the
 * consequence is not: with _IONBF every putc goes through _flsbuf's unbuffered
 * arm, which writes the character directly and never dereferences _ptr.
 * Measured by removing the arm and running a probe that calls setbuf(stdout,0)
 * first, which is what f_init does on a tty (err.c:94-96): both spellings print
 * the same bytes.  No case is written for it, deliberately -- there is nothing
 * observable to assert, and a case with no difference to detect is the vacuous
 * kind this tree keeps finding.
 *
 * It is kept because it is one comparison and because advancing _ptr off NULL
 * is undefined whether or not anything later reads it -- the do_seek lesson,
 * where a check the compiler is entitled to delete was reached only through UB
 * that happened to give the right answer.  What it is NOT is a guard, and the
 * distinction is the whole reason this note is here.
 */
unsigned char *
v8_bufend(p)
FILE *p;
{
	if (p->_base == NULL)
		return (p->_base);
	return (p->_base + BUFSIZ);
}
