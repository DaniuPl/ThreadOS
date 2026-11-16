/* musl on ThreadOS 64: BSD sockets over the kernel's TCP and UDP
 *
 * The kernel has connections and UDP sockets of its own, numbered apart
 * from file descriptors and spoken to by calls of their own
 * (kernel64/syscall64.h). A C program wants them to be descriptors:
 * socket() gives a number, read() and write() and close() work on it,
 * and poll() waits on it beside files. So the numbers are made here.
 * A socket is a descriptor from SOCK_BASE up - above every number the
 * kernel hands out - and every call that takes a descriptor asks here
 * first (__tos_net_owns).
 *
 * IPv4 only, as the kernel is. A blocking call waits in short steps,
 * so a signal is seen between them. poll() and select() look at every
 * descriptor in turn and sleep a tick between rounds: a file or a pipe
 * counts as always ready, a socket as the kernel says (SYS64_TCP_POLL,
 * SYS64_UDP_POLL).
 *
 * Name lookups are musl's own resolver, which reads /etc/resolv.conf.
 * A ThreadOS disk has none, so when there is no such file a read-only
 * one is made up here from the name server the network was given.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include "syscall.h"
#include "kstat.h"

#define NET_HIDDEN __attribute__((__visibility__("hidden")))

#define T_SLEEP      4
#define T_TICKS      5
#define T_FDINFO    74
#define T_NETINFO   23
#define T_UDP_OPEN  24
#define T_UDP_SEND  25
#define T_UDP_RECV  26
#define T_UDP_CLOSE 27
#define T_TCP_SEND  29
#define T_TCP_CLOSE 31
#define T_TCP_LISTEN 90
#define T_TCP_ACCEPT 91
#define T_TCP_POLL   92
#define T_TCP_START  93
#define T_TCP_RECVT  94
#define T_TCP_ADDR   95
#define T_UDP_POLL   96
#define T_UDP_PORT   97
#define T_GETRANDOM  98
#define T_READ      12
#define T_OPEN      13
#define T_CLOSE     14
#define T_INITRD_READ 76

/* kernel/include/tcp.h */
#define P_IN        0x01
#define P_OUT       0x02
#define P_EOF       0x04
#define P_ERROR     0x08
#define P_CONNECTED 0x10
#define P_ACCEPT    0x20

#define T_HZ      100
#define T_FDS     32                 /* the kernel's descriptors */
#define CHUNK     1400               /* the most one TCP or UDP call moves */
#define SOCK_BASE 64
#define SOCKS     32
#define VFILE_BASE (SOCK_BASE + SOCKS)
#define VFILES    8

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

/* kernel64/syscall64.c's description of an address */
struct t_udp { int sock; unsigned ip; unsigned short port, pad; unsigned timeout; };

enum { S_NEW, S_CONNECTING, S_CONNECTED, S_LISTENING, S_FAILED };

static struct sk {
	unsigned char used, stream, state, nonblock;
	int kid;                         /* the kernel's number, -1 = none yet */
	int err;                         /* what SO_ERROR will say */
	unsigned short lport;            /* bound, host order */
	unsigned rip;                    /* the peer, host order */
	unsigned short rport;
} socks[SOCKS];

static volatile int sock_lock;

static void lock(void)
{
	while (__atomic_exchange_n(&sock_lock, 1, __ATOMIC_ACQUIRE))
		tos(T_SLEEP, 0, 0, 0, 0);
}

static void unlock(void)
{
	__atomic_store_n(&sock_lock, 0, __ATOMIC_RELEASE);
}

static struct sk *sock_of(long fd)
{
	if (fd < SOCK_BASE || fd >= SOCK_BASE + SOCKS)
		return 0;
	struct sk *s = &socks[fd - SOCK_BASE];
	return s->used ? s : 0;
}

/* --- Made-up files: /etc/resolv.conf, /dev/urandom, the CA bundle -- */

/* random: /dev/urandom or /dev/random, whose every read is the
   kernel's generator (kernel/random.c). Libraries that do not know
   musl has getrandom() - Mbed TLS asks for it only from glibc - open
   the file instead.
   big: a file too large for data[], on the heap - the CA bundle. */
static struct vf {
	int used, random;
	char data[64];
	char *big;
	int len, pos;
} vfiles[VFILES];

static struct vf *vfile_of(long fd)
{
	if (fd < VFILE_BASE || fd >= VFILE_BASE + VFILES)
		return 0;
	struct vf *v = &vfiles[fd - VFILE_BASE];
	return v->used ? v : 0;
}

static int put_ip(char *out, unsigned ip)
{
	int n = 0;
	for (int i = 3; i >= 0; i--) {
		unsigned b = (ip >> (i * 8)) & 255;
		if (b >= 100)
			out[n++] = '0' + b / 100;
		if (b >= 10)
			out[n++] = '0' + b / 10 % 10;
		out[n++] = '0' + b % 10;
		if (i)
			out[n++] = '.';
	}
	return n;
}

static long vfile_new(int random)
{
	lock();
	for (int i = 0; i < VFILES; i++) {
		if (vfiles[i].used)
			continue;
		memset(&vfiles[i], 0, sizeof(vfiles[i]));
		vfiles[i].used = 1;
		vfiles[i].random = random;
		unlock();
		return VFILE_BASE + i;
	}
	unlock();
	return -EMFILE;
}

/* --- The certificates TLS checks servers against --------------------
 *
 * Mozilla's roots, as curl.se publishes them (third_party/cacert/), are
 * in the initrd as cacert.pem, so every system has them - a live one
 * with no disk too. Programs look for them where Linux keeps them; curl
 * is built to (--with-ca-bundle) and so is much else. Roots of one's
 * own - a local network's CA - go into roots.der at the top of the
 * disk, as for the system's older TLS (user/ucert.h); they are added to
 * the bundle here, in PEM, so both kinds of program trust the same
 * roots.
 */
#define BUNDLE_MAX (1 << 20)
#define ROOTS_MAX  65536

static int is_bundle(const char *path)
{
	return !strcmp(path, "/etc/ssl/certs/ca-certificates.crt") ||
	       !strcmp(path, "/etc/ssl/cert.pem") ||
	       !strcmp(path, "/etc/pki/tls/certs/ca-bundle.crt");
}

static const char b64[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* One DER certificate as a PEM block, 64 characters to a line. The
   length written, or 0 when it does not fit. */
static long pem_block(const unsigned char *der, long n, char *out, long room)
{
	static const char head[] = "-----BEGIN CERTIFICATE-----\n";
	static const char tail[] = "-----END CERTIFICATE-----\n";
	long need = sizeof(head) - 1 + (n + 2) / 3 * 4 + (n + 47) / 48 +
	            sizeof(tail) - 1;
	if (need > room)
		return 0;
	long at = 0, col = 0;
	memcpy(out, head, sizeof(head) - 1);
	at += sizeof(head) - 1;
	for (long i = 0; i < n; i += 3) {
		unsigned v = (unsigned)der[i] << 16;
		if (i + 1 < n) v |= (unsigned)der[i + 1] << 8;
		if (i + 2 < n) v |= der[i + 2];
		out[at++] = b64[v >> 18 & 63];
		out[at++] = b64[v >> 12 & 63];
		out[at++] = i + 1 < n ? b64[v >> 6 & 63] : '=';
		out[at++] = i + 2 < n ? b64[v & 63] : '=';
		if ((col += 4) == 64 || i + 3 >= n) {
			out[at++] = '\n';
			col = 0;
		}
	}
	memcpy(out + at, tail, sizeof(tail) - 1);
	return at + sizeof(tail) - 1;
}

/* The length of the DER element at p (tag, length, contents), or 0 */
static long der_len(const unsigned char *p, long room)
{
	if (room < 2 || p[0] != 0x30)
		return 0;
	if (p[1] < 0x80)
		return 2 + p[1] <= room ? 2 + p[1] : 0;
	int k = p[1] & 0x7F;
	if (k < 1 || k > 3 || room < 2 + k)
		return 0;
	long n = 0;
	for (int i = 0; i < k; i++)
		n = n << 8 | p[2 + i];
	return 2 + k + n <= room ? 2 + k + n : 0;
}

static long open_bundle(void)
{
	char *buf = malloc(BUNDLE_MAX);
	if (!buf)
		return -ENOMEM;
	static const char name[] = "cacert.pem";
	touch(buf, BUNDLE_MAX);
	long n = tos(T_INITRD_READ, (long)name, sizeof(name) - 1, (long)buf,
	             BUNDLE_MAX);
	if (n < 0)
		n = 0;
	/* The disk's own roots, if any, each as one more PEM block */
	long fd = tos(T_OPEN, (long)"/roots.der", 10, 0, 0);
	unsigned char *der = fd >= 0 ? malloc(ROOTS_MAX) : 0;
	if (der) {
		long got = 0, r;
		if (n && buf[n - 1] != '\n' && n < BUNDLE_MAX)
			buf[n++] = '\n';        /* a block of ours starts a line */
		while (got < ROOTS_MAX &&
		       (r = tos(T_READ, fd, (long)(der + got),
		                ROOTS_MAX - got, 0)) > 0)
			got += r;
		for (long at = 0, len; at < got &&
		     (len = der_len(der + at, got - at)) > 0; at += len) {
			long w = pem_block(der + at, len, buf + n, BUNDLE_MAX - n);
			if (!w)
				break;
			n += w;
		}
		free(der);
	}
	if (fd >= 0)
		tos(T_CLOSE, fd, 0, 0, 0);
	if (!n) {
		free(buf);
		return -ENOENT;
	}
	long vfd = vfile_new(0);
	if (vfd < 0) {
		free(buf);
		return vfd;
	}
	struct vf *v = &vfiles[vfd - VFILE_BASE];
	v->big = buf;
	v->len = (int)n;
	return vfd;
}

NET_HIDDEN long __tos_open_virtual(const char *path)
{
	if (!strcmp(path, "/dev/urandom") || !strcmp(path, "/dev/random"))
		return vfile_new(1);
	if (is_bundle(path))
		return open_bundle();
	if (strcmp(path, "/etc/resolv.conf"))
		return -ENOENT;
	unsigned info[4];
	touch(info, sizeof(info));
	if (tos(T_NETINFO, (long)info, 0, 0, 0) <= 0 || !info[3])
		return -ENOENT;
	long fd = vfile_new(0);
	if (fd < 0)
		return fd;
	struct vf *v = &vfiles[fd - VFILE_BASE];
	memcpy(v->data, "nameserver ", 11);
	v->len = 11 + put_ip(v->data + 11, info[3]);
	v->data[v->len++] = '\n';
	return fd;
}

/* --- Sockets -------------------------------------------------------- */

static long sock_new(int stream, int nonblock)
{
	lock();
	for (int i = 0; i < SOCKS; i++) {
		if (socks[i].used)
			continue;
		memset(&socks[i], 0, sizeof(socks[i]));
		socks[i].used = 1;
		socks[i].stream = stream;
		socks[i].nonblock = nonblock;
		socks[i].kid = -1;
		unlock();
		return SOCK_BASE + i;
	}
	unlock();
	return -EMFILE;
}

static int addr_in(const void *addr, socklen_t len, unsigned *ip,
                   unsigned short *port)
{
	if (!addr || len < sizeof(struct sockaddr_in))
		return -EINVAL;
	touch(addr, sizeof(struct sockaddr_in));
	const struct sockaddr_in *sin = addr;
	if (sin->sin_family != AF_INET)
		return -EAFNOSUPPORT;
	*ip = ntohl(sin->sin_addr.s_addr);
	*port = ntohs(sin->sin_port);
	return 0;
}

static void addr_out(void *addr, socklen_t *len, unsigned ip, unsigned short port)
{
	if (!addr || !len)
		return;
	touch(len, sizeof(*len));
	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(ip);
	sin.sin_port = htons(port);
	socklen_t n = *len < sizeof(sin) ? *len : sizeof(sin);
	touch(addr, n);
	memcpy(addr, &sin, n);
	*len = sizeof(sin);
}

static unsigned own_ip(void)
{
	unsigned info[4];
	touch(info, sizeof(info));
	return tos(T_NETINFO, (long)info, 0, 0, 0) > 0 ? info[0] : 0;
}

/* A UDP socket gets its kernel side at bind, or at the first send */
static int udp_ready(struct sk *s, unsigned short port)
{
	if (s->kid >= 0)
		return 0;
	long k = tos(T_UDP_OPEN, port, 0, 0, 0);
	if (k < 0)
		return port ? -EADDRINUSE : -ENOBUFS;
	s->kid = k;
	s->lport = tos(T_UDP_PORT, k, 0, 0, 0);
	return 0;
}

/* Where a connecting stream stands; 1 = connected, 0 = not yet, or an
   error */
static int connect_progress(struct sk *s)
{
	if (s->state == S_CONNECTED)
		return 1;
	if (s->state == S_FAILED)
		return -s->err;
	long bits = tos(T_TCP_POLL, s->kid, 0, 0, 0);
	if (bits >= 0 && (bits & P_CONNECTED)) {
		s->state = S_CONNECTED;
		return 1;
	}
	if (bits < 0 || (bits & (P_ERROR | P_EOF))) {
		/* (a refused SYN is a reset; a kernel that gave up retrying
		   has freed the connection, which is a timeout) */
		s->state = S_FAILED;
		s->err = bits < 0 ? ETIMEDOUT : ECONNREFUSED;
		return -s->err;
	}
	return 0;
}

static long do_connect(struct sk *s, const void *addr, socklen_t len)
{
	unsigned ip;
	unsigned short port;
	int e = addr_in(addr, len, &ip, &port);
	if (e)
		return e;
	if (!s->stream) {
		/* a default peer, nothing more */
		s->rip = ip;
		s->rport = port;
		s->state = S_CONNECTED;
		return udp_ready(s, 0);
	}
	if (s->state == S_CONNECTED)
		return -EISCONN;
	if (s->state == S_CONNECTING) {
		int r = connect_progress(s);
		return r > 0 ? -EISCONN : r < 0 ? r : -EALREADY;
	}
	if (s->state != S_NEW)
		return -EINVAL;
	struct t_udp info = { -1, ip, port, 0, 0 };
	long k = tos(T_TCP_START, (long)&info, 0, 0, 0);
	if (k < 0)
		return -ENETUNREACH;
	s->kid = k;
	s->rip = ip;
	s->rport = port;
	s->state = S_CONNECTING;
	if (s->nonblock)
		return -EINPROGRESS;
	for (unsigned waited = 0;; waited += 10) {
		int r = connect_progress(s);
		if (r)
			return r > 0 ? 0 : r;
		if (waited >= 75000) {         /* Linux gives up about then too */
			s->state = S_FAILED;
			return -(s->err = ETIMEDOUT);
		}
		tos(T_SLEEP, 10, 0, 0, 0);
	}
}

static long do_bind(struct sk *s, const void *addr, socklen_t len)
{
	unsigned ip;
	unsigned short port;
	int e = addr_in(addr, len, &ip, &port);
	if (e)
		return e;
	if (s->stream) {
		s->lport = port;
		return 0;
	}
	if (s->kid >= 0)
		return -EINVAL;
	return udp_ready(s, port);
}

static long do_listen(struct sk *s)
{
	if (!s->stream)
		return -EOPNOTSUPP;
	if (s->state == S_LISTENING)
		return 0;
	if (s->state != S_NEW || !s->lport)
		return -EINVAL;
	long k = tos(T_TCP_LISTEN, s->lport, 0, 0, 0);
	if (k < 0)
		return -EADDRINUSE;
	s->kid = k;
	s->state = S_LISTENING;
	return 0;
}

static long do_accept(struct sk *s, void *addr, socklen_t *len, int flags)
{
	if (!s->stream || s->state != S_LISTENING)
		return -EINVAL;
	long k;
	for (;;) {
		k = tos(T_TCP_ACCEPT, s->kid, s->nonblock ? 0 : 200, 0, 0);
		if (k >= 0)
			break;
		if (s->nonblock)
			return -EAGAIN;
	}
	long fd = sock_new(1, (flags & SOCK_NONBLOCK) != 0);
	if (fd < 0) {
		tos(T_TCP_CLOSE, k, 0, 0, 0);
		return fd;
	}
	struct sk *c = sock_of(fd);
	struct { unsigned ip; unsigned short rport, lport; } ai;
	touch(&ai, sizeof(ai));
	tos(T_TCP_ADDR, k, (long)&ai, 0, 0);
	c->kid = k;
	c->state = S_CONNECTED;
	c->rip = ai.ip;
	c->rport = ai.rport;
	c->lport = ai.lport;
	addr_out(addr, len, c->rip, c->rport);
	return fd;
}

static long stream_send(struct sk *s, const char *buf, size_t len)
{
	if (s->state == S_CONNECTING)
		connect_progress(s);
	if (s->state != S_CONNECTED)
		return s->state == S_FAILED ? -s->err : -ENOTCONN;
	touch(buf, len);
	size_t done = 0;
	while (done < len) {
		size_t part = len - done > CHUNK ? CHUNK : len - done;
		long r = tos(T_TCP_SEND, s->kid, (long)buf + done, part, 0);
		if (r <= 0) {
			if (done)
				break;
			long bits = tos(T_TCP_POLL, s->kid, 0, 0, 0);
			return bits < 0 || (bits & P_ERROR) ? -ECONNRESET : -EPIPE;
		}
		done += r;
	}
	return done;
}

static long stream_recv(struct sk *s, char *buf, size_t len, int flags)
{
	if (flags & MSG_PEEK)
		return -EOPNOTSUPP;
	if (s->state == S_CONNECTING)
		connect_progress(s);
	if (s->state != S_CONNECTED)
		return s->state == S_FAILED ? -s->err : -ENOTCONN;
	if (!len)
		return 0;
	if (len > CHUNK)
		len = CHUNK;
	touch(buf, len);
	int wait = !s->nonblock && !(flags & MSG_DONTWAIT);
	for (;;) {
		long r = tos(T_TCP_RECVT, s->kid, (long)buf, len, wait ? 200 : 0);
		if (r >= 0)
			return r;                /* data, or 0: the other side finished */
		long bits = tos(T_TCP_POLL, s->kid, 0, 0, 0);
		if (bits < 0 || (bits & P_ERROR))
			return -ECONNRESET;
		if (bits & P_EOF)
			return 0;
		if (!wait)
			return -EAGAIN;
	}
}

static long dgram_send(struct sk *s, const void *buf, size_t len,
                       const void *addr, socklen_t alen)
{
	unsigned ip = s->rip;
	unsigned short port = s->rport;
	if (addr) {
		int e = addr_in(addr, alen, &ip, &port);
		if (e)
			return e;
	} else if (s->state != S_CONNECTED) {
		return -EDESTADDRREQ;
	}
	if (len > CHUNK)
		return -EMSGSIZE;
	int e = udp_ready(s, 0);
	if (e)
		return e;
	touch(buf, len);
	struct t_udp info = { s->kid, ip, port, 0, 0 };
	long r = tos(T_UDP_SEND, (long)&info, (long)buf, len, 0);
	return r < 0 ? -EHOSTUNREACH : (long)len;
}

static long dgram_recv(struct sk *s, void *buf, size_t len, int flags,
                       void *addr, socklen_t *alen)
{
	if (flags & MSG_PEEK)
		return -EOPNOTSUPP;
	int e = udp_ready(s, 0);
	if (e)
		return e;
	if (len > CHUNK)
		len = CHUNK;
	touch(buf, len ? len : 1);
	int wait = !s->nonblock && !(flags & MSG_DONTWAIT);
	for (;;) {
		struct t_udp info = { s->kid, 0, 0, 0, wait ? 200 : 0 };
		long r = tos(T_UDP_RECV, (long)&info, (long)buf, len, 0);
		if (r >= 0) {
			addr_out(addr, alen, info.ip, info.port);
			return r;
		}
		if (!wait)
			return -EAGAIN;
	}
}

static long do_close(struct sk *s)
{
	if (s->kid >= 0)
		tos(s->stream ? T_TCP_CLOSE : T_UDP_CLOSE, s->kid, 0, 0, 0);
	lock();
	s->used = 0;
	unlock();
	return 0;
}

static long do_getsockopt(struct sk *s, int level, int name, void *val,
                          socklen_t *len)
{
	int v = 0;
	if (level == SOL_SOCKET && name == SO_ERROR) {
		if (s->stream && s->state == S_CONNECTING)
			connect_progress(s);
		v = s->state == S_FAILED ? s->err : 0;
		s->err = 0;
	} else if (level == SOL_SOCKET && name == SO_TYPE) {
		v = s->stream ? SOCK_STREAM : SOCK_DGRAM;
	} else if (level == SOL_SOCKET &&
	           (name == SO_RCVBUF || name == SO_SNDBUF)) {
		v = 65536;
	}
	if (!val || !len)
		return -EFAULT;
	touch(len, sizeof(*len));
	touch(val, sizeof(int));
	if (*len < sizeof(int))
		return -EINVAL;
	memcpy(val, &v, sizeof(int));
	*len = sizeof(int);
	return 0;
}

/* What poll() reports for one socket */
static short sock_events(struct sk *s, short want)
{
	short got = 0;
	if (!s->stream) {
		if (s->kid >= 0 && tos(T_UDP_POLL, s->kid, 0, 0, 0) > 0)
			got |= POLLIN;
		got |= POLLOUT;
		return got & (want | POLLERR | POLLHUP);
	}
	switch (s->state) {
	case S_NEW:
		return POLLOUT & want;
	case S_FAILED:
		return POLLERR | (POLLOUT & want);
	case S_CONNECTING: {
		int r = connect_progress(s);
		if (!r)
			return 0;
		if (r < 0)
			return POLLERR | (POLLOUT & want);
		break;
	}
	case S_LISTENING: {
		long bits = tos(T_TCP_POLL, s->kid, 0, 0, 0);
		return bits > 0 && (bits & P_ACCEPT) ? POLLIN & want : 0;
	}
	}
	long bits = tos(T_TCP_POLL, s->kid, 0, 0, 0);
	if (bits < 0 || (bits & P_ERROR))
		return POLLERR | POLLHUP | (POLLIN & want);
	if (bits & (P_IN | P_EOF))
		got |= POLLIN;
	if (bits & P_EOF)
		got |= POLLHUP;
	if (bits & P_OUT)
		got |= POLLOUT;
	return got & (want | POLLERR | POLLHUP);
}

static short fd_events(int fd, short want)
{
	if (fd < 0)
		return 0;
	struct sk *s = sock_of(fd);
	if (s)
		return sock_events(s, want);
	if (vfile_of(fd))
		return want & (POLLIN | POLLOUT);
	if (fd >= T_FDS)
		return POLLNVAL;
	struct { int kind; unsigned size, pos; } fi;
	touch(&fi, sizeof(fi));
	if (tos(T_FDINFO, fd, (long)&fi, 0, 0) < 0 || (fi.kind == 0 && fd > 2))
		return POLLNVAL;
	/* a file, a pipe, the console: a read or a write would not wait
	   long, which is as much as the kernel lets anyone tell */
	return want & (POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM);
}

static long do_poll(struct pollfd *fds, unsigned long n, long ms)
{
	if (n)
		touch(fds, n * sizeof(*fds));
	unsigned long start = tos(T_TICKS, 0, 0, 0, 0);
	for (;;) {
		long ready = 0;
		for (unsigned long i = 0; i < n; i++) {
			fds[i].revents = fd_events(fds[i].fd, fds[i].events);
			if (fds[i].revents)
				ready++;
		}
		if (ready || !ms)
			return ready;
		if (ms > 0 &&
		    (tos(T_TICKS, 0, 0, 0, 0) - start) * (1000 / T_HZ) >= (unsigned long)ms)
			return 0;
		tos(T_SLEEP, 10, 0, 0, 0);
	}
}

static long do_select(int n, fd_set *r, fd_set *w, fd_set *e, long ms)
{
	if (n < 0 || n > FD_SETSIZE)
		return -EINVAL;
	unsigned long start = tos(T_TICKS, 0, 0, 0, 0);
	fd_set rin, win, ein;
	FD_ZERO(&rin); FD_ZERO(&win); FD_ZERO(&ein);
	size_t bytes = (n + 7) / 8;
	if (r) { touch(r, sizeof(*r)); memcpy(&rin, r, bytes); }
	if (w) { touch(w, sizeof(*w)); memcpy(&win, w, bytes); }
	if (e) { touch(e, sizeof(*e)); memcpy(&ein, e, bytes); }
	for (;;) {
		long ready = 0;
		fd_set ro, wo, eo;
		FD_ZERO(&ro); FD_ZERO(&wo); FD_ZERO(&eo);
		for (int fd = 0; fd < n; fd++) {
			short want = (FD_ISSET(fd, &rin) ? POLLIN : 0) |
			             (FD_ISSET(fd, &win) ? POLLOUT : 0);
			if (!want && !FD_ISSET(fd, &ein))
				continue;
			short got = fd_events(fd, want);
			if (got & POLLNVAL)
				return -EBADF;
			if ((got & (POLLIN | POLLHUP | POLLERR)) && FD_ISSET(fd, &rin)) {
				FD_SET(fd, &ro);
				ready++;
			}
			if ((got & (POLLOUT | POLLERR)) && FD_ISSET(fd, &win)) {
				FD_SET(fd, &wo);
				ready++;
			}
		}
		if (ready || !ms ||
		    (ms > 0 && (tos(T_TICKS, 0, 0, 0, 0) - start) * (1000 / T_HZ)
		               >= (unsigned long)ms)) {
			if (r) memcpy(r, &ro, bytes);
			if (w) memcpy(w, &wo, bytes);
			if (e) memcpy(e, &eo, bytes);
			return ready;
		}
		tos(T_SLEEP, 10, 0, 0, 0);
	}
}

static long ts_ms(const struct timespec *ts)
{
	if (!ts)
		return -1;
	touch(ts, sizeof(*ts));
	return ts->tv_sec * 1000 + (ts->tv_nsec + 999999) / 1000000;
}

/* --- Whose call is it ------------------------------------------------ */

/* Does this call belong here: a socket call, poll or select, or a call
   on a descriptor that is a socket or a made-up file */
NET_HIDDEN int __tos_net_owns(long n, long fd)
{
	switch (n) {
	case SYS_socket: case SYS_socketpair: case SYS_connect: case SYS_bind:
	case SYS_listen: case SYS_accept: case SYS_accept4:
	case SYS_sendto: case SYS_recvfrom: case SYS_sendmsg: case SYS_recvmsg:
	case SYS_shutdown: case SYS_getsockname: case SYS_getpeername:
	case SYS_setsockopt: case SYS_getsockopt:
	case SYS_poll: case SYS_ppoll: case SYS_select: case SYS_pselect6:
		return 1;
	case SYS_read: case SYS_write: case SYS_readv: case SYS_writev:
	case SYS_close: case SYS_fstat: case SYS_fcntl: case SYS_ioctl:
	case SYS_lseek: case SYS_pread64: case SYS_pwrite64:
		return fd >= SOCK_BASE && fd < VFILE_BASE + VFILES;
	}
	return 0;
}

static long vfile_call(struct vf *v, long n, long a, long b, long c)
{
	switch (n) {
	case SYS_read: {
		if (v->random) {
			long done = 0;
			while (done < c) {
				long k = c - done > 4096 ? 4096 : c - done;
				touch((char *)b + done, k);
				if (tos(T_GETRANDOM, b + done, k, 0, 0) != k)
					return done ? done : -EFAULT;
				done += k;
			}
			return done;
		}
		long k = v->len - v->pos < c ? v->len - v->pos : c;
		touch((void *)b, k);
		memcpy((void *)b, (v->big ? v->big : v->data) + v->pos, k);
		v->pos += k;
		return k;
	}
	case SYS_readv: {
		const struct iovec *iov = (const void *)b;
		long total = 0;
		for (int i = 0; i < c; i++) {
			long k = vfile_call(v, SYS_read, a, (long)iov[i].iov_base,
			                    iov[i].iov_len);
			total += k;
			if ((size_t)k < iov[i].iov_len)
				break;
		}
		return total;
	}
	case SYS_lseek: {
		if (v->random)
			return 0;
		/* SEEK_END too: a file is often measured by seeking to its
		   end and asking where that is (Mbed TLS reads its CA file
		   so) */
		long to = c == SEEK_SET ? b : c == SEEK_CUR ? v->pos + b
		        : c == SEEK_END ? v->len + b : -1;
		if (to < 0 || to > v->len)
			return -EINVAL;
		return v->pos = (int)to;
	}
	case SYS_close:
		lock();
		free(v->big);
		v->big = 0;
		v->used = 0;
		unlock();
		return 0;
	case SYS_fstat: {
		struct kstat *st = (void *)b;
		touch(st, sizeof(*st));
		memset(st, 0, sizeof(*st));
		st->st_mode = v->random ? S_IFCHR | 0444 : S_IFREG | 0444;
		st->st_size = v->random ? 0 : v->len;
		st->st_nlink = 1;
		return 0;
	}
	case SYS_fcntl:
		return c == F_GETFL ? O_RDONLY : 0;
	}
	return -EBADF;
}

NET_HIDDEN long __tos_net(long n, long a, long b, long c, long d, long e, long f)
{
	struct sk *s;
	struct vf *v;

	switch (n) {
	case SYS_socket: {
		int type = b & 0xf;
		if (a != AF_INET)
			return -EAFNOSUPPORT;
		if (type != SOCK_STREAM && type != SOCK_DGRAM)
			return -EPROTONOSUPPORT;
		return sock_new(type == SOCK_STREAM, (b & SOCK_NONBLOCK) != 0);
	}
	case SYS_socketpair:
		return -EAFNOSUPPORT;
	case SYS_poll:
		return do_poll((struct pollfd *)a, b, c);
	case SYS_ppoll:
		return do_poll((struct pollfd *)a, b, ts_ms((const void *)c));
	case SYS_select: {
		long ms = -1;
		if (e) {
			const struct timeval *tv = (const void *)e;
			touch(tv, sizeof(*tv));
			ms = tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000;
		}
		return do_select(a, (void *)b, (void *)c, (void *)d, ms);
	}
	case SYS_pselect6:
		return do_select(a, (void *)b, (void *)c, (void *)d,
		                 ts_ms((const void *)e));
	}

	if ((v = vfile_of(a)))
		return vfile_call(v, n, a, b, c);
	if (!(s = sock_of(a)))
		return n == SYS_close || n == SYS_fstat || n == SYS_read ||
		       n == SYS_write ? -EBADF : -ENOTSOCK;

	switch (n) {
	case SYS_connect:  return do_connect(s, (const void *)b, c);
	case SYS_bind:     return do_bind(s, (const void *)b, c);
	case SYS_listen:   return do_listen(s);
	case SYS_accept:   return do_accept(s, (void *)b, (socklen_t *)c, 0);
	case SYS_accept4:  return do_accept(s, (void *)b, (socklen_t *)c, d);
	case SYS_write:
	case SYS_sendto:
		if (n == SYS_write)
			d = e = f = 0;
		return s->stream ? stream_send(s, (const char *)b, c)
		                 : dgram_send(s, (const void *)b, c,
		                              (const void *)e, f);
	case SYS_read:
	case SYS_recvfrom:
		if (n == SYS_read)
			d = e = f = 0;
		return s->stream ? stream_recv(s, (char *)b, c, d)
		                 : dgram_recv(s, (void *)b, c, d, (void *)e,
		                              (socklen_t *)f);
	case SYS_writev:
	case SYS_readv:
	case SYS_sendmsg:
	case SYS_recvmsg: {
		const struct iovec *iov;
		int cnt, flags = 0;
		struct msghdr *m = 0;
		if (n == SYS_sendmsg || n == SYS_recvmsg) {
			m = (struct msghdr *)b;
			touch(m, sizeof(*m));
			iov = m->msg_iov;
			cnt = m->msg_iovlen;
			flags = c;
		} else {
			iov = (const struct iovec *)b;
			cnt = c;
		}
		int out = n == SYS_writev || n == SYS_sendmsg;
		long total = 0;
		for (int i = 0; i < cnt; i++) {
			if (!iov[i].iov_len)
				continue;
			long r;
			if (out)
				r = s->stream
				    ? stream_send(s, iov[i].iov_base, iov[i].iov_len)
				    : dgram_send(s, iov[i].iov_base, iov[i].iov_len,
				                 m ? m->msg_name : 0,
				                 m ? m->msg_namelen : 0);
			else
				r = s->stream
				    ? stream_recv(s, iov[i].iov_base, iov[i].iov_len,
				                  flags | (total ? MSG_DONTWAIT : 0))
				    : dgram_recv(s, iov[i].iov_base, iov[i].iov_len,
				                 flags, m ? m->msg_name : 0,
				                 m ? &m->msg_namelen : 0);
			if (r < 0)
				return total ? total : r;
			total += r;
			if ((size_t)r < iov[i].iov_len || (!s->stream && !out))
				break;
		}
		if (m && !out) {
			m->msg_controllen = 0;
			m->msg_flags = 0;
		}
		return total;
	}
	case SYS_close:
		return do_close(s);
	case SYS_shutdown:
		return s->stream && s->state == S_CONNECTED ? 0 : -ENOTCONN;
	case SYS_getsockname: {
		unsigned short port = s->lport;
		if (s->stream && s->kid >= 0 && s->state != S_LISTENING) {
			struct { unsigned ip; unsigned short rport, lport; } ai;
			touch(&ai, sizeof(ai));
			if (tos(T_TCP_ADDR, s->kid, (long)&ai, 0, 0) == 0)
				port = ai.lport;
		}
		addr_out((void *)b, (socklen_t *)c, s->kid >= 0 ? own_ip() : 0, port);
		return 0;
	}
	case SYS_getpeername:
		if (s->state != S_CONNECTED || (!s->rip && !s->rport))
			return -ENOTCONN;
		addr_out((void *)b, (socklen_t *)c, s->rip, s->rport);
		return 0;
	case SYS_setsockopt:
		return 0;                    /* reuse, no-delay, buffers: all fine */
	case SYS_getsockopt:
		return do_getsockopt(s, b, c, (void *)d, (socklen_t *)e);
	case SYS_fstat: {
		struct kstat *st = (void *)b;
		touch(st, sizeof(*st));
		memset(st, 0, sizeof(*st));
		st->st_mode = S_IFSOCK | 0777;
		st->st_nlink = 1;
		return 0;
	}
	case SYS_fcntl:
		if (b == F_GETFL)
			return O_RDWR | (s->nonblock ? O_NONBLOCK : 0);
		if (b == F_SETFL) {
			s->nonblock = (c & O_NONBLOCK) != 0;
			return 0;
		}
		return b == F_GETFD || b == F_SETFD ? 0 : -EINVAL;
	case SYS_ioctl:
		if (b == FIONBIO) {
			touch((void *)c, sizeof(int));
			s->nonblock = *(int *)c != 0;
			return 0;
		}
		if (b == FIONREAD) {
			long bits = s->stream && s->kid >= 0
			            ? tos(T_TCP_POLL, s->kid, 0, 0, 0) : 0;
			touch((void *)c, sizeof(int));
			*(int *)c = bits > 0 ? (int)((unsigned long)bits >> 16) : 0;
			return 0;
		}
		return -ENOTTY;
	case SYS_lseek:
		return -ESPIPE;
	}
	return -EINVAL;
}
