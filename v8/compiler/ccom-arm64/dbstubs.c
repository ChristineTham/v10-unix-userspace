/*
 * Debug-symbol hooks -- stubs.
 *
 * vax/debug.c emits .stabs records for `pi`, V8's source debugger.  The format
 * is tied to the VAX a.out symbol table, and `pi` is not part of this port, so
 * none of it carries over.  The machine-independent passes call these hooks
 * unconditionally from the grammar (common/cgram.c calls dbline, dblbrac,
 * dbrbrac and dbnargs), so they have to exist even when they do nothing.
 *
 * When debugging support does arrive it should be DWARF emitted through the
 * host assembler's .loc/.file directives, not stabs -- which is why these are
 * empty rather than a half-ported stabs writer that would have to be thrown
 * away.  Until then, code compiled by v8cc is debuggable only at the assembly
 * level, which lldb handles fine given the honest AAPCS64 frame that
 * compiler/ccom-arm64/emit.c builds.
 */

dbnargs(n)	int n;		{ }
dbfunbeg(p)	char *p;	{ }
dbfunarg(p)	char *p;	{ }
dbfunret()			{ }
dbfunend(lab)	int lab;	{ }
dblbrac()			{ }
dbrbrac()			{ }
dbline()			{ }
ejsdb()				{ }

dbfile(pname)
	char *pname;
{
	return (0);
}
