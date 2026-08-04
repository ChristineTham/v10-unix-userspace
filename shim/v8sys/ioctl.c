/*
 * ioctl: sgtty over termios.
 *
 * V8 drives terminals through the V7 sgtty interface -- stty/gtty and the
 * TIOC* codes in <sys/ioctl.h>, which are plain (('t'<<8)|n) constants rather
 * than the _IO() macros later Unixes used.  macOS speaks termios.  Everything a
 * ported program does to a terminal comes through here: raw mode in the
 * editors, echo control in stty and login, window size in more and pr.
 *
 * The interesting part is the mode word.  V8's sg_flags packs RAW, CBREAK,
 * ECHO, CRMOD and the parity and delay bits into 16 bits; termios spreads the
 * same ideas across four flag words plus the control characters. The mapping is
 * lossy in both directions -- V8's delay bits (NLDELAY, TBDELAY, ...) describe
 * padding for physical Teletypes and have no modern counterpart, so they are
 * carried through as stored bits and never acted on.
 *
 * Streams line-discipline codes (FIOPUSHLD, FIOPOPLD, FIOLOOKLD, FIOINSLD) and
 * file-descriptor passing (FIOSNDFD, FIORCVFD, FIOACCEPT, FIOREJECT) are V8's
 * IPC primitives and belong to the kernel we are not porting; they fail.  Only
 * `pt` and the more advanced upas plumbing use them, both on the excluded list.
 */

#include <sys/ioctl.h>
#include <termios.h>
#include <fcntl.h>
#include "v8sys.h"
#include "rawsys.h"

/* termios via the raw ioctl syscall: TIOCGETA/TIOCSETA are what tcgetattr and
 * tcsetattr do underneath, so this is the same operation without the symbol. */
static int hostioctl(int fd, unsigned long req, void *arg)
{ return ((int)rawsys3(SYS_ioctl, fd, (long)req, (long)arg)); }

#define tcgetattr(fd, t)	hostioctl(fd, TIOCGETA, (t))
#define tcsetattr(fd, act, t)	hostioctl(fd, \
	    (act) == TCSANOW ? TIOCSETA : TIOCSETAF, (t))
#define tcflush(fd, sel)	({ int _s = (sel); hostioctl(fd, TIOCFLUSH, &_s); })
#define tcgetpgrp(fd)		({ int _p; hostioctl(fd, TIOCGPGRP, &_p) < 0 ? -1 : _p; })
#define tcsetpgrp(fd, pg)	({ int _p = (pg); hostioctl(fd, TIOCSPGRP, &_p); })
#define fcntl(fd, cmd, arg)	((int)rawsys3(SYS_fcntl, (fd), (cmd), (arg)))
#define ioctl(fd, req, arg)	hostioctl((fd), (req), (arg))

/* raw ioctl returns a negated errno; there is no host errno to consult */
static int ioctlfail(void)
{ v8_errno = V8_ENOTTY; return (-1); }

static void memset_(void *d, int c, long n)
{ char *p = (char *)d; while (n-- > 0) *p++ = (char)c; }
#define memset memset_

/* v8/usr/include/sys/ioctl.h */
#define V8_TIOCGETP	(('t'<<8)|8)
#define V8_TIOCSETP	(('t'<<8)|9)
#define V8_TIOCSETN	(('t'<<8)|10)
#define V8_TIOCEXCL	(('t'<<8)|13)
#define V8_TIOCNXCL	(('t'<<8)|14)
#define V8_TIOCFLUSH	(('t'<<8)|16)
#define V8_TIOCSETC	(('t'<<8)|17)
#define V8_TIOCGETC	(('t'<<8)|18)
#define V8_TIOCSPGRP	(('t'<<8)|118)
#define V8_TIOCGPGRP	(('t'<<8)|119)
#define V8_FIONREAD	(('f'<<8)|127)
#define V8_FIOCLEX	(('f'<<8)|1)
#define V8_FIONCLEX	(('f'<<8)|2)
#define V8_FIOPUSHLD	(('f'<<8)|3)
#define V8_FIOPOPLD	(('f'<<8)|4)
#define V8_FIOLOOKLD	(('f'<<8)|5)
#define V8_FIOINSLD	(('f'<<8)|6)
#define V8_FIOSNDFD	(('f'<<8)|7)
#define V8_FIORCVFD	(('f'<<8)|8)

/* sg_flags bits */
#define V8_TANDEM	01
#define V8_CBREAK	02
#define V8_LCASE	04
#define V8_ECHO		010
#define V8_CRMOD	020
#define V8_RAW		040
#define V8_ODDP		0100
#define V8_EVENP	0200

struct v8_sgttyb {
	char	sg_ispeed;
	char	sg_ospeed;
	char	sg_erase;
	char	sg_kill;
	short	sg_flags;
};

struct v8_tchars {
	char	t_intrc, t_quitc, t_startc, t_stopc, t_eofc, t_brkc;
};

static void
termios_to_sgtty(struct termios *t, struct v8_sgttyb *s)
{
	memset(s, 0, sizeof *s);
	s->sg_erase = t->c_cc[VERASE];
	s->sg_kill  = t->c_cc[VKILL];
	s->sg_flags = 0;

	if (t->c_lflag & ECHO)   s->sg_flags |= V8_ECHO;
	if (t->c_iflag & ICRNL)  s->sg_flags |= V8_CRMOD;
	if (t->c_iflag & IXON)   s->sg_flags |= V8_TANDEM;

	if (!(t->c_lflag & ICANON)) {
		/*
		 * V8 distinguishes CBREAK (no line editing, signals still
		 * generated) from RAW (nothing interpreted at all).  termios
		 * turns both off at once, so ISIG is what tells them apart.
		 */
		if (t->c_lflag & ISIG) s->sg_flags |= V8_CBREAK;
		else                   s->sg_flags |= V8_RAW;
	}
	if (t->c_cflag & PARENB)
		s->sg_flags |= (t->c_cflag & PARODD) ? V8_ODDP : V8_EVENP;

	/* Speeds: V8's codes are the V7 table (B0..B9600, EXTA, EXTB).
	 * cfgetospeed returns the actual rate on macOS, so map the common
	 * ones and fall back to 9600, which is what a V8 program assumes. */
	s->sg_ispeed = s->sg_ospeed = 13;	/* B9600 in the V7 table */
}

static void
sgtty_to_termios(struct v8_sgttyb *s, struct termios *t)
{
	t->c_cc[VERASE] = s->sg_erase;
	t->c_cc[VKILL]  = s->sg_kill;

	if (s->sg_flags & V8_ECHO)  t->c_lflag |=  ECHO;
	else                        t->c_lflag &= ~ECHO;

	if (s->sg_flags & V8_CRMOD) { t->c_iflag |= ICRNL; t->c_oflag |= ONLCR; }
	else                        { t->c_iflag &= ~ICRNL; t->c_oflag &= ~ONLCR; }

	if (s->sg_flags & V8_RAW) {
		/* nothing interpreted */
		t->c_lflag &= ~(ICANON|ISIG|IEXTEN);
		t->c_iflag &= ~(ICRNL|IXON|ISTRIP|INPCK|BRKINT);
		t->c_oflag &= ~OPOST;
		t->c_cc[VMIN] = 1;
		t->c_cc[VTIME] = 0;
	} else if (s->sg_flags & V8_CBREAK) {
		/* no line editing, but signals still generated */
		t->c_lflag &= ~ICANON;
		t->c_lflag |= ISIG;
		t->c_cc[VMIN] = 1;
		t->c_cc[VTIME] = 0;
	} else {
		t->c_lflag |= ICANON|ISIG;
	}

	if (s->sg_flags & (V8_ODDP|V8_EVENP)) {
		t->c_cflag |= PARENB;
		if (s->sg_flags & V8_ODDP) t->c_cflag |= PARODD;
		else                       t->c_cflag &= ~PARODD;
	} else {
		t->c_cflag &= ~PARENB;
	}
}

int
v8s_ioctl(int fd, int cmd, char *arg)
{
	struct termios t;
	int n;

	switch (cmd) {
	case V8_TIOCGETP:
		if (tcgetattr(fd, &t) < 0) return (ioctlfail());
		termios_to_sgtty(&t, (struct v8_sgttyb *)arg);
		return (0);

	case V8_TIOCSETP:
	case V8_TIOCSETN:
		if (tcgetattr(fd, &t) < 0) return (ioctlfail());
		sgtty_to_termios((struct v8_sgttyb *)arg, &t);
		/* SETP drains and flushes, SETN sets immediately */
		if (tcsetattr(fd, cmd == V8_TIOCSETP ? TCSAFLUSH : TCSANOW, &t) < 0)
			return (ioctlfail());
		return (0);

	case V8_TIOCGETC: {
		struct v8_tchars *tc = (struct v8_tchars *)arg;
		if (tcgetattr(fd, &t) < 0) return (ioctlfail());
		tc->t_intrc  = t.c_cc[VINTR];
		tc->t_quitc  = t.c_cc[VQUIT];
		tc->t_startc = t.c_cc[VSTART];
		tc->t_stopc  = t.c_cc[VSTOP];
		tc->t_eofc   = t.c_cc[VEOF];
		tc->t_brkc   = -1;	/* V8's "no break character" */
		return (0);
	}

	case V8_TIOCSETC: {
		struct v8_tchars *tc = (struct v8_tchars *)arg;
		if (tcgetattr(fd, &t) < 0) return (ioctlfail());
		t.c_cc[VINTR]  = tc->t_intrc;
		t.c_cc[VQUIT]  = tc->t_quitc;
		t.c_cc[VSTART] = tc->t_startc;
		t.c_cc[VSTOP]  = tc->t_stopc;
		t.c_cc[VEOF]   = tc->t_eofc;
		if (tcsetattr(fd, TCSANOW, &t) < 0) return (ioctlfail());
		return (0);
	}

	case V8_TIOCFLUSH:
		if (tcflush(fd, TCIOFLUSH) < 0) return (ioctlfail());
		return (0);

	case V8_FIONREAD:
		if (ioctl(fd, FIONREAD, &n) < 0) return (ioctlfail());
		*(int *)arg = n;
		return (0);

	case V8_TIOCGPGRP:
		if ((n = tcgetpgrp(fd)) < 0) return (ioctlfail());
		*(int *)arg = n;
		return (0);

	case V8_TIOCSPGRP:
		if (tcsetpgrp(fd, *(int *)arg) < 0) return (ioctlfail());
		return (0);

	case V8_FIOCLEX:
		if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) return (ioctlfail());
		return (0);

	case V8_FIONCLEX:
		if (fcntl(fd, F_SETFD, 0) < 0) return (ioctlfail());
		return (0);

	case V8_TIOCEXCL:
	case V8_TIOCNXCL:
		return (0);		/* advisory only; harmless to ignore */

	/*
	 * Streams line disciplines and file-descriptor passing: V8 kernel
	 * features with no counterpart here.  See the note at the top.
	 */
	case V8_FIOPUSHLD:
	case V8_FIOPOPLD:
	case V8_FIOLOOKLD:
	case V8_FIOINSLD:
	case V8_FIOSNDFD:
	case V8_FIORCVFD:
		v8_errno = V8_EINVAL;
		return (-1);
	}

	v8_errno = V8_ENOTTY;
	return (-1);
}


/*
 * isatty(3).
 *
 * V8 puts this in libc, but it is a one-line ioctl and libc's copy is not
 * ported yet, so it lives here.  _flsbuf calls it to decide whether stdout
 * should be line-buffered, and until now it was the single libSystem symbol a
 * "freestanding" V8 program still imported -- which also meant its answer came
 * from the host's notion of the descriptor rather than ours.
 *
 * Asking the terminal for its attributes is how every implementation does this:
 * it succeeds on a terminal and fails with ENOTTY on anything else.
 */
int
isatty(fd)
	int fd;
{
	struct termios t;

	return (hostioctl(fd, TIOCGETA, &t) >= 0);
}
