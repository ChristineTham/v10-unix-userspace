/*
 * v8 -- enter the Eighth Edition world.
 *
 * V8's own launcher, adapted.  The original is twelve lines and does four
 * things: chroot("/v8"), chdir, drop privilege, exec the shell.  Three of them
 * survive; the two that changed are changed for reasons this port has already
 * had to establish, and both are recorded here rather than in a commit message
 * nobody will find.
 *
 * chroot IS GONE, and nothing replaces it here, because the chroot already
 * happened.  rootpath() in shim/v8sys/syscall.c resolves V8 paths inside the
 * world, and every binary this port produces links libv8sys, so each one is
 * jailed by construction -- per binary rather than per process tree.  A real
 * chroot(2) is not available anyway: it needs root, and every V8 binary here is
 * a Mach-O linked against libSystem, so the jail would need the SIP-protected
 * dyld shared cache inside it.  See PLAN.md S4b.
 *
 * setuid/setgid ARE GONE.  V8 dropped to uid 3 / gid 4 because it was a real
 * multi-user system and this was a guest login.  Ours is not: PLAN.md S1 names
 * multi-user login as a non-goal, the Mac owns identity, and /etc/passwd is
 * synthesized to name the person actually running this.  Dropping to a uid that
 * does not exist on the host would break every file operation and buy nothing.
 *
 * What is left is honest: set the working directory, set the V7 umask, and hand
 * over to V8's shell.
 */

#include <stdio.h>

main(argc, argv)
	char **argv;
{
	/*
	 * "/" is the world's root -- the shim maps it -- so this is V8's own
	 * `chdir(argc>1? argv[1]: "/")` unchanged in meaning as well as in text.
	 */
	if (chdir(argc > 1 ? argv[1] : "/") < 0) {
		printf("v8: can't chdir\n");
		exit(1);
	}
	umask(02);
	execl("/bin/sh", "-", 0);
	printf("v8: can't exec /bin/sh\n");
	exit(1);
}
