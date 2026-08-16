#include <stdio.h>
#include "def.h"

int routnum;
FILE *debfd;
LOGICAL routerr;
int nodenum, accessnum;
VERT **graph;			/* PORT: was `int **' -- see def.h's VERT */
int progtype;
VERT stopvert, retvert;
VERT START;
