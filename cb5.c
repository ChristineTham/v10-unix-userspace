union store { union store *ptr; long dummy[1]; int calloc; };
long a(p) union store *p; { return (long)(p->ptr) & ~1; }
long b(p) union store *p; { return (long)(p->ptr) & -2; }
long c(p) union store *p; { return (long)(p->ptr) & ~1L; }
