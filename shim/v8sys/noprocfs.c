/*
 * The default /proc: there isn't one.
 *
 * ITS OWN FILE, and the file next door explains why in advance -- nokmemu.c
 * says that putting a second default beside kmemu_synth() means every groveler
 * dies with a duplicate definition, because the object gets pulled in for the
 * OTHER symbol.  Written here first, put in nokmemu.c anyway, and the link
 * failed exactly as described:
 *
 *	duplicate symbol '_kmemu_synth' in:
 *	    libkmemu.a[4](synth.o)
 *	    libv8sys.a[5](nokmemu.o)
 *
 * vfs.c references kmemu_procfs() unconditionally, so that object is now always
 * pulled in; had they shared a file it would have dragged kmemu_synth in with
 * it.  One substitutable default per object, every time.
 *
 * WHAT THE NULL MEANS.  vfs.c's mount table has rows for /proc whose type comes
 * from here, so in a binary without libkmemu nothing claims the path and it
 * falls through to the host -- where macOS has no /proc, so a V8 program gets
 * ENOENT.  That is the truth rather than an empty directory.
 *
 * /proc lives in libkmemu rather than beside the passthrough type because it
 * answers from proc_listpids(2) and proc_pidinfo(2), and those are libc -- the
 * sanctioned exception PLAN.md section 7 grants for reading facts about the
 * running system.  Keeping it on that side is what stops every V8 binary from
 * importing libSystem to have a /proc it never opens.
 */

struct v8fstyp;

struct v8fstyp *
kmemu_procfs(void)
{
	return ((struct v8fstyp *)0);
}
