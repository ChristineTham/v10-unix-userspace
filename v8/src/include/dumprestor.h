#define NTREC   	10
#define MLEN    	16
#define MSIZ    	4096

#define TS_TAPE 	1
#define TS_INODE	2
#define TS_BITS 	3
#define TS_ADDR 	4
#define TS_END  	5
#define TS_CLRI 	6
#define MAGIC   	(int)60011
#define MAGIC4K		(int)60101
#define CHECKSUM	(int)84446

/*
 * PATCHED: c_date and c_ddate spelled `int' where upstream spells `time_t'.
 *
 * THIS IS A TAPE RECORD, and it is the same argument <sys/ino.h> makes about a
 * disk record, one layer out: the other end is not another program in this
 * port.  V8's VAX compiler defined NOLONG, so `long' there was 32 bits and both
 * of these are four bytes; daddr_t is already narrowed globally by
 * <sys/types.h>, and time_t cannot be, because it crosses the shim seam to
 * macOS -- so it is narrowed HERE, per field, exactly as di_atime is.
 *
 * THE STRUCT ALREADY HAD ONE VAX-SHAPED HALF, which is what makes leaving these
 * alone worse than either choice: c_dinode is a `struct dinode', fixed at 64
 * bytes by S8a step 4a.  At eight-byte times the header would be VAX-correct
 * from c_dinode onwards and shifted by eight before it.
 *
 * The offsets are the format, not an implementation detail.  dumptape.c's
 * `char tblock[NTREC][BSIZE(0)]' writes each record as exactly BSIZE(0) = 1024
 * bytes, so what goes to tape is the FIRST 1024 BYTES of this struct -- the
 * 100-byte header plus 924 of c_addr, with the rest of c_addr truncated by
 * design.  Widening a header field does not grow the record, it slides
 * everything after it and silently drops eight bytes off the end of c_addr.
 *
 * AND THE CHECKSUM DOES NOT NOTICE.  restor.c's checksum() sums
 * BSIZE(0)/sizeof(int) = 256 ints over those same 1024 bytes and requires
 * CHECKSUM; dump writes a compensating word.  That catches a writer and a
 * reader who disagree, and here both are ours, so both would be wrong
 * together.  tests/mkfs asserts the layout on the BYTES of a written tape,
 * which is the only thing that can see it -- the same rule the -DDIRSIZ=14
 * group already follows.
 *
 * struct idates below is deliberately NOT narrowed: /etc/ddate is a TEXT file
 * (see DUMPOUTFMT/DUMPINFMT), so id_ddate never leaves memory as bytes and
 * ctime(&itwalk->id_ddate) is correct at eight.  One struct here is a wire
 * format and one is not, and that is the whole difference.
 */
struct	spcl {
	int	c_type;
	int	c_date;		/* upstream time_t -- see above */
	int	c_ddate;	/* upstream time_t -- see above */
	int	c_volume;
	daddr_t	c_tapea;
	ino_t	c_inumber;
	int	c_magic;
	int	c_checksum;
	struct	dinode	c_dinode;
	int	c_count;
	char	c_addr[BSIZE(0)];
} spcl;

struct	idates {
	char	id_name[16];
	char	id_incno;
	time_t	id_ddate;
};

#define	DUMPOUTFMT	"%-16s %c %s"		/* for printf */
						/* name, incno, ctime(date) */
#define	DUMPINFMT	"%16s %c %[^\n]\n"	/* inverse for scanf */
