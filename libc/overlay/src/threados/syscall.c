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
 *   threads  SYS64_CLONE, SYS64_FUTEX and SYS64_EXIT_GROUP are close
 *            to Linux's own; what this file keeps for the whole
 *            program (free mmap ranges, signal handlers) is behind a
 *            lock of its own, because threads now come here at once
 *
 * Sockets, poll and select live in socket.c beside this file.
 *
 * What there is no way to build answers ENOSYS, rather than a Linux
 * number falling on some unrelated ThreadOS call.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/times.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include "syscall.h"
#include "kstat.h"

/* src/threados/socket.c: sockets, poll and select, and descriptors that
   are not the kernel's */
__attribute__((__visibility__("hidden")))
int __tos_net_owns(long n, long fd);
__attribute__((__visibility__("hidden")))
long __tos_net(long n, long a, long b, long c, long d, long e, long f);
__attribute__((__visibility__("hidden")))
long __tos_open_virtual(const char *path);
__attribute__((__visibility__("hidden")))
long __tos_open_dir(const char *path);
__attribute__((__visibility__("hidden")))
long __tos_stat_virtual(const char *path, struct kstat *st);

/* src/threados/unix.c: Unix socket pairs, which are kernel descriptors */
__attribute__((__visibility__("hidden")))
long __tos_unix(long n, long fd, long b, long c, long d, long e, long f);


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
#define T_FUTEX     84
#define T_EXIT_GROUP 85
#define T_GETTGID   86
#define T_NCORES    87
#define T_CORE      42
#define T_FTRUNCATE 88
#define T_FDINODE   89
#define T_GETRANDOM 98
#define T_DISCARD  104
#define T_IPC      105
#define T_FDCTL    106               /* FD_CLOEXEC: (fd, -1 ask / 0 / 1) */
#define T_SIGMASK  110               /* (how, bits, &old) */
#define T_SIGSUSPEND 111
#define T_READDIR2 116               /* (path, len, &cursor, &struct t_dirent2) */
#define T_STAT     117               /* (path, len, &struct t_stat) */
#define T_FDINFO2  118               /* (fd, &struct fdinfo) */
#define T_PATH    1024               /* FS_PATH_MAX: a path with its NUL */
#define T_DT_DIR     2
#define T_DT_LINK    3
/* POSIX's metadata (kernel64/syscall64.h): these answer -errno */
#define T_OPEN2    119               /* (path, len, how, mode) */
#define T_MKDIR2   120               /* (path, len, mode) */
#define T_FDSTAT   121               /* (fd, &struct t_stat) */
#define T_CHMOD    122               /* (path, len | 0: fd, mode) */
#define T_CHOWN    123               /* (path, len | 0: fd, uid, gid) */
#define T_UTIMES   124               /* (path, len | 0: fd, &long[2]) */
#define T_LINK     125
#define T_SYMLINK  126
#define T_READLINK 127               /* (path, len, buf, size) */
#define T_ACCESS   128
#define T_UMASK    129
#define T_FSERRNO  130               /* why the last file call failed */
#define T_STAT_NOFOLLOW 1
#define T_OPEN_EXCL  4
#define T_OPEN_WRITE 8
#define T_UTIME_NOW  (-1L)
#define T_UTIME_OMIT (-2L)

/* SYS64_IPC's operations (kernel/include/ipc.h) used here */
#define IPC_MEMFD 6
#define IPC_MAP   7
#define IPC_UNMAP 8
#define IPC_NULL  9

#define T_FUTEX_WAIT    0
#define T_FUTEX_WAKE    1
#define T_FUTEX_REQUEUE 3

#define T_OPEN_CREATE 1
#define T_OPEN_TRUNC  2

#define K_NONE   0                   /* nothing: 0 and 1 are the console */
#define K_FILE   1
#define K_PIPE_R 2
#define K_PIPE_W 3
#define K_USOCK  4                   /* an end of a socket pair (unix.c) */
#define K_MEMFD  5                   /* shared memory, memfd_create */
#define K_NULL   6                   /* /dev/null */

#define T_FDS    256                 /* FD_MAX in kernel64/task64.c */
/* Where the main thread's stack ends, and how far down it may grow:
   USER64_STACK_TOP and _MAX (kernel64/elf64.h), USER_TOP and
   USER_STACK on ARM64 (kernelarm64/proc.h) */
#if defined(__aarch64__)
#define T_STACK_TOP  (512UL << 30)
#define T_STACK_ROOM (8UL << 20)
#else
#define T_STACK_TOP  0x60000000UL
#define T_STACK_ROOM (4UL << 20)
#endif
#define T_HZ     100                 /* the timer, kernel64/kmain64.c */
#define T_CHUNK  4096                /* the most one read or write moves */
#define AT_EMPTY  0x1000             /* AT_EMPTY_PATH, a GNU name */

#include "tos.h"

/* What this file keeps for the whole program, threads included, is
   changed under this lock. It is held for a few instructions, never
   across a call that waits, so a thread that finds it taken only gives
   up its turn and looks again. */
static volatile int shim_lock;

static void lock(void)
{
	while (__atomic_exchange_n(&shim_lock, 1, __ATOMIC_ACQUIRE))
		tos(T_SLEEP, 0, 0, 0, 0);
}

static void unlock(void)
{
	__atomic_store_n(&shim_lock, 0, __ATOMIC_RELEASE);
}

/* kernel64/syscall64.h's struct fdinfo2_64 and struct stat64k */
struct fdinfo { int kind; unsigned pad; unsigned long size, pos, ino; };
struct t_stat {
	unsigned type, mode;
	unsigned long size, ino;
	unsigned uid, gid, nlink, blksize;
	long atime, mtime, ctime;
	unsigned long blocks;
};

static int fdinfo(int fd, struct fdinfo *fi)
{
	return tos(T_FDINFO2, fd, (long)fi, 0, 0) < 0 ? -1 : 0;
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

/* Close-on-exec is the kernel's to keep: exec is where it acts, and a
   descriptor's flag outlives this program's memory */
static int set_cloexec(int fd, int on)
{
	return tos(T_FDCTL, fd, on ? 1 : 0, 0, 0) < 0 ? -1 : 0;
}

__attribute__((__visibility__("hidden")))
void __tos_set_cloexec(int fd)
{
	set_cloexec(fd, 1);
}

/* (for unix.c, whose sockets are kernel descriptors with flags here) */
__attribute__((__visibility__("hidden")))
int __tos_fd_flags(int fd)
{
	return flags_of(fd);
}

__attribute__((__visibility__("hidden")))
void __tos_fd_set_flags(int fd, int flags)
{
	set_flags(fd, flags);
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
	/* (a pipe is a pair of socket ends in the kernel: one that may not
	   wait reads the way a socket does) */
	if (k == K_USOCK || (k == K_PIPE_R && (flags_of(fd) & O_NONBLOCK)))
		return __tos_unix(SYS_read, fd, (long)buf, n, 0, 0, 0);
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
	if (k == K_USOCK || (k == K_PIPE_W && (flags_of(fd) & O_NONBLOCK)))
		return __tos_unix(SYS_write, fd, (long)buf, n, 0, 0, 0);
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
	if (kind_of(fd) == K_USOCK)
		return __tos_unix(SYS_readv, fd, (long)iov, cnt, 0, 0, 0);
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
	if (kind_of(fd) == K_USOCK)
		return __tos_unix(SYS_writev, fd, (long)iov, cnt, 0, 0, 0);
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
	if (n > T_PATH - 1)
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
	while (*p && n < T_PATH - 1)
		out[n++] = *p++;
	while (n > 1 && out[n - 1] == '/')
		n--;
	out[n] = 0;
	return out;
}

/* What the kernel says is at a path. (This used to go to a directory
   and come back to tell it was one - changing the whole process's
   working directory for a moment, under every other thread's feet.) */
static int kstat_path_how(const char *p, struct t_stat *ks, long how)
{
	touch(ks, sizeof(*ks));
	return tos(T_STAT, (long)p, slen(p), (long)ks, how) < 0 ? -1 : 0;
}

static int kstat_path(const char *p, struct t_stat *ks)
{
	return kstat_path_how(p, ks, 0);
}

/* Why the kernel's last file call for this program failed - an errno,
   or 'otherwise' when it does not say (a kernel before TFS4's) */
static long fs_errno_or(long otherwise)
{
	long e = tos(T_FSERRNO, 0, 0, 0, 0);
	return e > 0 && e < 4096 ? -e : otherwise;
}

static int is_dir(const char *p)
{
	struct t_stat ks;
	if (!strcmp(p, ".") || !strcmp(p, "/"))
		return 1;
	return kstat_path(p, &ks) == 0 && ks.type == T_DT_DIR;
}

static long do_open(const char *path, int flags, unsigned mode)
{
	char buf[T_PATH];
	int e = path_ok(path);
	if (e)
		return e;
	touch(path, slen(path) + 1);
	path = tidy(path, buf);

	if (!strcmp(path, "/dev/null")) {
		long fd = tos(T_IPC, IPC_NULL, 0, 0, 0);
		if (fd < 0)
			return -EMFILE;
		set_flags(fd, flags & O_ACCMODE);
		if (flags & O_CLOEXEC)
			set_cloexec(fd, 1);
		return fd;
	}
	int acc = flags & O_ACCMODE;
	/* O_TMPFILE, an unnamed file in a directory, carries O_DIRECTORY's
	   bit with it: the file system has no unnamed files, and saying so
	   as Linux's file systems without them do sends the program to a
	   named one (SQLite does exactly that) */
	if ((flags & O_TMPFILE) == O_TMPFILE)
		return -EOPNOTSUPP;
	if (flags & O_DIRECTORY)
		return acc != O_RDONLY ? -EISDIR : __tos_open_dir(path);
	if ((flags & O_CREAT) && (flags & O_EXCL)) {
		long fd = tos(T_OPEN, (long)path, slen(path), 0, 0);
		if (fd >= 0) {
			tos(T_CLOSE, fd, 0, 0, 0);
			return -EEXIST;
		}
		if (is_dir(path))
			return -EEXIST;
	}
	/* tmpfile() and mkstemp() make their files in /tmp, which a fresh
	   ThreadOS disk does not have: it is made the first time a file is
	   created there */
	if ((flags & O_CREAT) && !strncmp(path, "/tmp/", 5) && !is_dir("/tmp")) {
		char tmp[] = "/tmp";
		tos(T_MKDIR, (long)tmp, 4, 0, 0);
		tos(T_CHMOD, (long)tmp, 4, 01777, 0);   /* everybody's, as /tmp */
	}
	/* the kernel opens with the mode, the umask and the access asked
	   for, and says why it would not */
	long how2 = (flags & O_CREAT ? T_OPEN_CREATE : 0) |
	            ((flags & O_TRUNC) && acc != O_RDONLY ? T_OPEN_TRUNC : 0) |
	            ((flags & O_CREAT) && (flags & O_EXCL) ? T_OPEN_EXCL : 0) |
	            (acc != O_RDONLY ? T_OPEN_WRITE : 0);
	long fd2 = tos(T_OPEN2, (long)path, slen(path), how2, mode & 07777);
	if (fd2 != -ENOSYS) {
		if (fd2 == -EISDIR && acc == O_RDONLY)
			return __tos_open_dir(path);
		/* a file the system makes up when the disk has none
		   (/etc/resolv.conf, /dev/urandom: socket.c) */
		if (fd2 < 0 && acc == O_RDONLY && !(flags & O_CREAT)) {
			long v = __tos_open_virtual(path);
			if (v >= 0)
				return v;
		}
		if (fd2 < 0)
			return fd2;
		set_flags(fd2, flags & ~(O_CREAT | O_EXCL | O_TRUNC | O_NOCTTY |
		                         O_CLOEXEC));
		if (flags & O_CLOEXEC)
			set_cloexec(fd2, 1);
		return fd2;
	}
	/* a kernel without T_OPEN2 (the phone's): the old way */
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
			return acc == O_RDONLY ? __tos_open_dir(path) : -EISDIR;
		/* a file the system makes up when the disk has none
		   (/etc/resolv.conf, socket.c) */
		if (acc == O_RDONLY && !(flags & O_CREAT)) {
			long v = __tos_open_virtual(path);
			if (v >= 0)
				return v;
		}
		return -ENOENT;
	}
	set_flags(fd, flags & ~(O_CREAT | O_EXCL | O_TRUNC | O_NOCTTY | O_CLOEXEC));
	if (flags & O_CLOEXEC)
		set_cloexec(fd, 1);
	return fd;
}

static void fill_stat(struct kstat *st, unsigned mode, unsigned long size,
                      long ino);

/* What the kernel said of a file, where it keeps POSIX's metadata
   (TFS4); with mode 0 it keeps none (TFS3) and the defaults stand */
static void from_kernel(struct kstat *st, const struct t_stat *ks)
{
	unsigned type = ks->type == T_DT_DIR ? S_IFDIR
	                : ks->type == T_DT_LINK ? S_IFLNK : S_IFREG;
	if (!ks->mode) {
		fill_stat(st, type | (type == S_IFDIR ? 0755 : 0644), ks->size,
		          (long)ks->ino);
		if (type == S_IFDIR)
			st->st_nlink = 2;
		return;
	}
	fill_stat(st, type | ks->mode, ks->size, (long)ks->ino);
	st->st_uid = ks->uid;
	st->st_gid = ks->gid;
	st->st_nlink = ks->nlink;
	st->st_blksize = ks->blksize ? ks->blksize : 4096;
	st->st_blocks = ks->blocks;
	st->st_atime_sec = ks->atime;
	st->st_mtime_sec = ks->mtime;
	st->st_ctime_sec = ks->ctime;
}

static void fill_stat(struct kstat *st, unsigned mode, unsigned long size,
                      long ino)
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

/* A file's inode number, as the kernel gives it by name (T_STAT) and
   by descriptor (T_FDINFO2): the same through every descriptor and by
   its name - a database compares the two to tell its file was not
   replaced under it */
static long file_ino(int fd)
{
	struct fdinfo fi;
	return fdinfo(fd, &fi) == 0 && fi.ino ? (long)fi.ino : 100 + fd;
}

static long do_fstat(int fd, struct kstat *st)
{
	struct fdinfo fi;
	touch(st, sizeof(*st));
	if (fd < 0 || fd >= T_FDS || fdinfo(fd, &fi) < 0)
		return -EBADF;
	switch (fi.kind) {
	case K_FILE: {
		struct t_stat ks;
		touch(&ks, sizeof(ks));
		if (tos(T_FDSTAT, fd, (long)&ks, 0, 0) == 0)
			from_kernel(st, &ks);
		else
			fill_stat(st, S_IFREG | 0644, fi.size, file_ino(fd));
		return 0;
	}
	case K_PIPE_R:
	case K_PIPE_W:
		fill_stat(st, S_IFIFO | 0600, 0, 10 + fd);
		return 0;
	case K_MEMFD:
		fill_stat(st, S_IFREG | 0600, fi.size, 4000 + fd);
		return 0;
	case K_NULL:
		fill_stat(st, S_IFCHR | 0666, 0, 5000);
		st->st_rdev = 0x0103;    /* 1, 3: Linux's numbers for it */
		return 0;
	case K_USOCK:
		return __tos_unix(SYS_fstat, fd, (long)st, 0, 0, 0, 0);
	}
	if (fd <= 2 && (fd == 0 ? is_console_in(0) : is_console_out(fd))) {
		fill_stat(st, S_IFCHR | 0620, 0, 1 + fd);
		st->st_rdev = 0x0400 + fd;
		return 0;
	}
	return -EBADF;
}

static long do_stat_how(const char *path, struct kstat *st, int nofollow)
{
	char buf[T_PATH];
	struct t_stat ks;
	int e = path_ok(path);
	if (e)
		return e;
	touch(path, slen(path) + 1);
	touch(st, sizeof(*st));
	path = tidy(path, buf);
	if (!strcmp(path, "/dev/null")) {
		fill_stat(st, S_IFCHR | 0666, 0, 5000);
		st->st_rdev = 0x0103;
		return 0;
	}
	if (kstat_path_how(path, &ks, nofollow ? T_STAT_NOFOLLOW : 0) == 0) {
		from_kernel(st, &ks);
		return 0;
	}
	long why = fs_errno_or(0);
	if (why == -EACCES || why == -ENOTDIR || why == -ELOOP ||
	    why == -ENAMETOOLONG)
		return why;
	/* a kernel without T_STAT (the phone's): open it and ask */
	long fd = tos(T_OPEN, (long)path, slen(path), 0, 0);
	if (fd >= 0) {
		struct fdinfo fi = { 0 };
		fdinfo(fd, &fi);
		long ino = file_ino(fd);
		tos(T_CLOSE, fd, 0, 0, 0);
		fill_stat(st, S_IFREG | 0644, fi.size, ino);
		return 0;
	}
	if (is_dir(path)) {
		fill_stat(st, S_IFDIR | 0755, 0, 2000);
		st->st_nlink = 2;
		return 0;
	}
	/* what the system makes up: its fonts (socket.c) */
	return __tos_stat_virtual(path, st);
}

static long do_stat(const char *path, struct kstat *st)
{
	return do_stat_how(path, st, 0);
}

static long do_lseek(int fd, long off, int whence)
{
	struct fdinfo fi;
	if (fd < 0 || fd >= T_FDS || fdinfo(fd, &fi) < 0)
		return -EBADF;
	if (fi.kind != K_FILE && fi.kind != K_MEMFD)
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
	case F_DUPFD_CLOEXEC: {
		long r = do_dupfd(fd, arg);
		if (r >= 0 && cmd == F_DUPFD_CLOEXEC)
			set_cloexec(r, 1);
		return r;
	}
	case F_GETFD: {
		long r = tos(T_FDCTL, fd, -1, 0, 0);
		if (r < 0)            /* the console is no descriptor of the kernel's */
			return fd <= 2 ? 0 : -EBADF;
		return r ? FD_CLOEXEC : 0;
	}
	case F_SETFD:
		if (set_cloexec(fd, arg & FD_CLOEXEC) < 0)
			return fd <= 2 ? 0 : -EBADF;
		return 0;
	case F_GETFL:
		return flags_of(fd);
	case F_SETFL:
		set_flags(fd, (flags_of(fd) & O_ACCMODE) |
		              (arg & (O_APPEND | O_NONBLOCK)));
		return 0;
	case F_GETLK:
		/* Nobody else holds a lock - the answer is "unlocked", which
		   is how a lock test says it would succeed */
		touch((void *)arg, sizeof(struct flock));
		((struct flock *)arg)->l_type = F_UNLCK;
		return 0;
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
	if (!tty && kind_of(fd) == K_USOCK)
		return __tos_unix(SYS_ioctl, fd, req, (long)arg, 0, 0, 0);
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
	char buf[T_PATH];
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
	if (nr == T_MKDIR && fs_errno_or(0) == 0)
		return is_dir(path) || do_stat(path, &(struct kstat){0}) == 0
			? -EEXIST : -ENOENT;
	return fs_errno_or(-ENOENT);
}

/* A call on a path that answers -errno itself (T_CHMOD and the rest);
   'otherwise' is what a kernel without it (the phone's) amounts to */
static long path_call(int nr, const char *path, long x, long y,
                      long otherwise)
{
	char buf[T_PATH];
	int e = path_ok(path);
	if (e)
		return e;
	touch(path, slen(path) + 1);
	path = tidy(path, buf);
	long r = tos(nr, (long)path, slen(path), x, y);
	return r == -ENOSYS ? otherwise : r;
}

static long do_mkdir(const char *path, unsigned mode)
{
	long r = path_call(T_MKDIR2, path, mode & 07777, 0, 1);
	return r == 1 ? name_call(T_MKDIR, path) : r;
}

/* unlink() takes no directory and rmdir() nothing else; the kernel's
   call takes either */
static long do_remove(const char *path, int dir)
{
	struct kstat st;
	long e = do_stat_how(path, &st, 1);
	if (e == 0 && dir != S_ISDIR(st.st_mode))
		return dir ? -ENOTDIR : -EISDIR;
	return name_call(T_UNLINK, path);
}

/* struct timespec[2] as utimensat(2) takes it into the kernel's pair of
   seconds, "now" or "leave it" */
static long do_utimens(int fd, const char *path, const struct timespec *t)
{
	long out[2];
	for (int i = 0; i < 2; i++) {
		if (!t || t[i].tv_nsec == UTIME_NOW)
			out[i] = T_UTIME_NOW;
		else if (t[i].tv_nsec == UTIME_OMIT)
			out[i] = T_UTIME_OMIT;
		else if (t[i].tv_nsec < 0 || t[i].tv_nsec > 999999999 ||
		         t[i].tv_sec < 0)
			return -EINVAL;
		else
			out[i] = t[i].tv_sec;
	}
	touch(out, sizeof(out));
	if (!path) {
		long r = tos(T_UTIMES, fd, 0, (long)out, 0);
		return r == -ENOSYS ? 0 : r;
	}
	return path_call(T_UTIMES, path, (long)out, 0, 0);
}

static long do_readlink(const char *path, char *buf, long size)
{
	if (size <= 0)
		return -EINVAL;
	touch(buf, size);
	return path_call(T_READLINK, path, (long)buf, size, -EINVAL);
}

static long do_access(const char *path, int mode)
{
	long r = path_call(T_ACCESS, path, mode & 7, 0, 1);
	return r == 1 ? do_stat(path, &(struct kstat){0}) : r;
}

static long do_rename(const char *from, const char *to)
{
	char a[T_PATH], b[T_PATH];
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
	return r == -2 ? -EACCES : r < 0 ? fs_errno_or(-ENOENT) : 0;
}

/* link() and symlink(): two paths into the kernel (for symlink the
   first is the target, kept as it is, not tidied) */
static long do_link(const char *a, const char *b, int nr)
{
	char ba[T_PATH], bb[T_PATH];
	int e = path_ok(a);
	if (!e)
		e = path_ok(b);
	if (e)
		return e;
	touch(a, slen(a) + 1);
	touch(b, slen(b) + 1);
	if (nr == T_LINK)
		a = tidy(a, ba);
	b = tidy(b, bb);
	long r = tos(nr, (long)a, slen(a), (long)b, slen(b));
	return r == -ENOSYS ? -EPERM : r;
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

/* Pages given back to the system, reading as zeros from now on - as
   Linux promises after madvise(MADV_DONTNEED), and WebKit relies on
   when it decommits memory and later takes it again. The kernel drops
   what is this program's alone; what it shares (a page still shared
   with a forked parent, say) is cleared here instead. Clearing it all
   here would bring back every page of a big reservation never
   touched. (munmap keeps to filing the range and clearing it when it
   is used again: given back at once, it set off a race in programs
   with threads that has not been found yet.) */
static void scrub(unsigned long at, unsigned long len)
{
	if (tos(T_DISCARD, at, len, 0, 0) != 0)
		memset((void *)at, 0, len);
}

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

/* The stacks of detached threads that ended themselves (__unmapself):
   each is free once its thread is gone, and not a moment sooner, since
   the thread stands on it while it files it */
#define LATER 64
static struct { unsigned long at, len; long tid; } later[LATER];
static int nlater;

static void collect_later(void)
{
	for (int i = 0; i < nlater; ) {
		/* -3 = still running; a thread leaves no exit record, so
		   anything else means it is gone */
		if (tos(T_TRYWAIT, later[i].tid, 0, 0, 0) != -3) {
			hole_add(later[i].at, later[i].len);
			later[i] = later[--nlater];
		} else {
			i++;
		}
	}
}

__attribute__((__visibility__("hidden"), __noreturn__))
void __tos_unmapself(void *base, size_t len)
{
	lock();
	if (nlater < LATER) {
		later[nlater].at = (unsigned long)base;
		later[nlater].len = (len + 4095) & ~4095UL;
		later[nlater].tid = tos(T_GETPID, 0, 0, 0, 0);
		nlater++;
	}
	/* (with no room the stack is simply never reused) */
	unlock();
	for (;;)
		tos(T_EXIT, 0, 0, 0, 0);
}

static long do_mmap_locked(unsigned long len)
{
	collect_later();
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

/* The memfds mapped MAP_SHARED: their pages are the memfd's, and
   munmap has to give them back to it (IPC_UNMAP) - filing the range
   for reuse, as for anonymous memory, would hand the next mmap pages
   another process still draws on. Copied by fork with the rest of
   memory, as the mappings are. */
#define SHARED 256
static struct { unsigned long at, len; } shared[SHARED];
static int nshared;

static long map_memfd(int fd, unsigned long len, int flags, long off)
{
	if (off < 0 || (off & 4095))
		return -EINVAL;
	if ((flags & 3) == MAP_PRIVATE) {
		/* a copy of what is there now, the program's own from here on */
		lock();
		long at = do_mmap_locked(len);
		unlock();
		if (at < 0)
			return at;
		long was = do_lseek(fd, 0, SEEK_CUR);
		do_lseek(fd, off, SEEK_SET);
		for (unsigned long done = 0; done < len; ) {
			long r = tos(T_READ, fd, at + done, len - done > T_CHUNK
			             ? T_CHUNK : len - done, 0);
			if (r <= 0)
				break;               /* past its end: zeros */
			done += r;
		}
		do_lseek(fd, was, SEEK_SET);
		return at;
	}
	long at = tos(T_IPC, IPC_MAP, fd, off, len);
	if (at < 0)
		return at == -EINVAL ? -ENXIO : -ENOMEM;   /* past its end */
	lock();
	if (nshared == SHARED) {
		unlock();
		tos(T_IPC, IPC_UNMAP, at, len, 0);
		return -ENOMEM;
	}
	shared[nshared].at = at;
	shared[nshared].len = len;
	nshared++;
	unlock();
	return at;
}

static long do_munmap(unsigned long addr, unsigned long len);

/* A private mapping of a file - a disk file or one of the made-up ones
   (socket.c: the system's fonts, the CA bundle) - is a copy of it:
   nothing written through a private mapping reaches the file, so the
   only difference a program could see is that it does not follow later
   writes by others, which none of the libraries here wait for. Skia
   checks a font is usable by mapping it, and with every map of a file
   refused it took no font of the system's for its own and drew all
   text with fallbacks: right letters, but metrics of nought, and so
   list markers with no height. A shared mapping of a file is still
   refused; there is no page cache here to share. */
static long map_copy(int fd, unsigned long len, long off)
{
	if (off < 0 || (off & 4095))
		return -EINVAL;
	int made_up = __tos_net_owns(SYS_pread64, fd);
	if (!made_up && kind_of(fd) != K_FILE)
		return kind_of(fd) < 0 ? -EBADF : -ENODEV;
	lock();
	long at = do_mmap_locked(len);
	unlock();
	if (at < 0)
		return at;
	for (unsigned long done = 0; done < len; ) {
		unsigned long want = len - done > T_CHUNK ? T_CHUNK : len - done;
		long r;
		if (made_up) {
			r = __tos_net(SYS_pread64, fd, at + done, want,
			              off + done, 0, 0);
		} else {
			long was = do_lseek(fd, 0, SEEK_CUR);
			r = do_lseek(fd, off + done, SEEK_SET);
			if (r >= 0)
				r = do_read(fd, (void *)(at + done), want);
			do_lseek(fd, was, SEEK_SET);
		}
		if (r < 0 && !done) {
			do_munmap(at, len);      /* not readable: say why */
			return r;
		}
		if (r <= 0)
			break;                   /* past its end: zeros */
		done += r;
	}
	return at;
}

static long do_mmap(unsigned long addr, unsigned long len, int prot,
                    int flags, int fd, long off)
{
	(void)addr; (void)prot;
	if (!len)
		return -EINVAL;
	if (flags & MAP_FIXED)
		return -ENOMEM;
	len = (len + 4095) & ~4095UL;
	if (!(flags & MAP_ANONYMOUS)) {
		if (kind_of(fd) == K_MEMFD)
			return map_memfd(fd, len, flags, off);
		return (flags & 3) == MAP_PRIVATE ? map_copy(fd, len, off)
		                                  : -ENODEV;
	}
	lock();
	long r = do_mmap_locked(len);
	unlock();
	return r;
}

/* What of [at, end) is memfd pages goes back to the memfds. Called
   with the lock held; IPC_UNMAP never waits. */
static void unmap_shared(unsigned long at, unsigned long end)
{
	for (int i = 0; i < nshared; ) {
		unsigned long s = shared[i].at, e = s + shared[i].len;
		unsigned long lo = at > s ? at : s, hi = end < e ? end : e;
		if (lo >= hi) {
			i++;
			continue;
		}
		tos(T_IPC, IPC_UNMAP, lo, hi - lo, 0);
		if (lo == s && hi == e) {
			shared[i] = shared[--nshared];
			continue;            /* look at what moved here */
		}
		if (lo == s) {
			shared[i].at = hi;
			shared[i].len = e - hi;
		} else {
			shared[i].len = lo - s;
			if (hi < e && nshared < SHARED) {
				shared[nshared].at = hi;
				shared[nshared].len = e - hi;
				nshared++;
			}
			/* (with no room the tail is forgotten: munmap then only
			   files it, and its pages stay until the program ends) */
		}
		i++;
	}
}

static long do_munmap(unsigned long addr, unsigned long len)
{
	if (addr & 4095)
		return -EINVAL;
	len = (len + 4095) & ~4095UL;
	if (len) {
		lock();
		if (nshared)
			unmap_shared(addr, addr + len);
		hole_add(addr, len);
		unlock();
	}
	return 0;
}

/* --- Futexes ------------------------------------------------------- */

static long do_futex(int *addr, int op, int val, const struct timespec *to,
                     int *addr2, int val3)
{
	(void)val3;
	/* FUTEX_PRIVATE_FLAG changes nothing: every futex here is the
	   program's own. FUTEX_CLOCK_REALTIME only matters to the
	   absolute waits musl does not use. */
	op &= ~(128 | 256);
	switch (op) {
	case 0: {                        /* FUTEX_WAIT, relative timeout */
		long ms = 0;
		if (to) {
			touch(to, sizeof(*to));
			if (to->tv_sec < 0 || to->tv_nsec < 0 ||
			    to->tv_nsec >= 1000000000L)
				return -EINVAL;
			if (!to->tv_sec && !to->tv_nsec)
				return -ETIMEDOUT;
			if (to->tv_sec > 0x7FFFFFFF / 1000 - 1)
				ms = 0;          /* longer than the kernel counts: forever */
			else {
				ms = to->tv_sec * 1000 + (to->tv_nsec + 999999) / 1000000;
				if (!ms)
					ms = 1;
			}
		}
		touch(addr, 4);
		return tos(T_FUTEX, (long)addr, T_FUTEX_WAIT, (unsigned)val, ms);
	}
	case 1:                          /* FUTEX_WAKE */
		return tos(T_FUTEX, (long)addr, T_FUTEX_WAKE, val < 0 ? 0x7FFFFFFF : val, 0);
	case 3: {                        /* FUTEX_REQUEUE: the timeout is a count */
		long move = (long)to;
		if (val < 0 || move < 0)
			return -EINVAL;
		if (move > 0x7FFFFFFF)
			move = 0x7FFFFFFF;
		return tos(T_FUTEX, (long)addr, T_FUTEX_REQUEUE,
		           (long)(unsigned)val | move << 32, (long)addr2);
	}
	}
	return -ENOSYS;
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

/* posix_spawn's child (thread/x86_64/clone.s): musl makes it with
   clone(CLONE_VM | CLONE_VFORK), a child in the parent's memory that
   sets its descriptors up and execs. A fork runs the same code on its
   own copy - nothing a vfork child may do is out of a fork child's
   reach - and the parent learns whether exec worked from the
   close-on-exec pipe musl gives the child, as it would after a vfork. */
__attribute__((__visibility__("hidden")))
long __tos_clone_process(int (*func)(void *), void *arg)
{
	long pid = tos(T_FORK, 0, 0, 0, 0);
	if (pid == 0)
		tos(T_EXIT_GROUP, func(arg) & 0xff, 0, 0, 0);
	return pid < 0 ? -EAGAIN : pid;
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

/* A thread's signal mask is the kernel's (SYS64_SIGMASK): it decides
   which thread a signal for the program interrupts, and it holds one
   back across a wait */
static long kernel_mask(int how, unsigned long set, unsigned long *old)
{
	return tos(T_SIGMASK, how, (long)set, (long)old, 0);
}

/* The kernel calls it as Linux calls a handler - the signal, its
   siginfo_t and the ucontext_t of the frame it was interrupted in - so
   an SA_SIGINFO handler gets what it asks for */
static void on_signal(int sig, siginfo_t *info, void *uc)
{
	struct k_sigaction a = sig > 0 && sig < 65 ? actions[sig] :
	                       (struct k_sigaction){ 0 };
	void (*h)(int) = a.handler;
	if (h == SIG_IGN)
		return;
	if (h == SIG_DFL || !h) {
		/* What the kernel would have done had nobody asked */
		if (sig == SIGCHLD || sig == SIGWINCH || sig == SIGURG ||
		    sig == SIGCONT)
			return;
		/* A fault of the program's own (si_code above 0): no handler
		   for the kernel any more, and back to the instruction, which
		   faults again - and the kernel ends the program saying where,
		   as it does one that never had a handler */
		if (info && info->si_code > 0 &&
		    (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL ||
		     sig == SIGTRAP || sig == SIGFPE)) {
			tos(T_SIGNAL, 0, 0, 0, 0);
			return;
		}
		tos(T_EXIT_GROUP, 128 + sig, 0, 0, 0);
	}
	if (a.flags & SA_RESETHAND)
		actions[sig].handler = SIG_DFL;
	/* sa_mask on top of the signal itself, which the kernel holds back;
	   the mask from before comes back with the frame */
	unsigned long more = a.mask[0] | (unsigned long)a.mask[1] << 32;
	if (more)
		kernel_mask(SIG_BLOCK, more, 0);
	if (a.flags & SA_NODEFER)
		kernel_mask(SIG_UNBLOCK, 1UL << (sig - 1), 0);
	if (a.flags & SA_SIGINFO) {
		((void (*)(int, siginfo_t *, void *))h)(sig, info, uc);
		return;
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
	lock();
	actions[sig] = *act;
	if (!handler_installed && act->handler != SIG_DFL) {
		/* The return from it is musl's own __restore_rt, which says
		   SYS64_SIGRETURN (src/signal/x86_64/restore.s). A thread
		   created later gets the kernel's handler with the rest of
		   the process. */
		tos(T_SIGNAL, 0, (long)on_signal,
		    (long)(act->restorer ? act->restorer : 0), 0);
		handler_installed = 1;
	}
	unlock();
	return 0;
}

/* The kernel's generator (kernel/random.c), which serves a page at a
   time. Linux answers whole for any size a program asks in practice,
   and code that draws a key in one call counts on it. */
static long do_getrandom(unsigned char *buf, size_t len, unsigned flags)
{
	size_t done = 0;
	while (done < len) {
		size_t n = len - done > T_CHUNK ? T_CHUNK : len - done;
		touch(buf + done, n);
		long r = tos(T_GETRANDOM, (long)(buf + done), (long)n,
		             (flags & GRND_NONBLOCK) ? 1 : 0, 0);
		if (r == -2)
			return done ? (long)done : -EAGAIN;
		if (r < 0)
			return done ? (long)done : -EFAULT;
		done += (size_t)r;
	}
	return (long)done;
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
#if defined(__aarch64__)
	copy_name(u->version, "ThreadOS ARM64");
	copy_name(u->machine, "aarch64");
#else
	copy_name(u->version, "ThreadOS 64");
	copy_name(u->machine, "x86_64");
#endif
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
			old->rlim_cur = old->rlim_max = T_STACK_ROOM;
		if (res == RLIMIT_NOFILE)
			old->rlim_cur = old->rlim_max = T_FDS;
	}
	return 0;
}

/* x86-64 only: on ARM64 the thread pointer is TPIDR_EL0, which a
   program sets itself (src/thread/aarch64/__set_thread_area.s) */
#ifdef SYS_arch_prctl
static long do_arch_prctl(int code, unsigned long addr)
{
	if (code == 0x1002) {            /* ARCH_SET_FS */
		touch((void *)addr, 8);
		if (tos(T_SET_FS, addr, 0, 0, 0) < 0)
			return -EPERM;
		return 0;
	}
	if (code == 0x1003) {            /* ARCH_GET_FS */
		/* Each thread has its own; musl's thread block starts with
		   a pointer to itself, so the base is what FS:0 holds */
		unsigned long fs;
		__asm__ ("mov %%fs:0,%0" : "=r"(fs));
		touch((void *)addr, 8);
		*(unsigned long *)addr = fs;
		return 0;
	}
	return -EINVAL;
}
#endif

static long do_pipe2(int *fds, long flags)
{
	touch(fds, 2 * sizeof(int));
	if (tos(T_PIPE, (long)fds, 0, 0, 0) < 0)
		return -EMFILE;
	int nb = flags & O_NONBLOCK ? O_NONBLOCK : 0;
	set_flags(fds[0], O_RDONLY | nb);
	set_flags(fds[1], O_WRONLY | nb);
	if (flags & O_CLOEXEC) {
		set_cloexec(fds[0], 1);
		set_cloexec(fds[1], 1);
	}
	return 0;
}

static long do_fork(void)
{
	long r = tos(T_FORK, 0, 0, 0, 0);
	return r < 0 ? -EAGAIN : r;
}

/* --- The switch ---------------------------------------------------- */

__attribute__((__visibility__("hidden")))
long __tos_syscall(long n, long a, long b, long c, long d, long e, long f)
{
	if (__tos_net_owns(n, a))
		return __tos_net(n, a, b, c, d, e, f);
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
	case SYS_openat:
		if (a != AT_FDCWD && ((const char *)b)[0] != '/')
			return -ENOSYS;
		return do_open((const char *)b, c, d);
	case SYS_close:      return do_close(a);
	case SYS_lseek:      return do_lseek(a, b, c);
	case SYS_fstat:      return do_fstat(a, (struct kstat *)b);
	case SYS_newfstatat:
		if ((d & AT_EMPTY) && !*(const char *)b)
			return do_fstat(a, (struct kstat *)c);
		if (a != AT_FDCWD && ((const char *)b)[0] != '/')
			return -ENOSYS;
		return do_stat_how((const char *)b, (struct kstat *)c,
		                   (d & AT_SYMLINK_NOFOLLOW) != 0);
	case SYS_statx:      return -ENOSYS;   /* musl falls back to fstatat */
	case SYS_faccessat:
	case SYS_faccessat2:
		if (a != AT_FDCWD && ((const char *)b)[0] != '/')
			return -ENOSYS;
		return do_access((const char *)b, c);
	case SYS_ioctl:      return do_ioctl(a, b, (void *)c);
	case SYS_fcntl:      return do_fcntl(a, b, c);
	case SYS_dup:        return do_dupfd(a, 3);
	case SYS_dup3: {
		if (a == b)
			return -EINVAL;
		long r = do_dup2(a, b);
		if (r >= 0 && (c & O_CLOEXEC))
			set_cloexec(r, 1);
		return r;
	}
	case SYS_pipe2:      return do_pipe2((int *)a, b);
	case SYS_memfd_create: {
		/* MFD_CLOEXEC and MFD_ALLOW_SEALING: nothing to do (no seals
		   are ever added, so allowing them changes nothing) */
		if (b & ~3L)
			return -EINVAL;
		long fd = tos(T_IPC, IPC_MEMFD, 0, 0, 0);
		if (fd < 0)
			return fd == -ENOMEM ? -ENFILE : fd;
		set_flags(fd, O_RDWR);
		if (b & 1)                   /* MFD_CLOEXEC */
			set_cloexec(fd, 1);
		return fd;
	}
	case SYS_fsync:
	case SYS_fdatasync:
	case SYS_sync:       return 0;
	case SYS_ftruncate: {
		struct fdinfo fi;
		if (a < 0 || a >= T_FDS || fdinfo(a, &fi) < 0 ||
		    (fi.kind != K_FILE && fi.kind != K_MEMFD))
			return -EBADF;
		if (b < 0)
			return -EINVAL;
		if (fi.kind == K_MEMFD) {
			long r = tos(T_FTRUNCATE, a, b, 0, 0);
			return r < 0 ? (r == -ENOMEM ? -ENOMEM : -EINVAL) : 0;
		}
		long r = tos(T_FTRUNCATE, a, b, 0, 0);
		return r == -2 ? -EACCES : r < 0 ? -EIO : 0;
	}
	case SYS_getcwd:     return do_getcwd((char *)a, b);
	case SYS_chdir:      return name_call(T_CHDIR, (const char *)a);
	case SYS_mkdirat:    return do_mkdir((const char *)b, c);
	case SYS_unlinkat:
		return do_remove((const char *)b, (c & AT_REMOVEDIR) != 0);
	case SYS_renameat:
	case SYS_renameat2:  return do_rename((const char *)b, (const char *)d);
	case SYS_readlinkat:
		return do_readlink((const char *)b, (char *)c, d);
	case SYS_umask: {
		long old = tos(T_UMASK, a & 0777, 0, 0, 0);
		return old < 0 ? 022 : old;
	}
	case SYS_fchmod: {
		struct fdinfo fi;
		if (a < 0 || a >= T_FDS || fdinfo(a, &fi) < 0)
			return -EBADF;
		if (fi.kind != K_FILE)
			return 0;
		long r = tos(T_CHMOD, a, 0, b & 07777, 0);
		return r == -ENOSYS ? 0 : r;
	}
	case SYS_fchmodat:
		if (a != AT_FDCWD && ((const char *)b)[0] != '/')
			return -ENOSYS;
		return path_call(T_CHMOD, (const char *)b, c & 07777, 0, 0);
	case SYS_fchownat:
		if (a != AT_FDCWD && ((const char *)b)[0] != '/')
			return -ENOSYS;
		return path_call(T_CHOWN, (const char *)b, (int)c, (int)d, 0);
	case SYS_fchown: {
		long r = tos(T_CHOWN, a, 0, (int)b, (int)c);
		return r == -ENOSYS ? 0 : r;
	}
	case SYS_utimensat:
		if (b && a != AT_FDCWD && ((const char *)b)[0] != '/')
			return -ENOSYS;
		return do_utimens(a, (const char *)b, (const struct timespec *)c);
	case SYS_linkat:
		if ((a != AT_FDCWD && ((const char *)b)[0] != '/') ||
		    (c != AT_FDCWD && ((const char *)d)[0] != '/'))
			return -ENOSYS;
		return do_link((const char *)b, (const char *)d, T_LINK);
	case SYS_symlinkat:
		if (b != AT_FDCWD && ((const char *)c)[0] != '/')
			return -ENOSYS;
		return do_link((const char *)a, (const char *)c, T_SYMLINK);

	case SYS_brk:        return do_brk(a);
	case SYS_mmap:       return do_mmap(a, b, c, d, e, f);
	case SYS_munmap:     return do_munmap(a, b);
	case SYS_mremap:
		/* With MREMAP_MAYMOVE it is musl's realloc, told to copy
		   instead. Without, it is musl's pthread_getattr_np feeling
		   for the bottom of the main thread's stack a page at a time
		   (WebKit asks where its stack ends): "in use" (ENOMEM)
		   while the page is inside the room the stack may grow into
		   (T_STACK_TOP and T_STACK_ROOM), "nothing there"
		   (EFAULT) below it */
		if (d & 1)
			return -ENOMEM;
		return (unsigned long)a >= T_STACK_TOP - T_STACK_ROOM &&
		       (unsigned long)a < T_STACK_TOP ? -ENOMEM : -EFAULT;
	case SYS_madvise:
		/* MADV_DONTNEED and MADV_FREE: the pages may go, and read as
		   zeros afterwards; the rest are hints */
		if ((c == 4 || c == 8) && b && !(a & 4095))
			scrub(a, (b + 4095) & ~4095UL);
		return 0;
	case SYS_mprotect:
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

	case SYS_exit:               /* this thread */
		for (;;)
			tos(T_EXIT, a, 0, 0, 0);
	case SYS_exit_group:         /* the whole program */
		for (;;)
			tos(T_EXIT_GROUP, a, 0, 0, 0);
	case SYS_getpid:
		return tos(T_GETTGID, 0, 0, 0, 0);
	case SYS_gettid:
	case SYS_set_tid_address:
		/* the main thread's clear_tid is never needed: when it
		   ends, so does the program */
		return tos(T_GETPID, 0, 0, 0, 0);
	case SYS_getppid:    return 1;
	case SYS_getuid:
	case SYS_geteuid:
	case SYS_getgid:
	case SYS_getegid:
		return tos(T_LEVEL, 0, 0, 0, 0) == 0 ? 0 : 1000;
	case SYS_getpgid:
	case SYS_getsid:     return tos(T_GETTGID, 0, 0, 0, 0);
	case SYS_setpgid:
	case SYS_setsid:     return 0;
	case SYS_clone:
		/* Only as musl's fork asks where Linux has no fork call
		   (AArch64): a child with its own copy of everything, and
		   SIGCHLD for its end. Threads come by __clone. */
		if (a != SIGCHLD || b)
			return -ENOSYS;
		return do_fork();
	case SYS_execve:     return do_execve((const char *)a, (char *const *)b);
	case SYS_wait4:      return do_wait4(a, (int *)b, c);
	case SYS_kill:
	case SYS_tkill: {
		/* (the third argument: a thread's number, not a program's) */
		long r = tos(T_KILL, a, b, n == SYS_tkill, 0);
		return r == -2 ? -EPERM : r < 0 ? -ESRCH : 0;
	}
	case SYS_tgkill: {
		long r = tos(T_KILL, b, c, 1, 0);
		return r == -2 ? -EPERM : r < 0 ? -ESRCH : 0;
	}
	case SYS_rt_sigaction:
		return do_sigaction(a, (const struct k_sigaction *)b,
		                    (struct k_sigaction *)c);
	case SYS_rt_sigprocmask: {
		unsigned long old = 0;
		int how = b ? (int)a : 3;              /* 3: only asking */
		if (how < 0 || how > 3)
			return -EINVAL;
		if (kernel_mask(how, b ? *(const unsigned long *)b : 0,
		                c ? &old : 0) < 0)
			return -EINVAL;
		if (c) {
			touch((void *)c, 8);
			*(unsigned long *)c = old;
		}
		return 0;
	}
	case SYS_rt_sigsuspend:
		/* Ends once a handler has run: always "interrupted" */
		tos(T_SIGSUSPEND, *(const unsigned long *)a, 0, 0, 0);
		return -EINTR;
	case SYS_sigaltstack:
		return 0;

	case SYS_uname:      return do_uname((struct utsname *)a);
	case SYS_sysinfo:    return do_sysinfo((struct sysinfo *)a);
	case SYS_prlimit64:  return do_prlimit(b, (const struct rlimit *)c,
	                                       (struct rlimit *)d);
	case SYS_getrlimit:  return do_prlimit(a, 0, (struct rlimit *)b);
	case SYS_setrlimit:  return 0;
	case SYS_getrandom:  return do_getrandom((unsigned char *)a, b, c);
	case SYS_getcpu:
		if (a) {
			touch((void *)a, sizeof(unsigned));
			*(unsigned *)a = tos(T_CORE, 0, 0, 0, 0);
		}
		if (b) {
			touch((void *)b, sizeof(unsigned));
			*(unsigned *)b = 0;
		}
		return 0;
	case SYS_sched_getaffinity: {
		/* Every thread may run on every core: what sysconf counts
		   as the processors online */
		long n = tos(T_NCORES, 0, 0, 0, 0);
		size_t bytes = (n + 63) / 64 * 8;
		if (n < 1 || (size_t)b < bytes)
			return -EINVAL;
		touch((void *)c, bytes);
		memset((void *)c, 0, bytes);
		for (long i = 0; i < n; i++)
			((unsigned long *)c)[i / 64] |= 1UL << (i % 64);
		return bytes;
	}
	case SYS_futex:
		return do_futex((int *)a, b, c, (const struct timespec *)d,
		                (int *)e, f);

#ifdef SYS_open
	/* Linux's older calls, which the PC has and AArch64 does not:
	   there musl uses the *at ones above for all of them */
	case SYS_open:       return do_open((const char *)a, b, c);
	case SYS_stat:       return do_stat((const char *)a, (struct kstat *)b);
	case SYS_lstat:
		return do_stat_how((const char *)a, (struct kstat *)b, 1);
	case SYS_access:     return do_access((const char *)a, b);
	case SYS_dup2:       return do_dup2(a, b);
	case SYS_pipe:       return do_pipe2((int *)a, 0);
	case SYS_mkdir:      return do_mkdir((const char *)a, b);
	case SYS_rmdir:      return do_remove((const char *)a, 1);
	case SYS_unlink:     return do_remove((const char *)a, 0);
	case SYS_rename:     return do_rename((const char *)a, (const char *)b);
	case SYS_readlink:
		return do_readlink((const char *)a, (char *)b, c);
	case SYS_chmod:
		return path_call(T_CHMOD, (const char *)a, b & 07777, 0, 0);
	case SYS_chown:
	case SYS_lchown:
		return path_call(T_CHOWN, (const char *)a, (int)b, (int)c, 0);
	case SYS_link:
		return do_link((const char *)a, (const char *)b, T_LINK);
	case SYS_symlink:
		return do_link((const char *)a, (const char *)b, T_SYMLINK);
	case SYS_time: {
		struct timespec ts;
		wall_clock(&ts);
		if (a)
			*(long *)a = ts.tv_sec;
		return ts.tv_sec;
	}
	case SYS_getpgrp:    return tos(T_GETTGID, 0, 0, 0, 0);
	case SYS_fork:
	case SYS_vfork:      return do_fork();
	case SYS_arch_prctl: return do_arch_prctl(a, b);
#endif
	}
	return -ENOSYS;
}
