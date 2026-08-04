#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ndir.h>
main()
{
	DIR *d; struct direct *e; struct stat s, ss; int n = 0, hit = 0;

	stat(".", &s);
	fprintf(stderr, "cwd ino=%ld dev=%ld\n", (long)s.st_ino, (long)s.st_dev);
	if ((d = opendir("..")) == NULL) { fprintf(stderr, "opendir .. failed\n"); return 1; }
	if (chdir("..") < 0) fprintf(stderr, "chdir .. failed\n");
	fprintf(stderr, "chdir(..) done\n");
	fstat(d->dd_fd, &ss);
	fprintf(stderr, ".. ino=%ld dev=%ld  samedev=%d\n",
	    (long)ss.st_ino, (long)ss.st_dev, s.st_dev == ss.st_dev);
	while ((e = readdir(d)) != NULL) {
		n++;
		if (n <= 4) fprintf(stderr, "  [%s] ino=%ld\n", e->d_name, (long)e->d_ino);
		if (e->d_ino == s.st_ino) { hit = 1; fprintf(stderr, "  MATCH [%s]\n", e->d_name); }
	}
	fprintf(stderr, "entries=%d hit=%d\n", n, hit);
	return 0;
}
