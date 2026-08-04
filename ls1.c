#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ndir.h>
main(argc, argv) char **argv;
{
	DIR *d; struct direct *e; struct stat s;
	char path[256]; int n = 0;
	if ((d = opendir(argv[1])) == NULL) { printf("openfail\n"); return 1; }
	while ((e = readdir(d)) != NULL) {
		n++;
		strcpy(path, argv[1]); strcat(path, "/"); strcat(path, e->d_name);
		s.st_ino = 0;
		stat(path, &s);
		if (strcmp(e->d_name, "private") == 0 || n < 4)
			printf("%-16s readdir_ino=%ld stat_ino=%ld\n",
			    e->d_name, (long)e->d_ino, (long)s.st_ino);
	}
	printf("entries=%d\n", n);
	return 0;
}
