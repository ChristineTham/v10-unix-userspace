#define	JERQ
/*
 * mc - columnate
 *
 * mc[-][-LINEWIDTH][-t][file...]
 *	-suppresses break on colon
 *	-LINEWIDTH sets width of line in which to columnate(default 80)
 *	-t suppresses expanding multiple blanks into tabs
 *
 */
#include	<stdio.h>
#ifdef	JERQ
#include	<sgtty.h>
/*
 * PORT: was #include "/usr/jerq/include/jioctl.h".
 *
 * An absolute path into the 1985 filesystem, which is the one kind of include
 * cpp cannot be told to resolve elsewhere -- no -I affects it.  The header is
 * authentic and unchanged; the rootfs carries it at usr/include/jioctl.h,
 * copied from jerq/include (see the Makefile).  Identical change, for the
 * identical reason, to src/cmd/ls.c:11 and src/lib/libtermlib/termcap.c.
 *
 * Note that the block IS live here where it is dead in the other two: mc.c's
 * FIRST LINE is `#define JERQ', so the file turns it on itself.  The ioctl
 * still fails -- the shim implements no JWINSIZE -- and mc keeps WIDTH, which
 * is upstream's own fallback two lines further on.
 */
#include	<jioctl.h>
#endif
#define WIDTH		80
#define NWALLOC		1024
#define NALLOC		4096
int linewidth=WIDTH; 
int colonflag=1; 
int tabflag=1; 
char *cbuf, *cbufp; 
char **word; 
FILE *file; 
char *malloc(); 
char *realloc(); 
int maxwidth=0; 
int nalloc=NALLOC; 
int nwalloc=NWALLOC; 
int nchars=0; 
int nwords=0; 

main(argc, argv)
	char *argv[]; 
{
	FILE *fopen(); 
	register i; 
	char buf[BUFSIZ]; 
#ifdef	JERQ
	struct winsize wbuf;

	if(ioctl(1, JWINSIZE, &wbuf)==0){
		linewidth=wbuf.bytesx;
		if(linewidth<0)
			linewidth=WIDTH; 
	}
#endif
	if(argc>1){
		/* PORT: `argv[1]!=0 &&' added.  Every arm of the switch below
		 * consumes an argument, so once the loop has eaten the last
		 * option argv[1] is the vector's NULL terminator -- reached by
		 * `mc -20', `mc -t', `mc -' and any other option in the final
		 * position.  On the VAX address 0 held crt0's first byte, 0x00
		 * (V8's binaries are ZMAGIC, so N_TXTOFF is 1024 and the
		 * a.out header is never mapped), which is not '-', so the loop
		 * simply ended and the argc==1 test below sent mc to stdin.
		 * macOS leaves page 0 unmapped, so `mc -20 < file' SIGSEGV'd
		 * after reading the width and before reading a byte of input.
		 *
		 * Byte for byte the shape diffh.c:93 carries, and ncheck's -i
		 * loop, and cpio's option guard; PLAN.md S4i has the class.
		 * The guard restores the ANSWER -- the loop ends and mc reads
		 * stdin -- rather than merely removing the fault. */
		while(argv[1]!=0 && *argv[1]=='-'){
			--argc; argv++; 
			switch(argv[0][1]){
			case '\0':
				colonflag=0; 
				break; 
			case 't':
				tabflag=0; 
				break; 
			default:
				linewidth=atoi(&argv[0][1]); 
				if(linewidth<=1)
					linewidth=WIDTH; 
				break; 
			}
		}
	}
	setbuf(stdout, buf); 
	cbuf=cbufp=malloc(NALLOC); 
	word=(char **)malloc(NWALLOC *(sizeof *word)); 
	if(word==0 || cbuf==0)
		error("out of memory"); 
	if(argc==1){
		file=stdin; 
		readbuf(); 
	}else{
		colonflag=0; 
		for(i=1; i<argc; i++){
			file=freopen(*++argv, "r", stdin); 
			if(file==NULL)
				fprintf(stderr, "mc: can't open %s\n", *argv); 
			else
				readbuf(); 
		}
	}
	columnate(); 
	exit(0); 
}
error(s)
	char *s; 
{
	fprintf(stderr, "mc: %s\n", s); 
	exit(1); 
}
readbuf()
{
	register c, lastwascolon=0; 
	do{
		if(nchars++>=nalloc){
			if((cbuf=realloc(cbuf, nalloc+=NALLOC))==0)
				error("out of memory"); 
			cbufp=cbuf+nchars-1; 
		}
		*cbufp++=c=getc(file); 
		if(colonflag && c==':')
			lastwascolon++; 
		else if(lastwascolon){
			if(c=='\n'){
				register n=1; 
				--nchars; 	/* skip newline */
				while(nchars>0 && cbuf[--nchars]!='\n')
					n++; 
				if(cbuf[nchars]=='\n')
					nchars++; 
				columnate(); 
				fwrite(cbuf+nchars, 1, n, stdout); 
				nchars=0; 
				cbufp=cbuf; 
			}
			lastwascolon=0; 
		}
	}while(c!=EOF); 
}
scanwords(){
	register char *p, *q; 
	register i; 
	nwords=0; 
	maxwidth=0; 
	for(p=q=cbuf, i=0; i<nchars; i++){
		if(*p++=='\n'){
			if(nwords>=nwalloc){
				if((word=(char **)realloc((char *)word, (nwalloc+=NWALLOC)*sizeof(*word)))==0)
					error("out of memory"); 
			}
			word[nwords++]=q; 
			p[-1]=0; 
			if(p-q>maxwidth)
				maxwidth=p-q; 
			q=p; 
		}
	}
}
int words_per_line; 
int nlines; 
int col; 
int tabcol; 
int endcol; 
int maxcol; 
columnate(){
	register char *p; 
	register i, j; 
	scanwords(); 
	if(nwords==0)
		return; 
	words_per_line=linewidth/maxwidth; 
	if(words_per_line<=0)
		words_per_line=1; 
	nlines=(nwords+words_per_line-1)/words_per_line; 
	for(i=0; i<nlines; i++){
		col=0; 
		endcol=0; 
		for(j=0; i+j<nwords; ){
			endcol+=maxwidth; 
			p=word[i+j]; 
			while(*p){
				putchar(*p++); 
				col++; 
			}
			if(i+(j+=nlines)<nwords){
				tabcol=(col|07)+1; 
				if(tabflag)
					while(tabcol<=endcol){
						putchar('\t'); 
						col=tabcol; 
						tabcol+=8; 
					}
				while(col<endcol){
					putchar(' '); 
					col++; 
				}
			}
		}
		putchar('\n'); 
	}
}
