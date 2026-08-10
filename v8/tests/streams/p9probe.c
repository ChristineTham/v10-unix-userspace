/*
 * A 9P client for the v8fs server -- §8a step 5e.
 *
 * WHY A PROBE AND NOT JUST THE REAL CLIENT.  The real client is in libv8sys
 * and reaches the server through open(2); this one speaks the wire directly.
 * That is the difference between testing the server and testing the pair, and
 * §8a step 5d is the precedent for wanting both: the probe there wrote a file
 * and read it back and every case stayed green under a mutation that made
 * alloc() hand out the same block twice, because a probe's writer and reader
 * are one program and share its beliefs.  What caught it was icheck, fsck and
 * cmp -- readers that know nothing about the probe.
 *
 * So this is the server's independent reader.  It can ask for things the shim
 * would never send (a walk off the end of a path, a clunked fid, a write to a
 * read-only server) and it can see the answers as 9P rather than as an errno.
 *
 * It shares the CODEC with both halves -- shim/p9/p9.c, the same source
 * libv8sys.a and the server get -- and that is deliberate rather than lazy: a
 * hand-rolled encoder here would test the server against a second
 * implementation of the same misunderstanding.  What it does test
 * independently is the SERVER's message construction, because every field it
 * reads was built by the server's own p9_p* calls and is checked against a
 * value this file knows from the image.
 *
 * Prints `key value' lines; run.sh asserts on them.  A DUPLICATED KEY IS
 * SILENT, so each is used once.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "../../shim/p9/p9.h"

static int	fd;
static unsigned char	out[P9_MSIZE], in[P9_MSIZE];
static struct p9buf	tb, rb;
static long		msize = P9_MSIZE;

#define ROOTFID		0
#define TAG		1

static void
die(const char *what)
{
	fprintf(stderr, "p9probe: %s: %s\n", what, strerror(errno));
	exit(2);
}

static void
begin(int type)
{
	p9_hdr(&tb, out, sizeof out, type, TAG);
}

/*
 * Send, receive, and return the reply's type.  The tag is checked here rather
 * than at each call site: this client has one outstanding message at a time,
 * so a reply carrying a different tag is a server that has lost track of the
 * conversation, and every case after it would be reading the wrong answer.
 */
static int
xact(void)
{
	long n;
	p9_u32 tag;
	int type;

	if (p9_send(fd, &tb) < 0) die("send");
	if ((n = p9_recv(fd, in, (long)sizeof in)) <= 0) die("recv");
	p9_init(&rb, in, n);
	(void)p9_g32(&rb);
	type = (int)p9_g8(&rb);
	tag = p9_g16(&rb);
	if (tag != TAG) { fprintf(stderr, "p9probe: tag %u\n", tag); exit(2); }
	return (type);
}

/* The error name out of an Rerror, or "-" if the reply was not one. */
static const char *
errname(int type)
{
	static char e[64];

	if (type != P9_Rerror) return ("-");
	if (p9_gstr(&rb, e, sizeof e) < 0) return ("?");
	return (e);
}

static void
attach(void)
{
	begin(P9_Tversion);
	p9_p32(&tb, P9_MSIZE);
	p9_pstr(&tb, P9_VERSION);
	if (xact() != P9_Rversion) { fprintf(stderr, "no Rversion\n"); exit(2); }
	msize = (long)p9_g32(&rb);
	{
		char v[32];
		p9_gstr(&rb, v, sizeof v);
		printf("version %s\n", v);
		printf("msize-le-ours %d\n", msize <= P9_MSIZE ? 1 : 0);
	}

	begin(P9_Tattach);
	p9_p32(&tb, ROOTFID);
	p9_p32(&tb, P9_NOFID);
	p9_pstr(&tb, "v8");
	p9_pstr(&tb, "");
	if (xact() != P9_Rattach) { fprintf(stderr, "no Rattach\n"); exit(2); }
	{
		struct p9qid q;
		p9_gqid(&rb, &q);
		printf("root-qtdir %d\n", (q.q_type & P9_QTDIR) ? 1 : 0);
		printf("root-qpath %llu\n", (unsigned long long)q.q_path);
	}
}

/*
 * Walk `n' names from ROOTFID to `nf'.  Returns the number of qids the server
 * reported, or -1 for an Rerror -- and the two are different answers, which is
 * the whole reason this returns a count.  9P says a walk that fails on the
 * FIRST name is an error and one that fails later is a SHORT Rwalk, so a
 * client can tell "no such file" from "no such directory on the way to it".
 */
static int
walk(p9_u32 nf, int n, char **names)
{
	int i, type;
	struct p9qid q;

	begin(P9_Twalk);
	p9_p32(&tb, ROOTFID);
	p9_p32(&tb, nf);
	p9_p16(&tb, (p9_u32)n);
	for (i = 0; i < n; i++) p9_pstr(&tb, names[i]);
	if ((type = xact()) == P9_Rerror) return (-1);
	if (type != P9_Rwalk) { fprintf(stderr, "no Rwalk (%d)\n", type); exit(2); }
	n = (int)p9_g16(&rb);
	for (i = 0; i < n; i++) p9_gqid(&rb, &q);
	return (n);
}

static int
qtype_of(p9_u32 fid)
{
	begin(P9_Tstat);
	p9_p32(&tb, fid);
	if (xact() != P9_Rstat) return (-1);
	{
		struct p9stat s;
		(void)p9_g16(&rb);		/* the outer count */
		if (p9_gstat(&rb, &s) < 0) return (-1);
		return ((s.s_qid.q_type & P9_QTDIR) ? 1 : 0);
	}
}

static int
dial(const char *path)
{
	struct sockaddr_un sa;
	int s;

	if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) die("socket");
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	strncpy(sa.sun_path, path, sizeof sa.sun_path - 1);
	if (connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) die("connect");
	return (s);
}

int
main(int argc, char **argv)
{
	char *w1[1], *w2[2];
	int type;
	long total, got;
	FILE *save;

	if (argc != 3) {
		fprintf(stderr, "usage: p9probe socket readback\n");
		return (2);
	}
	fd = dial(argv[1]);

	attach();

	/* ---------------------------------------------------- walking */

	w1[0] = "hello";
	printf("walk-hello %d\n", walk(1, 1, w1));
	printf("hello-isdir %d\n", qtype_of(1));

	w2[0] = "sub"; w2[1] = "deep";
	printf("walk-sub-deep %d\n", walk(2, 2, w2));
	printf("deep-isdir %d\n", qtype_of(2));

	/*
	 * A name that is not there, and a name that is not there BELOW one that
	 * is.  The pair is the point: the first must be an Rerror and the
	 * second a short Rwalk, and a server that answered them the same way
	 * would be indistinguishable from one that worked, on every path that
	 * exists.
	 */
	w1[0] = "nosuch";
	printf("walk-missing %d\n", walk(3, 1, w1));
	printf("walk-missing-err %s\n", errname(P9_Rerror));

	w2[0] = "sub"; w2[1] = "nosuch";
	printf("walk-short %d\n", walk(4, 2, w2));

	/* ...and walking THROUGH a plain file is ENOTDIR, not ENOENT. */
	w2[0] = "hello"; w2[1] = "x";
	printf("walk-thru-file %d\n", walk(5, 2, w2));

	/* A zero-name walk clones the fid; the clone must stat the same file. */
	printf("walk-clone %d\n", walk(6, 0, (char **)0));
	printf("clone-isdir %d\n", qtype_of(6));

	/* ------------------------------------------------------ stat */

	begin(P9_Tstat);
	p9_p32(&tb, 1);				/* hello */
	if (xact() == P9_Rstat) {
		struct p9stat s;
		long outer = (long)p9_g16(&rb);
		if (p9_gstat(&rb, &s) == 0) {
			/*
			 * Rstat is size[4] type[1] tag[2] outer[2] inner[2],
			 * so the inner count is at offset 9 -- and the two
			 * must differ by exactly two.  Read off the bytes
			 * rather than from the decoder, because the decoder is
			 * the thing under test.
			 */
			printf("stat-outer-inner-differ-by-2 %d\n",
			    outer == (long)(in[9] | (in[10] << 8)) + 2 ? 1 : 0);
			/*
			 * THE QID PATH, AND IT IS THE ONLY FIELD HERE THAT CAN
			 * IDENTIFY THE FILE.  s_name is the name this client
			 * sent in the Twalk, echoed back out of the fid -- so
			 * a server that walked to the wrong inode entirely
			 * would still print the name asked for.  The qid path
			 * is i_number, which comes from the directory entry.
			 */
			printf("stat-qpath %llu\n",
			    (unsigned long long)s.s_qid.q_path);
			printf("stat-name %s\n", s.s_name);
			printf("stat-len %llu\n", (unsigned long long)s.s_length);
			printf("stat-mode %o\n", s.s_mode & 07777);
			printf("stat-uid %s\n", s.s_uid);
			printf("stat-isdir %d\n", (s.s_mode & P9_DMDIR) ? 1 : 0);
		} else
			printf("stat-name DECODE-FAILED\n");
	} else
		printf("stat-name RERROR\n");

	/* ------------------------------------------------------ read */

	/*
	 * A read on a fid that has not been opened must fail.  9P has no rule
	 * that a Tread implies an open, and a server that allowed it would be
	 * handing out file contents to a client that never asked permission --
	 * which is where access() is consulted.
	 */
	begin(P9_Tread);
	p9_p32(&tb, 2);
	p9_p64(&tb, 0);
	p9_p32(&tb, 16);
	printf("read-unopened %s\n", errname(xact()));

	begin(P9_Topen);
	p9_p32(&tb, 2);				/* sub/deep */
	p9_p8(&tb, P9_OREAD);
	type = xact();
	printf("open-deep %d\n", type == P9_Ropen ? 1 : 0);
	if (type == P9_Ropen) {
		struct p9qid q;
		p9_gqid(&rb, &q);
		printf("open-iounit-positive %d\n", p9_g32(&rb) > 0 ? 1 : 0);
	}

	/* A fid that is open may not be walked -- the spec, and the reason is
	 * that a walk would move the file an offset already refers to. */
	w1[0] = "x";
	printf("walk-open-fid %d\n", walk(7, 1, w1));

	if ((save = fopen(argv[2], "w")) == 0) die("readback");
	total = 0;
	for (;;) {
		long want = msize - P9_IOHDRSZ;
		unsigned char *d;

		begin(P9_Tread);
		p9_p32(&tb, 2);
		p9_p64(&tb, (p9_u64)total);
		p9_p32(&tb, (p9_u32)want);
		if (xact() != P9_Rread) { printf("read-failed 1\n"); break; }
		got = (long)p9_g32(&rb);
		if (got == 0) break;
		if ((d = p9_gdata(&rb, got)) == 0) { printf("read-short 1\n"); break; }
		fwrite(d, 1, (size_t)got, save);
		total += got;
		if (total > 1 << 20) { printf("read-runaway 1\n"); break; }
	}
	fclose(save);
	/*
	 * The count, and nothing else.  A checksum was here and is gone: run.sh
	 * cmps the readback against the file that went into the image, which is
	 * the same claim made by a reader that knows the answer independently.
	 * An output nothing asserts on is noise.
	 */
	printf("read-total %ld\n", total);

	/*
	 * A read PAST the end is not an error; it is zero bytes.  V8's own
	 * readi does the same thing and every program in the tree depends on
	 * it, since that is how a copy loop terminates.
	 */
	begin(P9_Tread);
	p9_p32(&tb, 2);
	p9_p64(&tb, (p9_u64)(total + 4096));
	p9_p32(&tb, 64);
	if (xact() == P9_Rread) printf("read-past-end %u\n", p9_g32(&rb));
	else printf("read-past-end RERROR\n");

	/* --------------------------------------------- directory read */

	/*
	 * A DIRECTORY READ RETURNS 9P STATS, not the raw 16-byte records the
	 * image holds -- that is what makes this plain 9P2000 rather than a
	 * private protocol, and it is what lets a foreign client mount the
	 * world.  "." and ".." are in the listing on purpose; v8fsd.c argues
	 * it from the seam rule.
	 */
	printf("walk-root-clone %d\n", walk(8, 0, (char **)0));
	begin(P9_Topen);
	p9_p32(&tb, 8);
	p9_p8(&tb, P9_OREAD);
	printf("open-root %d\n", xact() == P9_Ropen ? 1 : 0);

	{
		int ndot = 0, ndotdot = 0, nhello = 0, nsub = 0, nent = 0;
		long doff = 0;

		for (;;) {
			struct p9buf db;
			struct p9stat s;
			unsigned char *d;
			long n2;

			begin(P9_Tread);
			p9_p32(&tb, 8);
			p9_p64(&tb, (p9_u64)doff);
			p9_p32(&tb, 512);
			if (xact() != P9_Rread) { printf("dir-read-failed 1\n"); break; }
			n2 = (long)p9_g32(&rb);
			if (n2 == 0) break;
			if ((d = p9_gdata(&rb, n2)) == 0) break;
			/*
			 * BARE STATS, one after another, with NO outer count.
			 * Rstat has one and a directory read does not, which
			 * is easy to get backwards -- and getting it backwards
			 * shifts every field by two and decodes as a plausible
			 * short name rather than as an error.
			 */
			p9_init(&db, d, n2);
			while (p9_len(&db) < n2) {
				if (p9_gstat(&db, &s) < 0) break;
				nent++;
				if (strcmp(s.s_name, ".") == 0) ndot++;
				else if (strcmp(s.s_name, "..") == 0) ndotdot++;
				else if (strcmp(s.s_name, "hello") == 0) nhello++;
				else if (strcmp(s.s_name, "sub") == 0) nsub++;
			}
			doff += n2;
			if (doff > 1 << 16) break;
		}
		printf("dir-entries %d\n", nent);
		printf("dir-dot %d\n", ndot);
		printf("dir-dotdot %d\n", ndotdot);
		printf("dir-hello %d\n", nhello);
		printf("dir-sub %d\n", nsub);
	}

	/* ------------------------------------------------ refusals */

	begin(P9_Twrite);
	p9_p32(&tb, 2);
	p9_p64(&tb, 0);
	p9_p32(&tb, 4);
	p9_pdata(&tb, "xxxx", 4);
	printf("write-refused %s\n", errname(xact()));

	begin(P9_Tclunk);
	p9_p32(&tb, 1);
	printf("clunk %d\n", xact() == P9_Rclunk ? 1 : 0);
	printf("stat-after-clunk %d\n", qtype_of(1));

	/*
	 * An unknown message type, and a Tauth this server does not offer.
	 * Both must be Rerror rather than silence: a server that ignored a
	 * message it did not know would leave the client waiting forever,
	 * which is the failure mode that reads as a hang rather than an error.
	 */
	begin(200);
	printf("unknown-type %s\n", errname(xact()));

	begin(P9_Tauth);
	p9_p32(&tb, 20);
	p9_pstr(&tb, "v8");
	p9_pstr(&tb, "");
	printf("auth-refused %s\n", errname(xact()));

	/* ------------------------------------------- a SECOND connection
	 *
	 * THE WHOLE REASON THE SERVER HAS A poll() LOOP, and until this it was
	 * untested: every case above uses one connection, so a server that
	 * accepted a second and then ignored it would have passed all of them.
	 * "One connection per open file" is the design -- the socket IS the
	 * descriptor -- so two at once is the ordinary case, not the exotic one.
	 *
	 * IT ATTACHES AS FID 0, WHICH IS ALREADY IN USE ON THE FIRST
	 * CONNECTION.  That is the sharp part: 9P fid spaces are per
	 * connection, so this must succeed.  A server with one shared table
	 * would answer EEXIST, and would have looked perfectly correct to every
	 * case above.
	 */
	{
		int first = fd, second = dial(argv[1]);

		fd = second;
		begin(P9_Tversion);
		p9_p32(&tb, P9_MSIZE);
		p9_pstr(&tb, P9_VERSION);
		printf("conn2-version %d\n", xact() == P9_Rversion ? 1 : 0);

		begin(P9_Tattach);
		p9_p32(&tb, ROOTFID);		/* 0, in use on the other one */
		p9_p32(&tb, P9_NOFID);
		p9_pstr(&tb, "v8");
		p9_pstr(&tb, "");
		printf("conn2-attach-same-fid %d\n", xact() == P9_Rattach ? 1 : 0);

		w1[0] = "hello";
		printf("conn2-walk %d\n", walk(1, 1, w1));
		begin(P9_Topen);
		p9_p32(&tb, 1);
		p9_p8(&tb, P9_OREAD);
		printf("conn2-open %d\n", xact() == P9_Ropen ? 1 : 0);
		begin(P9_Tread);
		p9_p32(&tb, 1);
		p9_p64(&tb, 0);
		p9_p32(&tb, 512);
		if (xact() == P9_Rread) printf("conn2-read %u\n", p9_g32(&rb));
		else printf("conn2-read RERROR\n");

		/*
		 * ...and the FIRST connection is still there and still knows its
		 * own fids.  Interleaved deliberately: the server has answered
		 * six messages on the second connection since the first said
		 * anything, so a fid table that belonged to the server rather
		 * than to the connection would have been overwritten by now.
		 */
		fd = first;
		printf("conn1-still-alive %d\n", qtype_of(8));

		close(second);
		/*
		 * A CLOSE IS NOT AN ERROR AND THE SERVER MUST SURVIVE IT.  A V8
		 * program exiting closes every descriptor it holds, so this is
		 * the ordinary end of a connection rather than a fault -- and a
		 * server that took SIGPIPE or dropped the poll loop here would
		 * take every other client with it.
		 */
		printf("conn1-after-conn2-closed %d\n", qtype_of(8));
	}

	close(fd);
	return (0);
}
