/*
 * The default kmemu_synth(): no synthetic kernel files.
 *
 * This is what the shim's open() reaches in the 48 V8 binaries that are not
 * grovelers.  shim/libkmemu/synth.c has the real one, and a program that links
 * libkmemu gets that instead -- see the note above the call in syscall.c for
 * why the substitution is done this way rather than with a weak reference.
 *
 * ITS OWN FILE, and that is the whole point.  The linker takes an archive
 * member only to resolve a symbol that is still undefined, so with libkmemu on
 * the link line ahead of libv8sys.a this object is simply never pulled in --
 * no duplicate symbol, no link order subtlety beyond that one fact.  Put the
 * same three lines at the bottom of syscall.c instead and every groveler dies
 * with a duplicate definition, because syscall.o is pulled in for v8s_open.
 * Same reasoning as libv8stubs.a being one object per syscall.
 */

int
kmemu_synth(const char *v8path, const char *root)
{
	(void)v8path;
	(void)root;
	return (0);
}
