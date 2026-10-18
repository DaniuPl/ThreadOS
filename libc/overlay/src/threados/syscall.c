/* musl on ThreadOS 64: Linux system calls in ThreadOS words
 *
 * musl is written against Linux: it asks for read, openat, mmap,
 * clock_gettime by Linux's numbers and expects Linux's answers - a
 * result, or minus an errno. ThreadOS has its own calls
 * (kernel64/syscall64.h), with its own numbers and shapes: a path is a
 * pointer and a length, open takes a create flag rather than O_*
 * bits, a failure is -1 (or -2 for "not allowed") with no reason.
 * Every call musl makes comes here (arch/x86_64/syscall_arch.h sends
 * it), and this is where one is turned into the other.
 *
 * Some of it is a straight renaming. Some of it the kernel does not
 * have, and it is built here out of what it does have:
 *
 *   stat     opens the file and asks the kernel about the descriptor
 *            (SYS64_FDINFO); a directory is one we can change into
 *   lseek    to the end is to the size; SEEK_CUR is to the position
 *   O_APPEND moves to the end before every write
 *   mmap     the kernel only hands out fresh memory and never takes
 *            it back, so what munmap returns is kept here and handed
 *            out again (zeroed, as Linux promises)
 *   stderr   there is no console on descriptor two; while two is
 *            unassigned, what is written there goes where one goes
 *   a tty    the keyboard comes without an echo and without line
 *            editing - a terminal's line discipline is built here, for
 *            a program reading the console a line at a time
 *   signals  the kernel keeps one handler per process; the table per
 *            signal is kept here, behind a handler of our own
 *   clocks   the CMOS clock has seconds and the timer has hundredths;
 *            the wall clock is the first plus the second since then
 *
 * What there is no way to build - threads, sockets, poll - answers
 * ENOSYS, rather than a Linux number falling on some unrelated
 * ThreadOS call.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/times.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include "syscall.h"
#include "kstat.h"

/* --- The ThreadOS side (kernel64/syscall64.h) --------------------- */

#define T_WRITE      1
#define T_EXIT       2
#define T_SLEEP      4
#define T_TICKS      5
#define T_FORK       6
#define T_EXEC       7
#define T_WAIT       8
#define T_SIGNAL     9
#define T_KILL      11
#define T_READ      12
#define T_OPEN      13
#define T_CLOSE     14
#define T_SBRK      15
#define T_MKDIR     16
#define T_CHDIR     17
#define T_GETCWD    18
#define T_UNLINK    20
#define T_PIPE      21
#define T_DUP2      22
#define T_TIME      37
#define T_SYSINFO   38
#define T_GETPID    39
#define T_MMAP      43
#define T_LEVEL     47
#define T_TRYWAIT   48
#define T_SEEK      49
#define T_RENAME    55
#define T_FDINFO    74
#define T_SET_FS    75

#define T_OPEN_CREATE 1
#define T_OPEN_TRUNC  2

#define K_NONE   0                   /* nothing: 0 and 1 are the console */
#define K_FILE   1
#define K_PIPE_R 2
#define K_PIPE_W 3

#define T_FDS    8                   /* FD_MAX in kernel64/task64.c */
#define T_HZ     100                 /* the timer, kernel64/kmain64.c */
#define T_CHUNK  4096                /* the most one read or write moves */
#define AT_EMPTY  0x1000             /* AT_EMPTY_PATH, a GNU name */

static long tos(long n, long a1, long a2, long a3, long a4)
{
	long ret;
	register long r10 __asm__("r10") = a4;
	__asm__ __volatile__ ("syscall" : "=a"(ret)
		: "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
		: "rcx", "r11", "memory");
	return ret;
}

struct fdinfo { int kind; unsigned size, pos; };

static int fdinfo(int fd, struct fdinfo *fi)
{
	return tos(T_FDINFO, fd, (long)fi, 0, 0) < 0 ? -1 : 0;
}

static int kind_of(int fd)
{
	struct fdinfo fi;
	if (fd < 0 || fd >= T_FDS || fdinfo(fd, &fi) < 0)
		return -1;
	return fi.kind;
}

/* The kernel checks a buffer by its page tables, and a page of heap or
   stack nobody has touched yet is not in them - it is made on the
   first touch. Touching each page first turns "not yours" into a page
   like any other. */
static void touch(const void *p, size_t len)
{
	const volatile char *c = p;
	if (!len)
		return;
	for (size_t off = 0; off < len; off += 4096 - ((size_t)(c + off) & 4095))
		(void)c[off];
	(void)c[len - 1];
}

/* Descriptor 0 unassigned is the keyboard, 1 the screen. And 2 is
   nothing to the kernel - so while it is unassigned it stands for
   wherever 1 goes. */
static int is_console_in(int fd)
{
	return fd == 0 && kind_of(0) == K_NONE;
}

static int is_console_out(int fd)
{
	if (fd == 2 && kind_of(2) == K_NONE)
		fd = 1;
	return fd == 1 && kind_of(1) == K_NONE;
}

static int out_fd(int fd)
{
	return fd == 2 && kind_of(2) == K_NONE ? 1 : fd;
}

/* What ThreadOS has no notion of: the open flags of a descriptor (for
   O_APPEND and fcntl). Inherited by a fork with the rest of memory. */
static int fd_flags[T_FDS];
static unsigned char fd_known[T_FDS];

static int flags_of(int fd)
{
	if (fd < 0 || fd >= T_FDS)
		return 0;
	if (fd_known[fd])
		return fd_flags[fd];
	return fd == 0 ? O_RDONLY : fd <= 2 ? O_WRONLY : O_RDWR;
}

static void set_flags(int fd, int flags)
{
	if (fd >= 0 && fd < T_FDS) {
		fd_flags[fd] = flags;
		fd_known[fd] = 1;
	}
}

static size_t slen(const char *s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}

/* --- The console's line discipline ---------------------------------
 *
 * The keyboard hands over characters as they are typed, and nobody
 * shows them: in ThreadOS the echo is the reader's job. A C program
 * reading stdin expects a terminal's manners instead - a whole line at
 * a time, typed where it can be seen, Backspace taking back a letter
 * (all of a UTF-8 letter, not one byte of it), Ctrl+D on an empty line
 * the end of the input. */
static char line[1024];
static size_t line_len, line_pos;

static void echo(const char *s, size_t n)
{
	if (is_console_out(1))
		tos(T_WRITE, 1, (long)s, n, 0);
}

static long console_read(char *buf, size_t n)
{
	if (line_pos == line_len) {
		line_len = line_pos = 0;
		for (;;) {
			char c;
			long r = tos(T_READ, 0, (long)&c, 1, 0);
			if (r == 0)
				return 0;
			if (r < 0)
				return -EINTR;   /* a signal; what was typed stays */
			if (c == '\r')
				c = '\n';
			if (c == '\n') {
				line[line_len++] = '\n';
				echo("\n", 1);
				break;
			}
			if (c == '\b' || c == 0x7f) {
				if (!line_len)
					continue;
				while (line_len && ((unsigned char)line[line_len - 1] & 0xC0) == 0x80)
					line_len--;
				if (line_len)
					line_len--;
				echo("\b", 1);
				continue;
			}
			if (c == 4) {            /* Ctrl+D */
				if (!line_len)
					return 0;
				break;
			}
			if ((unsigned char)c < ' ' && c != '\t')
				continue;
			if (line_len < sizeof(line) - 1) {
				line[line_len++] = c;
				echo(&c, 1);
			}
		}
	}
	size_t k = line_len - line_pos;
	if (k > n)
		k = n;
	memcpy(buf, line + line_pos, k);
	line_pos += k;
	return k;
}

/* --- Reading and writing ------------------------------------------- */

static long do_read(int fd, void *buf, size_t n)
{
	if (!n)
		return 0;
	if (fd < 0 || fd >= T_FDS)
		return -EBADF;
	touch(buf, n);
	if (is_console_in(fd))
		return console_read(buf, n);
	int k = kind_of(fd);
	if (k == K_NONE || k == K_PIPE_W)
		return -EBADF;
	if (n > T_CHUNK)
		n = T_CHUNK;
	long r = tos(T_READ, fd, (long)buf, n, 0);
	return r < 0 ? (k == K_PIPE_R ? -EINTR : -EIO) : r;
}

static long do_write(int fd, const void *buf, size_t n)
{
	if (fd < 0 || fd >= T_FDS)
		return -EBADF;
	int append = flags_of(fd) & O_APPEND;
	fd = out_fd(fd);
	int k = kind_of(fd);
	if (k == K_PIPE_R || (k == K_NONE && fd != 1))
		return -EBADF;
	touch(buf, n);
	size_t done = 0;
	while (done < n) {
		size_t part = n - done > T_CHUNK ? T_CHUNK : n - done;
		if (append && k == K_FILE) {
			struct fdinfo fi;
			if (fdinfo(fd, &fi) == 0)
				tos(T_SEEK, fd, fi.size, 0, 0);
		}
		long r = tos(T_WRITE, fd, (long)buf + done, part, 0);
		if (r <= 0) {
			if (done)
				break;
			return k == K_PIPE_W ? -EPIPE : r == 0 ? -ENOSPC : -EIO;
		}
		done += r;
	}
	return done;
}

static long do_readv(int fd, const struct iovec *iov, int cnt)
{
	long total = 0;
	for (int i = 0; i < cnt; i++) {
		if (!iov[i].iov_len)
			continue;
		long r = do_read(fd, iov[i].iov_base, iov[i].iov_len);
		if (r < 0)
			return total ? total : r;
		total += r;
		if ((size_t)r < iov[i].iov_len)
			break;
	}
	return total;
}

static long do_writev(int fd, const struct iovec *iov, int cnt)
{
	long total = 0;
	for (int i = 0; i < cnt; i++) {
		if (!iov[i].iov_len)
			continue;
		long r = do_write(fd, iov[i].iov_base, iov[i].iov_len);
		if (r < 0)
			return total ? total : r;
		total += r;
		if ((size_t)r < iov[i].iov_len)
			break;
	}
	return total;
}

/* --- Files and directories ----------------------------------------- */

static int path_ok(const char *p)
{
	if (!p)
		return -EFAULT;
	size_t n = slen(p);
	if (!n)
		return -ENOENT;
	if (n > 255)
		return -ENAMETOOLONG;
	return 0;
}

/* "/x" for "x" in the root, "./" and trailing slashes stripped: the
   ThreadOS file system knows neither */
static const char *tidy(const char *p, char *out)
{
	size_t n = 0;
	while (p[0] == '.' && p[1] == '/')
		p += 2;
	while (*p && n < 255)
		out[n++] = *p++;
	while (n > 1 && out[n - 1] == '/')
		n--;
	out[n] = 0;
	return out;
}

/* A directory is a place one can go to. The kernel has no stat, so we
   ask by going there and coming back. */
static int is_dir(const char *p)
{
	char here[256];
	if (!strcmp(p, ".") || !strcmp(p, "/"))
		return 1;
	if (tos(T_GETCWD, (long)here, sizeof(here), 0, 0) < 0)
		return 0;
	if (tos(T_CHDIR, (long)p, slen(p), 0, 0) < 0)
		return 0;
	tos(T_CHDIR, (long)here, slen(here), 0, 0);
	return 1;
}

static long do_open(const char *path, int flags)
{
	char buf[256];
	int e = path_ok(path);
	if (e)
		return e;
	touch(path, slen(path) + 1);
	path = tidy(path, buf);

	if (flags & O_DIRECTORY)
		return is_dir(path) ? -ENOSYS : -ENOTDIR;
	int acc = flags & O_ACCMODE;
	if ((flags & O_CREAT) && (flags & O_EXCL)) {
		long fd = tos(T_OPEN, (long)path, slen(path), 0, 0);
		if (fd >= 0) {
			tos(T_CLOSE, fd, 0, 0, 0);
			return -EEXIST;
		}
		if (is_dir(path))
			return -EEXIST;
	}
	long how = 0;
	if (flags & O_CREAT)
		how |= T_OPEN_CREATE;
	if ((flags & O_TRUNC) && acc != O_RDONLY)
		how |= T_OPEN_TRUNC;
	long fd = tos(T_OPEN, (long)path, slen(path), how, 0);
	if (fd == -2)
		return -EACCES;
	if (fd < 0) {
		if (is_dir(path))
			return acc == O_RDONLY ? -ENOSYS : -EISDIR;
		return -ENOENT;
	}
	set_flags(fd, flags & ~(O_CREAT | O_EXCL | O_TRUNC | O_NOCTTY | O_CLOEXEC));
	return fd;
}

static void fill_stat(struct kstat *st, unsigned mode, unsigned size, long ino)
{
	memset(st, 0, sizeof(*st));
	st->st_mode = mode;
	st->st_nlink = 1;
	st->st_ino = ino;
	st->st_size = size;
	st->st_blksize = 4096;
	st->st_blocks = (size + 511) / 512;
	st->st_uid = st->st_gid = tos(T_LEVEL, 0, 0, 0, 0) == 0 ? 0 : 1000;
}

static long do_fstat(int fd, struct kstat *st)
{
	struct fdinfo fi;
	touch(st, sizeof(*st));
	if (fd < 0 || fd >= T_FDS || fdinfo(fd, &fi) < 0)
		return -EBADF;
	switch (fi.kind) {
	case K_FILE:
		fill_stat(st, S_IFREG | 0644, fi.size, 100 + fd);
		return 0;
	case K_PIPE_R:
	case K_PIPE_W:
		fill_stat(st, S_IFIFO | 0600, 0, 10 + fd);
		return 0;
	}
	if (fd <= 2 && (fd == 0 ? is_console_in(0) : is_console_out(fd))) {
		fill_stat(st, S_IFCHR | 0620, 0, 1 + fd);
		st->st_rdev = 0x0400 + fd;
		return 0;
	}
	return -EBADF;
}

static long do_stat(const char *path, struct kstat *st)
{
	char buf[256];
	int e = path_ok(path);
	if (e)
		return e;
	touch(path, slen(path) + 1);
	touch(st, sizeof(*st));
	path = tidy(path, buf);
	long fd = tos(T_OPEN, (long)path, slen(path), 0, 0);
	if (fd >= 0) {
		struct fdinfo fi = { 0 };
		fdinfo(fd, &fi);
		tos(T_CLOSE, fd, 0, 0, 0);
		fill_stat(st, S_IFREG | 0644, fi.size, 1000);
		return 0;
	}
	if (is_dir(path)) {
		fill_stat(st, S_IFDIR | 0755, 0, 2000);
		st->st_nlink = 2;
		return 0;
	}
	return -ENOENT;
}

static long do_lseek(int fd, long off, int whence)
{
	struct fdinfo fi;
	if (fd < 0 || fd >= T_FDS || fdinfo(fd, &fi) < 0)
		return -EBADF;
	if (fi.kind != K_FILE)
		return fi.kind == K_NONE && fd > 2 ? -EBADF : -ESPIPE;
	long at;
	switch (whence) {
	case SEEK_SET: at = off; break;
	case SEEK_CUR: at = (long)fi.pos + off; break;
	case SEEK_END: at = (long)fi.size + off; break;
	default: return -EINVAL;
	}
	if (at < 0)
		return -EINVAL;
	if (at > 0xFFFFFFFFL)
		return -EOVERFLOW;
	if (tos(T_SEEK, fd, at, 0, 0) < 0)
		return -EINVAL;
	return at;
}

static long do_close(int fd)
{
	if (fd < 0 || fd >= T_FDS)
		return -EBADF;
	fd_known[fd] = 0;
	if (kind_of(fd) == K_NONE)
		return fd <= 2 ? 0 : -EBADF;
	return tos(T_CLOSE, fd, 0, 0, 0) < 0 ? -EBADF : 0;
}

static long do_dup2(int old, int fresh)
{
	if (old < 0 || old >= T_FDS || fresh < 0 || fresh >= T_FDS)
		return -EBADF;
	if (old == fresh)
		return kind_of(old) == K_NONE && old > 2 ? -EBADF : fresh;
	if (kind_of(old) == K_NONE) {
		/* The console is the ABSENCE of a descriptor - copying it is
		   closing the other one, where that means the same */
		if (old > 2 || fresh > 2 || (old == 0) != (fresh == 0))
			return -EBADF;
		do_close(fresh);
		return fresh;
	}
	if (tos(T_DUP2, old, fresh, 0, 0) < 0)
		return -EBADF;
	set_flags(fresh, flags_of(old));
	return fresh;
}

static long do_dupfd(int old, int from)
{
	for (int fd = from < 3 ? 3 : from; fd < T_FDS; fd++)
		if (kind_of(fd) == K_NONE)
			return do_dup2(old, fd);
	return -EMFILE;
}

static long do_fcntl(int fd, int cmd, long arg)
{
	if (fd < 0 || fd >= T_FDS)
		return -EBADF;
	switch (cmd) {
	case F_DUPFD:
	case F_DUPFD_CLOEXEC:
		return do_dupfd(fd, arg);
	case F_GETFD:
	case F_SETFD:
		return 0;
	case F_GETFL:
		return flags_of(fd);
	case F_SETFL:
		set_flags(fd, (flags_of(fd) & O_ACCMODE) |
		              (arg & (O_APPEND | O_NONBLOCK)));
		return 0;
	case F_GETLK:
	case F_SETLK:
	case F_SETLKW:
		return 0;                    /* one program at a time: no locks */
	}
	return -EINVAL;
}

static long do_ioctl(int fd, unsigned long req, void *arg)
{
	int tty = fd == 0 ? is_console_in(0) : is_console_out(fd);
	if (fd < 0 || fd >= T_FDS)
		return -EBADF;
	if (!tty)
		return -ENOTTY;
	if (req == TIOCGWINSZ) {
		struct winsize *ws = arg;
		touch(ws, sizeof(*ws));
		ws->ws_row = 25;
		ws->ws_col = 80;
		ws->ws_xpixel = ws->ws_ypixel = 0;
		return 0;
	}
	return -ENOTTY;
}

static long name_call(int nr, const char *path)
{
	char buf[256];
	int e = path_ok(path);
	if (e)
		return e;
	touch(path, slen(path) + 1);
	path = tidy(path, buf);
	long r = tos(nr, (long)path, slen(path), 0, 0);
	if (r == -2)
		return -EACCES;
	if (r >= 0)
		return 0;
	if (nr == T_MKDIR)
		return is_dir(path) || do_stat(path, &(struct kstat){0}) == 0
			? -EEXIST : -ENOENT;
	return -ENOENT;
}

static long do_rename(const char *from, const char *to)
{
	char a[256], b[256];
	int e = path_ok(from);
	if (!e)
		e = path_ok(to);
	if (e)
		return e;
	touch(from, slen(from) + 1);
	touch(to, slen(to) + 1);
	from = tidy(from, a);
	to = tidy(to, b);
	long r = tos(T_RENAME, (long)from, slen(from), (long)to, slen(to));
	return r == -2 ? -EACCES : r < 0 ? -ENOENT : 0;
}

static long do_getcwd(char *buf, size_t size)
{
	if (!size)
		return -EINVAL;
	touch(buf, size);
	long r = tos(T_GETCWD, (long)buf, size, 0, 0);
	if (r < 0)
		return -EIO;
	if ((size_t)r + 1 >= size && size < 256)
		return -ERANGE;
	return r + 1;                    /* Linux counts the zero */
}

/* --- Memory -------------------------------------------------------- */

static long do_brk(unsigned long want)
{
	long cur = tos(T_SBRK, 0, 0, 0, 0);
	if (!want || cur < 0)
		return cur;
	long r = tos(T_SBRK, (long)(want - cur), 0, 0, 0);
	return r < 0 ? cur : (long)want;
}

/* What munmap gave back, waiting to be handed out again. The kernel's
   mapping area only grows, and malloc gives back and asks again all
   the time - without this a long-running program would walk through
   all of it. */
#define HOLES 256
static struct { unsigned long at, len; } holes[HOLES];
static int nholes;

static void hole_add(unsigned long at, unsigned long len)
{
	for (int i = 0; i < nholes; i++) {
		if (holes[i].at + holes[i].len == at) {
			holes[i].len += len;
			for (int j = 0; j < nholes; j++)
				if (j != i && holes[j].at == holes[i].at + holes[i].len) {
					holes[i].len += holes[j].len;
					holes[j] = holes[--nholes];
					break;
				}
			return;
		}
		if (at + len == holes[i].at) {
			holes[i].at = at;
			holes[i].len += len;
			return;
		}
	}
	if (nholes < HOLES) {
		holes[nholes].at = at;
		holes[nholes].len = len;
		nholes++;
	}
	/* (with no room left the range is simply forgotten) */
}

static long do_mmap(unsigned long addr, unsigned long len, int prot,
                    int flags, int fd, long off)
{
	(void)addr; (void)prot; (void)off; (void)fd;
	if (!len)
		return -EINVAL;
	if (!(flags & MAP_ANONYMOUS))
		return -ENODEV;
	if (flags & MAP_FIXED)
		return -ENOMEM;
	len = (len + 4095) & ~4095UL;
	int best = -1;
	for (int i = 0; i < nholes; i++)
		if (holes[i].len >= len && (best < 0 || holes[i].len < holes[best].len))
			best = i;
	if (best >= 0) {
		unsigned long at = holes[best].at;
		holes[best].at += len;
		holes[best].len -= len;
		if (!holes[best].len)
			holes[best] = holes[--nholes];
		memset((void *)at, 0, len);
		return at;
	}
	long at = tos(T_MMAP, len, 0, 0, 0);
	return at <= 0 ? -ENOMEM : at;
}

static long do_munmap(unsigned long addr, unsigned long len)
{
	if (addr & 4095)
		return -EINVAL;
	len = (len + 4095) & ~4095UL;
	if (len)
		hole_add(addr, len);
	return 0;
}

/* --- Time ---------------------------------------------------------- */

struct rtc { unsigned short year; unsigned char mon, day, hour, min, sec; }
	__attribute__((packed));

static long days_from_civil(long y, unsigned m, unsigned d)
{
	y -= m <= 2;
	long era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = (unsigned)(y - era * 400);
	unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (long)doe - 719468;
}

/* The wall clock: the CMOS clock's second once, the timer from then on.
   The CMOS clock alone would stand still for a whole second and the
   timer alone does not know the date. */
static long wall_base, ticks_base;

static void wall_clock(struct timespec *ts)
{
	unsigned long t = tos(T_TICKS, 0, 0, 0, 0);
	if (!wall_base) {
		struct rtc r;
		tos(T_TIME, (long)&r, 0, 0, 0);
		wall_base = days_from_civil(r.year, r.mon, r.day) * 86400L +
		            r.hour * 3600L + r.min * 60L + r.sec;
		ticks_base = t;
	}
	unsigned long since = t - ticks_base;
	ts->tv_sec = wall_base + since / T_HZ;
	ts->tv_nsec = (since % T_HZ) * (1000000000L / T_HZ);
}

static long do_clock_gettime(int clk, struct timespec *ts)
{
	touch(ts, sizeof(*ts));
	if (clk == CLOCK_REALTIME || clk == CLOCK_REALTIME_COARSE) {
		wall_clock(ts);
		return 0;
	}
	if (clk < 0 || clk > CLOCK_TAI)
		return -EINVAL;
	unsigned long t = tos(T_TICKS, 0, 0, 0, 0);
	ts->tv_sec = t / T_HZ;
	ts->tv_nsec = (t % T_HZ) * (1000000000L / T_HZ);
	return 0;
}

static long do_sleep(int clk, int flags, const struct timespec *req)
{
	struct timespec now;
	long sec = req->tv_sec, nsec = req->tv_nsec;
	if (nsec < 0 || nsec >= 1000000000L || sec < 0)
		return -EINVAL;
	if (flags & TIMER_ABSTIME) {
		do_clock_gettime(clk, &now);
		sec -= now.tv_sec;
		nsec -= now.tv_nsec;
		if (nsec < 0) {
			nsec += 1000000000L;
			sec--;
		}
		if (sec < 0)
			return 0;
	}
	long ms = sec * 1000 + (nsec + 999999) / 1000000;
	tos(T_SLEEP, ms, 0, 0, 0);
	return 0;
}

/* --- Processes and signals ----------------------------------------- */

static long do_wait4(int pid, int *status, int opts)
{
	int code = 0;
	long id;
	if (pid > 0) {
		for (;;) {
			long r = tos(T_TRYWAIT, pid, (long)&code, 0, 0);
			if (r == 0)
				break;
			if (r == -4 || r == -1)
				return -ECHILD;
			if (opts & WNOHANG)
				return 0;
			tos(T_SLEEP, 10, 0, 0, 0);
		}
		id = pid;
	} else {
		if (opts & WNOHANG)
			return -ENOSYS;
		id = tos(T_WAIT, (long)&code, 0, 0, 0);
		if (id < 0)
			return -ECHILD;
	}
	if (status) {
		touch(status, sizeof(*status));
		/* -1 is the kernel's "killed" - a signal, as far as the
		   parent can tell */
		*status = code < 0 ? SIGKILL : (code & 0xff) << 8;
	}
	return id;
}

static long do_execve(const char *path, char *const argv[])
{
	int e = path_ok(path);
	if (e)
		return e;
	/* ThreadOS runs programs by their name in the initrd */
	const char *name = path;
	for (const char *p = path; *p; p++)
		if (*p == '/')
			name = p + 1;
	long r = tos(T_EXEC, (long)name, slen(name), (long)argv, 0);
	return r < 0 ? -ENOENT : r;
}

/* One kernel handler for the process, one entry per signal here */
static struct k_sigaction { void (*handler)(int); unsigned long flags;
	void (*restorer)(void); unsigned mask[2]; } actions[65];
static int handler_installed;

static void on_signal(int sig)
{
	void (*h)(int) = sig > 0 && sig < 65 ? actions[sig].handler : 0;
	if (h == SIG_IGN)
		return;
	if (h == SIG_DFL || !h) {
		/* What the kernel would have done had nobody asked */
		if (sig == SIGCHLD || sig == SIGWINCH || sig == SIGURG ||
		    sig == SIGCONT)
			return;
		tos(T_EXIT, 128 + sig, 0, 0, 0);
	}
	h(sig);
}

static long do_sigaction(int sig, const struct k_sigaction *act,
                         struct k_sigaction *old)
{
	if (sig < 1 || sig >= 65)
		return -EINVAL;
	if (old) {
		touch(old, sizeof(*old));
		*old = actions[sig];
	}
	if (!act)
		return 0;
	if (sig == SIGKILL || sig == SIGSTOP)
		return -EINVAL;
	actions[sig] = *act;
	if (!handler_installed && act->handler != SIG_DFL) {
		/* The return from it is musl's own __restore_rt, which says
		   SYS64_SIGRETURN (src/signal/x86_64/restore.s) */
		tos(T_SIGNAL, 0, (long)on_signal,
		    (long)(act->restorer ? act->restorer : 0), 0);
		handler_installed = 1;
	}
	return 0;
}

static unsigned char rng_state_set;
static unsigned long rng_state;

static long do_getrandom(unsigned char *buf, size_t len)
{
	touch(buf, len);
	if (!rng_state_set) {
		unsigned lo, hi;
		__asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
		rng_state = (((unsigned long)hi << 32) | lo) ^ 0x9E3779B97F4A7C15UL;
		rng_state_set = 1;
	}
	for (size_t i = 0; i < len; i++) {
		unsigned lo, hi;
		__asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
		rng_state ^= lo;
		rng_state ^= rng_state << 13;
		rng_state ^= rng_state >> 7;
		rng_state ^= rng_state << 17;
		buf[i] = (unsigned char)(rng_state >> 24);
	}
	return len;
}

static void copy_name(char *dst, const char *src)
{
	size_t i = 0;
	for (; src[i] && i < 64; i++)
		dst[i] = src[i];
	dst[i] = 0;
}

static long do_uname(struct utsname *u)
{
	touch(u, sizeof(*u));
	memset(u, 0, sizeof(*u));
	copy_name(u->sysname, "ThreadOS");
	copy_name(u->nodename, "threados");
	copy_name(u->release, "1.0");
	copy_name(u->version, "ThreadOS 64");
	copy_name(u->machine, "x86_64");
	return 0;
}

static long do_sysinfo(struct sysinfo *si)
{
	struct { unsigned ram_kib, used_kib, uptime_s; } t;
	touch(si, sizeof(*si));
	memset(si, 0, sizeof(*si));
	if (tos(T_SYSINFO, (long)&t, 0, 0, 0) < 0)
		return -EIO;
	si->uptime = t.uptime_s;
	si->totalram = t.ram_kib;
	si->freeram = t.ram_kib - t.used_kib;
	si->procs = 1;
	si->mem_unit = 1024;
	return 0;
}

static long do_prlimit(int res, const struct rlimit *fresh, struct rlimit *old)
{
	(void)fresh;
	if (old) {
		touch(old, sizeof(*old));
		old->rlim_cur = old->rlim_max = RLIM_INFINITY;
		if (res == RLIMIT_STACK)
			old->rlim_cur = old->rlim_max = 4 << 20;  /* USER64_STACK_MAX */
		if (res == RLIMIT_NOFILE)
			old->rlim_cur = old->rlim_max = T_FDS;
	}
	return 0;
}

static unsigned long fs_now;

static long do_arch_prctl(int code, unsigned long addr)
{
	if (code == 0x1002) {            /* ARCH_SET_FS */
		touch((void *)addr, 8);
		if (tos(T_SET_FS, addr, 0, 0, 0) < 0)
			return -EPERM;
		fs_now = addr;
		return 0;
	}
	if (code == 0x1003) {            /* ARCH_GET_FS */
		*(unsigned long *)addr = fs_now;
		return 0;
	}
	return -EINVAL;
}

/* --- The switch ---------------------------------------------------- */

__attribute__((__visibility__("hidden")))
long __tos_syscall(long n, long a, long b, long c, long d, long e, long f)
{
	(void)f;
	switch (n) {
	case SYS_read:       return do_read(a, (void *)b, c);
	case SYS_write:      return do_write(a, (const void *)b, c);
	case SYS_readv:      return do_readv(a, (const struct iovec *)b, c);
	case SYS_writev:     return do_writev(a, (const struct iovec *)b, c);
	case SYS_pread64:
	case SYS_pwrite64: {
		long at = do_lseek(a, 0, SEEK_CUR);
		if (at < 0)
			return at;
		long r = do_lseek(a, d, SEEK_SET);
		if (r < 0)
			return r;
		r = n == SYS_pread64 ? do_read(a, (void *)b, c)
		                     : do_write(a, (const void *)b, c);
		do_lseek(a, at, SEEK_SET);
		return r;
	}
	case SYS_open:       return do_open((const char *)a, b);
	case SYS_openat:
		if (a != AT_FDCWD && ((const char *)b)[0] != '/')
			return -ENOSYS;
		return do_open((const char *)b, c);
	case SYS_close:      return do_close(a);
	case SYS_lseek:      return do_lseek(a, b, c);
	case SYS_fstat:      return do_fstat(a, (struct kstat *)b);
	case SYS_stat:
	case SYS_lstat:      return do_stat((const char *)a, (struct kstat *)b);
	case SYS_newfstatat:
		if ((d & AT_EMPTY) && !*(const char *)b)
			return do_fstat(a, (struct kstat *)c);
		if (a != AT_FDCWD && ((const char *)b)[0] != '/')
			return -ENOSYS;
		return do_stat((const char *)b, (struct kstat *)c);
	case SYS_statx:      return -ENOSYS;   /* musl falls back to fstatat */
	case SYS_access:
		return do_stat((const char *)a, &(struct kstat){0});
	case SYS_faccessat:
	case SYS_faccessat2:
		return do_stat((const char *)b, &(struct kstat){0});
	case SYS_ioctl:      return do_ioctl(a, b, (void *)c);
	case SYS_fcntl:      return do_fcntl(a, b, c);
	case SYS_dup:        return do_dupfd(a, 3);
	case SYS_dup2:       return do_dup2(a, b);
	case SYS_dup3:       return a == b ? -EINVAL : do_dup2(a, b);
	case SYS_pipe:
	case SYS_pipe2: {
		int *fds = (int *)a;
		touch(fds, 2 * sizeof(int));
		if (tos(T_PIPE, a, 0, 0, 0) < 0)
			return -EMFILE;
		set_flags(fds[0], O_RDONLY);
		set_flags(fds[1], O_WRONLY);
		return 0;
	}
	case SYS_fsync:
	case SYS_fdatasync:
	case SYS_sync:       return 0;
	case SYS_ftruncate: {
		struct fdinfo fi;
		if (a < 0 || a >= T_FDS || fdinfo(a, &fi) < 0 || fi.kind != K_FILE)
			return -EBADF;
		return (unsigned long)b == fi.size ? 0 : -ENOSYS;
	}
	case SYS_getcwd:     return do_getcwd((char *)a, b);
	case SYS_chdir:      return name_call(T_CHDIR, (const char *)a);
	case SYS_mkdir:      return name_call(T_MKDIR, (const char *)a);
	case SYS_mkdirat:    return name_call(T_MKDIR, (const char *)b);
	case SYS_rmdir:
	case SYS_unlink:     return name_call(T_UNLINK, (const char *)a);
	case SYS_unlinkat:   return name_call(T_UNLINK, (const char *)b);
	case SYS_rename:     return do_rename((const char *)a, (const char *)b);
	case SYS_renameat:
	case SYS_renameat2:  return do_rename((const char *)b, (const char *)d);
	case SYS_readlink:
	case SYS_readlinkat: return -EINVAL;     /* there are no links */
	case SYS_umask:      return 022;
	case SYS_chmod:
	case SYS_fchmod:
	case SYS_fchmodat:
	case SYS_utimensat:  return 0;

	case SYS_brk:        return do_brk(a);
	case SYS_mmap:       return do_mmap(a, b, c, d, e, f);
	case SYS_munmap:     return do_munmap(a, b);
	case SYS_mremap:     return -ENOMEM;     /* realloc copies instead */
	case SYS_mprotect:
	case SYS_madvise:
	case SYS_membarrier: return 0;

	case SYS_clock_gettime: return do_clock_gettime(a, (struct timespec *)b);
	case SYS_clock_getres:
		if (b) {
			touch((void *)b, sizeof(struct timespec));
			((struct timespec *)b)->tv_sec = 0;
			((struct timespec *)b)->tv_nsec = 1000000000L / T_HZ;
		}
		return 0;
	case SYS_gettimeofday: {
		struct timespec ts;
		wall_clock(&ts);
		if (a) {
			touch((void *)a, 16);
			((long *)a)[0] = ts.tv_sec;
			((long *)a)[1] = ts.tv_nsec / 1000;
		}
		return 0;
	}
	case SYS_time: {
		struct timespec ts;
		wall_clock(&ts);
		if (a)
			*(long *)a = ts.tv_sec;
		return ts.tv_sec;
	}
	case SYS_nanosleep:
		return do_sleep(CLOCK_MONOTONIC, 0, (const struct timespec *)a);
	case SYS_clock_nanosleep:
		return do_sleep(a, b, (const struct timespec *)c);
	case SYS_sched_yield:
		tos(T_SLEEP, 0, 0, 0, 0);
		return 0;
	case SYS_times: {
		unsigned long t = tos(T_TICKS, 0, 0, 0, 0);
		if (a) {
			struct tms *tm = (struct tms *)a;
			touch(tm, sizeof(*tm));
			tm->tms_utime = t;
			tm->tms_stime = tm->tms_cutime = tm->tms_cstime = 0;
		}
		return t;
	}
	case SYS_getrusage:
		touch((void *)b, sizeof(struct rusage));
		memset((void *)b, 0, sizeof(struct rusage));
		return 0;

	case SYS_exit:
	case SYS_exit_group:
		tos(T_EXIT, a, 0, 0, 0);
		for (;;);
	case SYS_getpid:
	case SYS_gettid:
	case SYS_set_tid_address:
		return tos(T_GETPID, 0, 0, 0, 0);
	case SYS_getppid:    return 1;
	case SYS_getuid:
	case SYS_geteuid:
	case SYS_getgid:
	case SYS_getegid:
		return tos(T_LEVEL, 0, 0, 0, 0) == 0 ? 0 : 1000;
	case SYS_getpgid:
	case SYS_getsid:
	case SYS_getpgrp:    return tos(T_GETPID, 0, 0, 0, 0);
	case SYS_setpgid:
	case SYS_setsid:     return 0;
	case SYS_fork:
	case SYS_vfork: {
		long r = tos(T_FORK, 0, 0, 0, 0);
		return r < 0 ? -EAGAIN : r;
	}
	case SYS_execve:     return do_execve((const char *)a, (char *const *)b);
	case SYS_wait4:      return do_wait4(a, (int *)b, c);
	case SYS_kill:
	case SYS_tkill: {
		long r = tos(T_KILL, a, b, 0, 0);
		return r == -2 ? -EPERM : r < 0 ? -ESRCH : 0;
	}
	case SYS_tgkill: {
		long r = tos(T_KILL, b, c, 0, 0);
		return r == -2 ? -EPERM : r < 0 ? -ESRCH : 0;
	}
	case SYS_rt_sigaction:
		return do_sigaction(a, (const struct k_sigaction *)b,
		                    (struct k_sigaction *)c);
	case SYS_rt_sigprocmask:
		if (c) {
			touch((void *)c, 8);
			*(unsigned long *)c = 0;
		}
		return 0;
	case SYS_sigaltstack:
		return 0;

	case SYS_arch_prctl: return do_arch_prctl(a, b);
	case SYS_uname:      return do_uname((struct utsname *)a);
	case SYS_sysinfo:    return do_sysinfo((struct sysinfo *)a);
	case SYS_prlimit64:  return do_prlimit(b, (const struct rlimit *)c,
	                                       (struct rlimit *)d);
	case SYS_getrlimit:  return do_prlimit(a, 0, (struct rlimit *)b);
	case SYS_setrlimit:  return 0;
	case SYS_getrandom:  return do_getrandom((unsigned char *)a, b);
	case SYS_futex:
		/* One thread: nobody to wait for, nobody to wake */
		return (b & 127) == 0 ? -EAGAIN : 0;
	}
	return -ENOSYS;
}
