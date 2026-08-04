#include <stdio.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <ndir.h>
main()
{
	struct stat s;
	stat(".", &s);
	printf("sizeof(struct stat)=%d  ino_off=%d dev_off=%d  ino=%ld dev=%ld\n",
	    (int)sizeof(struct stat),
	    (int)((char *)&s.st_ino - (char *)&s),
	    (int)((char *)&s.st_dev - (char *)&s),
	    (long)s.st_ino, (long)s.st_dev);
	return 0;
}
