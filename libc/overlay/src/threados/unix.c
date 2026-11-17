/* musl on ThreadOS 64: Unix socket pairs (kernel64/ipc64.c)
 *
 * socketpair(AF_UNIX) gives two descriptors of the kernel's own, the
 * connected ends of a pair. What goes in at one end comes out at the
 * other, and sendmsg can send descriptors along with the bytes
 * (SCM_RIGHTS): the receiver gets numbers of its own for the same
 * pipes, sockets and memfds. That is how a program built of several
 * processes talks - WebKit's window, page and network processes do
 * nothing else.
 *
 * The kernel moves one message at a time, at most IPC_MSG_MAX bytes
 * and IPC_FDS_MAX descriptors. Here that becomes what POSIX promises:
 * a stream's longer writes go in pieces and its reads gather what has
 * come; a seqpacket's message stays whole, and one longer than the
 * room says so (MSG_TRUNC).
 *
 * The descriptors are ordinary kernel numbers, so close, dup2, fcntl
 * and fork need nothing from here; syscall.c passes the calls that do
 * (read, write, readv, writev, fstat, ioctl) and socket.c the socket
 * calls and poll. Named Unix sockets - bind and connect to a path -
 * are not there yet.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include "syscall.h"
#include "kstat.h"

#define HIDDEN __attribute__((__visibility__("hidden")))

#define T_FDINFO 74
#define T_IPC   105
#define T_FDS   256                  /* FD_MAX in kernel64/task64.c */
#define K_USOCK   4

/* kernel64/ipc64.h */
#define IPC_PAIR     0
#define IPC_SEND     1
#define IPC_RECV     2
#define IPC_POLL     3
#define IPC_WAIT     4
#define IPC_SHUTDOWN 5
#define IPC_IN   1
#define IPC_OUT  2
#define IPC_HUP  4
#define IPC_DATA 8
#define IPC_DONTWAIT 1
#define IPC_PEEK     2
#define MSG_MAX  (64 * 1024)
#define FDS_MAX  16
#define WAIT_MAX 64

struct ipc_io {
	unsigned long buf;
	unsigned len, flags;
	unsigned long fds;
	unsigned nfds, truncated;
};

int __tos_fd_flags(int fd);
void __tos_fd_set_flags(int fd, int flags);

static long tos(long n, long a1, long a2, long a3, long a4)
{
	long ret;
	register long r10 __asm__("r10") = a4;
	__asm__ __volatile__ ("syscall" : "=a"(ret)
		: "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
		: "rcx", "r11", "memory");
	return ret;
}

static void touch(const void *p, size_t len)
{
	const volatile char *c = p;
	if (!len)
		return;
	for (size_t off = 0; off < len; off += 4096 - ((size_t)(c + off) & 4095))
		(void)c[off];
	(void)c[len - 1];
}

struct fdinfo { int kind; unsigned size, pos; };

/* For a socket the kernel says the bytes waiting ('size') and the
   type, SOCK_STREAM or SOCK_SEQPACKET ('pos') */
static int info(long fd, struct fdinfo *fi)
{
	if (fd < 0 || fd >= T_FDS)
		return -1;
	if (tos(T_FDINFO, fd, (long)fi, 0, 0) < 0 || fi->kind != K_USOCK)
		return -1;
	return 0;
}

HIDDEN int __tos_unix_owns(long fd)
{
	struct fdinfo fi;
	return info(fd, &fi) == 0;
}

static int seqpacket(int fd)
{
	struct fdinfo fi;
	return info(fd, &fi) == 0 && fi.pos == SOCK_SEQPACKET;
}

static int dontwait(int fd, int flags)
{
	return (flags & MSG_DONTWAIT) || (__tos_fd_flags(fd) & O_NONBLOCK)
	       ? IPC_DONTWAIT : 0;
}

HIDDEN long __tos_unix_pair(long type, long proto, int *sv)
{
	int kind = type & 0xf;
	if (kind != SOCK_STREAM && kind != SOCK_SEQPACKET)
		return kind == SOCK_DGRAM ? -EOPNOTSUPP : -EPROTONOSUPPORT;
	if (proto)
		return -EPROTONOSUPPORT;
	int pair[2];
	long r = tos(T_IPC, IPC_PAIR, kind, (long)pair, 0);
	if (r < 0)
		return r == -ENOMEM ? -ENFILE : r;
	for (int i = 0; i < 2; i++)
		__tos_fd_set_flags(pair[i], O_RDWR |
		                   (type & SOCK_NONBLOCK ? O_NONBLOCK : 0));
	touch(sv, 2 * sizeof(int));
	sv[0] = pair[0];
	sv[1] = pair[1];
	return 0;
}

/* --- Sending ------------------------------------------------------------ */

static long send_one(int fd, const void *buf, size_t len, const int *fds,
                     int nfds, int how)
{
	struct ipc_io io = { (unsigned long)buf, len, how,
	                     (unsigned long)fds, nfds, 0 };
	return tos(T_IPC, IPC_SEND, fd, (long)&io, 0);
}

/* The bytes of 'iov' with 'fds'. A seqpacket's go as one message; a
   stream's in messages of at most MSG_MAX, the descriptors with the
   first, and a stream that would wait halfway says how much went. */
static long send_iov(int fd, const struct iovec *iov, int cnt,
                     const int *fds, int nfds, int flags)
{
	size_t total = 0;
	int parts = 0;
	for (int i = 0; i < cnt; i++) {
		total += iov[i].iov_len;
		parts += iov[i].iov_len != 0;
	}
	int seq = seqpacket(fd);
	if (seq && total > MSG_MAX)
		return -EMSGSIZE;
	if (!seq && !total)
		return 0;                    /* nothing, descriptors included */
	int how = dontwait(fd, flags);
	/* one piece of memory: no copy */
	if (parts <= 1) {
		const char *p = 0;
		for (int i = 0; i < cnt && !p; i++)
			if (iov[i].iov_len)
				p = iov[i].iov_base;
		size_t done = 0;
		do {
			size_t part = total - done > MSG_MAX ? MSG_MAX : total - done;
			long r = send_one(fd, p + done, part, done ? 0 : fds,
			                  done ? 0 : nfds, how);
			if (r < 0)
				return done ? (long)done : r;
			done += r;
		} while (done < total);
		return done;
	}
	size_t room = total > MSG_MAX ? MSG_MAX : total;
	char *buf = malloc(room);
	if (!buf)
		return -ENOBUFS;
	size_t done = 0;
	int at = 0;
	size_t in = 0;                   /* where in iov[at] */
	while (done < total) {
		size_t fill = 0;
		while (fill < room && at < cnt) {
			size_t k = iov[at].iov_len - in;
			if (k > room - fill)
				k = room - fill;
			memcpy(buf + fill, (const char *)iov[at].iov_base + in, k);
			fill += k;
			in += k;
			if (in == iov[at].iov_len) {
				at++;
				in = 0;
			}
		}
		long r = send_one(fd, buf, fill, done ? 0 : fds, done ? 0 : nfds, how);
		if (r < 0) {
			free(buf);
			return done ? (long)done : r;
		}
		done += r;
		if ((size_t)r < fill)
			break;               /* (the kernel takes a message whole) */
	}
	free(buf);
	return done;
}

/* The descriptors a message's SCM_RIGHTS carry */
static int rights(const struct msghdr *m, int *fds)
{
	int n = 0;
	if (!m->msg_control || m->msg_controllen < sizeof(struct cmsghdr))
		return 0;
	touch(m->msg_control, m->msg_controllen);
	for (struct cmsghdr *c = CMSG_FIRSTHDR(m); c;
	     c = CMSG_NXTHDR((struct msghdr *)m, c)) {
		if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
			continue;            /* SCM_CREDENTIALS: the kernel knows */
		if (c->cmsg_len < CMSG_LEN(0))
			return -EINVAL;
		int k = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
		if (n + k > FDS_MAX)
			return -ETOOMANYREFS;
		memcpy(fds + n, CMSG_DATA(c), k * sizeof(int));
		n += k;
	}
	for (int i = 0; i < n; i++)
		if (fds[i] < 0 || fds[i] >= T_FDS)
			return -EBADF;       /* a socket of socket.c's own */
	return n;
}

/* --- Receiving ---------------------------------------------------------- */

struct got {
	int fds[FDS_MAX];
	int nfds, lost;
	size_t whole;                    /* a seqpacket message's length */
};

static long recv_one(int fd, void *buf, size_t len, struct got *g,
                     int room, int how)
{
	struct ipc_io io = { (unsigned long)buf, len > MSG_MAX ? MSG_MAX : len,
	                     how, (unsigned long)(g->fds + g->nfds),
	                     room - g->nfds, 0 };
	long r = tos(T_IPC, IPC_RECV, fd, (long)&io, 0);
	if (r >= 0) {
		g->nfds += io.nfds;
		g->lost += io.truncated;
		g->whole = io.len;
	}
	return r;
}

/* Into one piece of memory. A stream waits only for its first bytes
   (or for all of them, MSG_WAITALL) and then takes whatever else has
   come, as a read from a socket does; a seqpacket takes one message. */
static long recv_buf(int fd, char *buf, size_t len, struct got *g, int room,
                     int flags, int seq)
{
	int how = dontwait(fd, flags) | (flags & MSG_PEEK ? IPC_PEEK : 0);
	long r = recv_one(fd, buf, len, g, room, how);
	if (r <= 0 || seq || (flags & MSG_PEEK))
		return r;
	size_t done = r;
	/* (descriptors end a read, as on Linux: they belong with the
	   bytes they came with) */
	while (done < len && !g->nfds && !g->lost) {
		int wait = (flags & MSG_WAITALL) && !(how & IPC_DONTWAIT);
		r = recv_one(fd, buf + done, len - done, g, room,
		             wait ? 0 : IPC_DONTWAIT);
		if (r <= 0)
			break;               /* nothing more yet, or the end */
		done += r;
	}
	return done;
}

static long recv_iov(int fd, const struct iovec *iov, int cnt, struct got *g,
                     int room, int flags)
{
	int seq = seqpacket(fd);
	size_t total = 0;
	int parts = 0;
	for (int i = 0; i < cnt; i++) {
		total += iov[i].iov_len;
		parts += iov[i].iov_len != 0;
	}
	if (parts <= 1) {
		char *p = 0;
		for (int i = 0; i < cnt && !p; i++)
			if (iov[i].iov_len)
				p = iov[i].iov_base;
		if (!p) {
			/* no room at all still takes a seqpacket message */
			char none;
			return seq ? recv_buf(fd, &none, 0, g, room, flags, 1) : 0;
		}
		return recv_buf(fd, p, total, g, room, flags, seq);
	}
	if (!seq) {
		/* a stream fills one piece after another */
		long done = 0;
		for (int i = 0; i < cnt; i++) {
			if (!iov[i].iov_len)
				continue;
			long r = recv_buf(fd, iov[i].iov_base, iov[i].iov_len, g,
			                  room, done ? flags | MSG_DONTWAIT : flags, 0);
			if (r < 0)
				return done ? done : r;
			done += r;
			if ((size_t)r < iov[i].iov_len)
				break;
		}
		return done;
	}
	/* a seqpacket's message lands whole, then is spread out */
	size_t size = total > MSG_MAX ? MSG_MAX : total;
	char *buf = malloc(size);
	if (!buf)
		return -ENOBUFS;
	long r = recv_buf(fd, buf, size, g, room, flags, 1);
	size_t at = 0;
	for (int i = 0; i < cnt && r > 0 && at < (size_t)r; i++) {
		size_t k = iov[i].iov_len < r - at ? iov[i].iov_len : r - at;
		memcpy(iov[i].iov_base, buf + at, k);
		at += k;
	}
	free(buf);
	return r;
}

static long do_recvmsg(int fd, struct msghdr *m, int flags)
{
	touch(m, sizeof(*m));
	int room = 0;
	if (m->msg_control && m->msg_controllen >= CMSG_LEN(sizeof(int))) {
		room = (m->msg_controllen - CMSG_LEN(0)) / sizeof(int);
		if (room > FDS_MAX)
			room = FDS_MAX;
	}
	struct got g = { .nfds = 0 };
	long r = recv_iov(fd, m->msg_iov, m->msg_iovlen, &g, room, flags);
	if (r < 0)
		return r;
	m->msg_flags = 0;
	if (m->msg_name)
		m->msg_namelen = 0;          /* the ends of a pair have no names */
	if (g.nfds) {
		touch(m->msg_control, m->msg_controllen);
		struct cmsghdr *c = CMSG_FIRSTHDR(m);
		c->cmsg_level = SOL_SOCKET;
		c->cmsg_type = SCM_RIGHTS;
		c->cmsg_len = CMSG_LEN(g.nfds * sizeof(int));
		memcpy(CMSG_DATA(c), g.fds, g.nfds * sizeof(int));
		size_t used = CMSG_SPACE(g.nfds * sizeof(int));
		if (used < m->msg_controllen)
			m->msg_controllen = used;
		for (int i = 0; i < g.nfds; i++)
			__tos_fd_set_flags(g.fds[i], O_RDWR);
	} else {
		m->msg_controllen = 0;
	}
	if (g.lost)
		m->msg_flags |= MSG_CTRUNC;  /* (the kernel closed those) */
	if (seqpacket(fd) && g.whole > (size_t)r) {
		m->msg_flags |= MSG_TRUNC;
		if (flags & MSG_TRUNC)
			return g.whole;      /* the real length, as Linux says */
	}
	return r;
}

/* --- Readiness ---------------------------------------------------------- */

/* As Linux: an end whose peer has gone is readable (the end of the
   data) and writable too - the write then fails at once with EPIPE
   rather than waiting for ever. */
HIDDEN short __tos_unix_events(int fd, short want)
{
	long bits = tos(T_IPC, IPC_POLL, fd, 0, 0);
	if (bits < 0)
		return POLLNVAL;
	short got = 0;
	if (bits & IPC_IN)
		got |= POLLIN | POLLRDNORM;
	if (bits & (IPC_OUT | IPC_HUP))
		got |= POLLOUT | POLLWRNORM;
	if (bits & IPC_HUP)
		got |= POLLHUP;
	return got & (want | POLLHUP);
}

/* A pipe's end, as Linux tells it: the read end is readable while
   bytes wait and hung up once no writer is left; the write end is
   writable while there is room, and in error (and writable, so the
   write fails at once) once no reader is left */
HIDDEN short __tos_pipe_events(int fd, short want, int writer)
{
	long bits = tos(T_IPC, IPC_POLL, fd, 0, 0);
	if (bits < 0)
		return POLLNVAL;
	short got = 0;
	if (writer) {
		if (bits & (IPC_OUT | IPC_HUP))
			got |= POLLOUT | POLLWRNORM;
		if (bits & IPC_HUP)
			got |= POLLERR;
	} else {
		if (bits & IPC_DATA)
			got |= POLLIN | POLLRDNORM;
		if (bits & IPC_HUP)
			got |= POLLHUP;
	}
	return got & (want | POLLHUP | POLLERR);
}

/* An entry of __tos_unix_wait's list: the descriptor and what poll
   waits for on it */
HIDDEN int __tos_unix_want(int fd, short events)
{
	int want = (events & (POLLIN | POLLRDNORM) ? IPC_IN : 0) |
	           (events & (POLLOUT | POLLWRNORM) ? IPC_OUT : 0);
	return fd | want << 16;
}

/* Sleeps until one of these ends is ready for what is wanted of it or
   loses its peer, or until 'ms' (-1: no limit) is over, or something
   changes at their peers. Then poll looks again. */
HIDDEN void __tos_unix_wait(const int *fds, int n, long ms)
{
	if (n > WAIT_MAX)
		n = WAIT_MAX;
	tos(T_IPC, IPC_WAIT, (long)fds, n, ms);
}

/* --- The calls ---------------------------------------------------------- */

static long sockname(void *addr, socklen_t *len)
{
	touch(len, sizeof(*len));
	sa_family_t fam = AF_UNIX;
	if (*len) {
		touch(addr, *len);
		memcpy(addr, &fam, *len < sizeof(fam) ? *len : sizeof(fam));
	}
	*len = sizeof(fam);
	return 0;
}

static long sock_getopt(int fd, int level, int opt, void *val, socklen_t *len)
{
	int v;
	if (level != SOL_SOCKET)
		return -ENOPROTOOPT;
	switch (opt) {
	case SO_TYPE:     v = seqpacket(fd) ? SOCK_SEQPACKET : SOCK_STREAM; break;
	case SO_DOMAIN:   v = AF_UNIX; break;
	case SO_PROTOCOL: v = 0; break;
	case SO_ERROR:    v = 0; break;
	case SO_ACCEPTCONN: v = 0; break;
	case SO_SNDBUF:
	case SO_RCVBUF:   v = 256 * 1024; break;   /* QUEUE_MAX, ipc64.c */
	default:          return -ENOPROTOOPT;
	}
	touch(len, sizeof(*len));
	socklen_t k = *len < sizeof(v) ? *len : sizeof(v);
	if (k) {
		touch(val, k);
		memcpy(val, &v, k);
	}
	*len = k;
	return 0;
}

HIDDEN long __tos_unix(long n, long fd, long b, long c, long d, long e, long f)
{
	(void)f;
	switch (n) {
	case SYS_write:
	case SYS_sendto: {
		if (n == SYS_sendto && e && f && !seqpacket(fd))
			return -EISCONN;     /* a pair is connected already */
		struct iovec v = { (void *)b, c };
		return send_iov(fd, &v, 1, 0, 0, n == SYS_write ? 0 : d);
	}
	case SYS_writev:
		return send_iov(fd, (const struct iovec *)b, c, 0, 0, 0);
	case SYS_sendmsg: {
		const struct msghdr *m = (const void *)b;
		int fds[FDS_MAX];
		touch(m, sizeof(*m));
		if (m->msg_name && m->msg_namelen && !seqpacket(fd))
			return -EISCONN;
		int k = rights(m, fds);
		if (k < 0)
			return k;
		return send_iov(fd, m->msg_iov, m->msg_iovlen, fds, k, c);
	}
	case SYS_read:
	case SYS_recvfrom: {
		struct got g = { .nfds = 0 };
		int flags = n == SYS_read ? 0 : d;
		struct iovec v = { (void *)b, c };
		long r = recv_iov(fd, &v, 1, &g, 0, flags);
		if (r >= 0 && n == SYS_recvfrom && e && f) {
			touch((void *)f, sizeof(socklen_t));
			*(socklen_t *)f = 0;
		}
		if (r >= 0 && (flags & MSG_TRUNC) && seqpacket(fd) && g.whole > (size_t)r)
			return g.whole;
		return r;
	}
	case SYS_readv: {
		struct got g = { .nfds = 0 };
		return recv_iov(fd, (const struct iovec *)b, c, &g, 0, 0);
	}
	case SYS_recvmsg:
		return do_recvmsg(fd, (struct msghdr *)b, c);
	case SYS_shutdown:
		if (b < SHUT_RD || b > SHUT_RDWR)
			return -EINVAL;
		return tos(T_IPC, IPC_SHUTDOWN, fd, b, 0);
	case SYS_getsockname:
	case SYS_getpeername:
		return sockname((void *)b, (socklen_t *)c);
	case SYS_setsockopt:
		return 0;                    /* SO_PASSCRED, buffer sizes: all fine */
	case SYS_getsockopt:
		return sock_getopt(fd, b, c, (void *)d, (socklen_t *)e);
	case SYS_connect:
		return -EISCONN;
	case SYS_bind:
	case SYS_listen:
	case SYS_accept:
	case SYS_accept4:
		return -EINVAL;
	case SYS_fstat: {
		struct kstat *st = (void *)b;
		touch(st, sizeof(*st));
		memset(st, 0, sizeof(*st));
		st->st_mode = S_IFSOCK | 0777;
		st->st_nlink = 1;
		st->st_ino = 3000 + fd;
		st->st_blksize = 4096;
		return 0;
	}
	case SYS_ioctl: {
		struct fdinfo fi;
		if (b == FIONREAD) {
			touch((void *)c, sizeof(int));
			*(int *)c = info(fd, &fi) == 0 ? (int)fi.size : 0;
			return 0;
		}
		if (b == FIONBIO) {
			touch((void *)c, sizeof(int));
			int fl = __tos_fd_flags(fd) & ~O_NONBLOCK;
			__tos_fd_set_flags(fd, fl | (*(int *)c ? O_NONBLOCK : 0));
			return 0;
		}
		return -ENOTTY;
	}
	}
	return -EINVAL;
}
