/* libctest - the C library, seen from a program
 *
 * Ordinary portable C, nothing of ThreadOS in it: the same source is
 * built against musl for ThreadOS and against the host's own C library
 * on Linux, and the test (scripts/tests/t_libc.py) compares the two
 * outputs line for line. So every line here is something both must
 * agree on - number formatting, strtod, the heap, files, errno,
 * setjmp, sorting - and what cannot be the same on two machines (a
 * process number, the date) is printed as a property, or on a line
 * starting with '~' that the test reads on its own terms.
 *
 * "libctest memory FILE" is the part a system with no disk to write
 * can run - the ARM64 kernel so far (scripts/tests/t_arm64libc.py) -
 * and FILE one it can read: every section but those that write files,
 * a file read instead, and the program started again by posix_spawn
 * ("libctest child ...", its output through a pipe).
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static int cmp_int(const void *a, const void *b)
{
	int x = *(const int *)a, y = *(const int *)b;
	return (x > y) - (x < y);
}

static jmp_buf jb;

static void deep(int n)
{
	if (!n)
		longjmp(jb, 42);
	deep(n - 1);
}

static void bye(void)
{
	printf("atexit: ran\n");
}

static void numbers(void)
{
	printf("printf: %d %i %u %x %X %o %c %s %%\n", -42, 7, 3000000000u,
	       0xbeef, 0xbeef, 8, 'Q', "str");
	printf("width: [%5d] [%-5d] [%05d] [%+d] [%.3s]\n", 42, 42, 42, 42,
	       "abcdef");
	printf("long: %ld %lld %zu\n", -1234567890123L, 9007199254740993LL,
	       (size_t)123456789);
	printf("double: %g %g %g %g\n", 0.1 + 0.2, 1e21, 1.0 / 3, -0.0);
	printf("exact: %.17g %.17g %.17g\n", 0.1 + 0.2, sqrt(2.0), 1.0 / 7);
	printf("fixed: %.2f %.0f %.10f %f\n", 2.675, 0.5, M_PI, 1e15);
	printf("expo: %e %.3e %E\n", 123456.789, 0.000123456, 6.02214076e23);
	printf("hexfloat: %a %a\n", 1.0, 0.1);
	printf("special: %f %f %f\n", INFINITY, -INFINITY, NAN);
	printf("strtod: %.17g %.17g %.17g\n", strtod("3.14159", 0),
	       strtod("1e-310", 0), strtod("0x1.8p1", 0));
	printf("strtol: %ld %ld %lu\n", strtol("-0x7f", 0, 16),
	       strtol("0755", 0, 0), strtoul("18446744073709551615", 0, 10));
	printf("atoi: %d %d\n", atoi("  123abc"), atoi("-99"));
	char buf[64];
	int n = snprintf(buf, 8, "%s", "truncated text");
	printf("snprintf: %d [%s]\n", n, buf);
	printf("math: %.15g %.15g %.15g %.15g\n", sin(1.0), exp(1.0),
	       pow(2.0, 0.5), log(10.0));
	printf("math2: %.15g %.15g %.15g %g %g\n", atan2(1.0, 1.0),
	       cbrt(27.0), hypot(3.0, 4.0), floor(-2.5), round(2.5));
	printf("fmod: %g %g\n", fmod(10.0, 3.0), remainder(10.0, 3.0));
}

static void strings(void)
{
	char a[32] = "hello";
	strcat(a, ", world");
	printf("strcat: %s %zu\n", a, strlen(a));
	printf("strcmp: %d %d %d\n", strcmp("a", "b") < 0, strcmp("b", "a") > 0,
	       strcmp("same", "same"));
	printf("strstr: %s\n", strstr("needle in a haystack", "hay"));
	printf("strchr: %s %s\n", strchr("a.b.c", '.'), strrchr("a.b.c", '.'));
	char tok[] = "one,two,,three";
	char *save, *t = strtok_r(tok, ",", &save);
	printf("strtok:");
	while (t) {
		printf(" [%s]", t);
		t = strtok_r(0, ",", &save);
	}
	printf("\n");
	printf("memcmp: %d\n", memcmp("abc", "abd", 3) < 0);
	char *d = strdup("duplicate");
	printf("strdup: %s\n", d);
	free(d);
}

static void heap(void)
{
	/* Small, large (past the point where malloc maps instead of
	   growing the heap), freed and asked for again */
	unsigned long sum = 0;
	for (int round = 0; round < 3; round++) {
		char *big = malloc(3 << 20);
		if (!big) {
			printf("malloc: big failed\n");
			return;
		}
		memset(big, round + 1, 3 << 20);
		sum += (unsigned char)big[(3 << 20) - 1];
		free(big);
	}
	int *v = 0;
	size_t cap = 0;
	for (int i = 0; i < 100000; i++) {
		if ((size_t)i == cap) {
			cap = cap ? cap * 2 : 16;
			v = realloc(v, cap * sizeof(*v));
		}
		v[i] = (i * 7919) % 100003;
	}
	long long total = 0;
	for (int i = 0; i < 100000; i++)
		total += v[i];
	qsort(v, 100000, sizeof(*v), cmp_int);
	int sorted = 1;
	for (int i = 1; i < 100000; i++)
		if (v[i - 1] > v[i])
			sorted = 0;
	printf("heap: big blocks %lu, total %lld, sorted %d, min %d max %d\n",
	       sum, total, sorted, v[0], v[99999]);
	free(v);
	char **many = malloc(5000 * sizeof(*many));
	for (int i = 0; i < 5000; i++) {
		many[i] = malloc(1 + i % 200);
		memset(many[i], 'x', 1 + i % 200);
	}
	for (int i = 0; i < 5000; i += 2)
		free(many[i]);
	for (int i = 0; i < 5000; i += 2)
		many[i] = calloc(1, 64);
	int zero = 1;
	for (int i = 0; i < 5000; i += 2)
		for (int k = 0; k < 64; k++)
			if (many[i][k])
				zero = 0;
	for (int i = 0; i < 5000; i++)
		free(many[i]);
	free(many);
	printf("calloc: zeroed %d\n", zero);
}

static void files(void)
{
	const char *name = "libc_t.txt";
	unlink(name);
	unlink("libc_t2.txt");

	FILE *f = fopen(name, "w");
	if (!f) {
		printf("fopen w: %s\n", strerror(errno));
		return;
	}
	for (int i = 1; i <= 300; i++)
		fprintf(f, "line %d: %d\n", i, i * i);
	printf("fclose: %d\n", fclose(f));

	/* (C leaves the order of a call's arguments open - so every call
	   with an effect gets a statement of its own) */
	struct stat st;
	int r = stat(name, &st);
	printf("stat: %d size %lld regular %d\n", r, (long long)st.st_size,
	       S_ISREG(st.st_mode));

	f = fopen(name, "r");
	char line[64];
	int count = 0;
	long long sq = 0;
	while (fgets(line, sizeof(line), f)) {
		int a, b;
		if (sscanf(line, "line %d: %d", &a, &b) == 2 && b == a * a)
			sq += b;
		count++;
	}
	printf("fgets: %d lines, sum of squares %lld\n", count, sq);
	fseek(f, -9, SEEK_END);
	const char *tail = fgets(line, sizeof(line), f) ? strtok(line, "\n") : "?";
	printf("fseek end: [%s] ftell %ld\n", tail, ftell(f));
	fseek(f, 5, SEEK_SET);
	int c1 = fgetc(f);
	int c2 = fgetc(f);
	printf("fseek set: %c%c\n", c1, c2);
	fclose(f);

	/* "w" again truncates; "a" appends */
	f = fopen(name, "w");
	fputs("first\n", f);
	fclose(f);
	f = fopen(name, "a");
	fputs("second\n", f);
	fclose(f);
	f = fopen(name, "r");
	size_t got = fread(line, 1, sizeof(line) - 1, f);
	line[got] = 0;
	fclose(f);
	printf("truncate+append: %zu [%s]\n", got, strtok(line, "\n"));

	int fd = open(name, O_CREAT | O_EXCL | O_WRONLY, 0644);
	int e = errno;
	printf("O_EXCL on an existing file: %d %s\n", fd, strerror(e));
	errno = 0;
	f = fopen("no_such_file.txt", "r");
	e = errno;
	printf("missing: %s %d %s\n", f ? "opened" : "null", e == ENOENT,
	       strerror(e));

	printf("rename: %d\n", rename(name, "libc_t2.txt"));
	printf("old name gone: %d\n", access(name, F_OK) != 0);
	printf("unlink: %d\n", unlink("libc_t2.txt"));
	r = stat("libc_t2.txt", &st);
	printf("unlinked gone: %d\n", r != 0 && errno == ENOENT);

	printf("mkdir: %d\n", mkdir("libc_dir", 0755));
	r = stat("libc_dir", &st);
	printf("dir stat: %d dir %d\n", r, S_ISDIR(st.st_mode));
	char cwd[256], back[256] = "/";
	if (!getcwd(back, sizeof(back)))
		printf("getcwd: failed\n");
	printf("chdir: %d\n", chdir("libc_dir"));
	if (!getcwd(cwd, sizeof(cwd)))
		strcpy(cwd, "?");
	const char *last = strrchr(cwd, '/');
	printf("getcwd ends in: %s\n", last ? last + 1 : cwd);
	f = fopen("inner.txt", "w");
	fputs("inside\n", f);
	fclose(f);
	if (chdir(back))
		printf("chdir back: failed\n");
	f = fopen("libc_dir/inner.txt", "r");
	printf("path into a directory: %s", f && fgets(line, sizeof(line), f)
	       ? line : "?\n");
	if (f)
		fclose(f);
	unlink("libc_dir/inner.txt");
	rmdir("libc_dir");
}

/* ftruncate cuts a file back and grows it with zeros: a database
   rolling back a transaction depends on both */
static void truncation(void)
{
	const char *name = "libc_tr.bin";
	unsigned char buf[6000];
	for (int i = 0; i < (int)sizeof(buf); i++)
		buf[i] = (unsigned char)(i * 7 + 1);
	int fd = open(name, O_CREAT | O_TRUNC | O_RDWR, 0644);
	if (fd < 0 || write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
		printf("ftruncate: cannot make the file\n");
		return;
	}
	struct stat st;
	printf("ftruncate to 1500: %d\n", ftruncate(fd, 1500));
	fstat(fd, &st);
	printf("size %lld\n", (long long)st.st_size);
	printf("ftruncate to 3100: %d\n", ftruncate(fd, 3100));
	fstat(fd, &st);
	unsigned char back[4000];
	ssize_t n = pread(fd, back, sizeof(back), 0);
	int same = n >= 1500 && !memcmp(back, buf, 1500);
	int zeros = 1;
	for (ssize_t i = 1500; i < n; i++)
		zeros &= back[i] == 0;
	printf("size %lld, read %zd, the first 1500 kept %d, the rest zeros %d\n",
	       (long long)st.st_size, n, same, zeros);
	/* a write past the end leaves zeros between */
	if (pwrite(fd, "xyz", 3, 5000) != 3)
		printf("pwrite past the end failed\n");
	fstat(fd, &st);
	n = pread(fd, back, 10, 4995);
	printf("pwrite at 5000: size %lld, around it %d %d %d %c%c%c\n",
	       (long long)st.st_size, back[0], back[4], (int)n,
	       back[5], back[6], back[7]);
	/* one file, one inode number, by name and by descriptor alike -
	   SQLite compares the two to tell its file was not replaced */
	struct stat by_name, other;
	int fd2 = open("libc_tr2.bin", O_CREAT | O_WRONLY, 0644);
	fstat(fd, &st);
	stat(name, &by_name);
	stat("libc_tr2.bin", &other);
	printf("inode: by name = by descriptor %d, another file differs %d\n",
	       st.st_ino == by_name.st_ino, st.st_ino != other.st_ino);
	close(fd2);
	unlink("libc_tr2.bin");
	printf("ftruncate to 0: %d\n", ftruncate(fd, 0));
	fstat(fd, &st);
	printf("size %lld\n", (long long)st.st_size);
	close(fd);
	unlink(name);
}

/* A file unlinked while open lives on behind its descriptor: that is
   how tmpfile() works, and how a database keeps its temporary files */
static void unlinked(void)
{
	const char *name = "libc_ul.txt";
	int fd = open(name, O_CREAT | O_TRUNC | O_RDWR, 0644);
	if (fd < 0 || write(fd, "still here after unlink\n", 24) != 24) {
		printf("unlinked: cannot make the file\n");
		return;
	}
	printf("unlink while open: %d\n", unlink(name));
	printf("the name is gone: %d\n", access(name, F_OK) != 0 && errno == ENOENT);
	int again = open(name, O_CREAT | O_EXCL | O_WRONLY, 0644);
	printf("a new file may take the name: %d\n", again >= 0);
	if (again >= 0) {
		close(again);
		unlink(name);
	}
	char buf[64] = {0};
	if (write(fd, "and more\n", 9) != 9)
		printf("unlinked: write failed\n");
	ssize_t n = pread(fd, buf, sizeof(buf) - 1, 0);
	printf("the descriptor still reads it: %zd [%s]\n", n, strtok(buf, "\n"));
	close(fd);

	FILE *t = tmpfile();
	if (!t) {
		printf("tmpfile: %s\n", strerror(errno));
		return;
	}
	for (int i = 0; i < 500; i++)
		fprintf(t, "%d,", i * i);
	rewind(t);
	long sum = 0;
	int v;
	while (fscanf(t, "%d,", &v) == 1)
		sum += v;
	printf("tmpfile: written, read back, sum %ld\n", sum);
	fclose(t);
}

/* A file mapped privately: the mapping shows what the file holds and
   zeros in the rest of its last page, an offset maps from there, one
   that is not a page's start is refused, and what is written into the
   mapping stays out of the file */
static void mappings(void)
{
	const char *name = "libc_map.bin";
	static unsigned char data[5000];
	for (int i = 0; i < (int)sizeof(data); i++)
		data[i] = (unsigned char)(i * 7 + 3);
	int fd = open(name, O_CREAT | O_TRUNC | O_RDWR, 0644);
	if (fd < 0 || write(fd, data, sizeof(data)) != (ssize_t)sizeof(data)) {
		printf("mmap: cannot make the file\n");
		return;
	}
	unsigned char *m = mmap(0, sizeof(data), PROT_READ | PROT_WRITE,
	                        MAP_PRIVATE, fd, 0);
	if (m == MAP_FAILED) {
		printf("mmap private: %s\n", strerror(errno));
	} else {
		printf("mmap private: same bytes %d, the rest of its page zero %d\n",
		       !memcmp(m, data, sizeof(data)),
		       m[sizeof(data)] == 0 && m[8191] == 0);
		m[0] ^= 0xFF;
		unsigned char first = 0;
		printf("written into the mapping, the file keeps its byte: %d\n",
		       pread(fd, &first, 1, 0) == 1 && first == data[0]);
		munmap(m, sizeof(data));
	}
	m = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 4096);
	printf("mmap from 4096: %s\n", m == MAP_FAILED ? strerror(errno)
	       : !memcmp(m, data + 4096, sizeof(data) - 4096) ? "the second page"
	       : "other bytes");
	if (m != MAP_FAILED)
		munmap(m, 4096);
	m = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 100);
	printf("mmap from 100: %s\n", m == MAP_FAILED ? strerror(errno)
	       : "mapped");
	if (m != MAP_FAILED)
		munmap(m, 4096);
	close(fd);
	unlink(name);
}

/* A file only read - byte by byte, from places, by its descriptor -
   and one that is not there */
static void reading(const char *name)
{
	FILE *f = fopen(name, "rb");
	if (!f) {
		printf("read %s: %s\n", name, strerror(errno));
		return;
	}
	unsigned long h = 0xcbf29ce484222325UL;
	long n = 0;
	int c;
	while ((c = getc(f)) != EOF) {
		h = (h ^ (unsigned char)c) * 0x100000001b3UL;
		n++;
	}
	printf("read: %ld bytes, fnv %016lx, at the end %d\n", n, h, feof(f));
	fseek(f, 1000, SEEK_SET);
	unsigned char b[4] = { 0 };
	size_t got = fread(b, 1, sizeof(b), f);
	long at = ftell(f);
	printf("fseek 1000: %zu bytes %02x%02x%02x%02x, ftell %ld\n", got,
	       b[0], b[1], b[2], b[3], at);
	fseek(f, -2, SEEK_END);
	got = fread(b, 1, sizeof(b), f);
	printf("fseek end-2: %zu bytes\n", got);
	fclose(f);

	struct stat st;
	int r = stat(name, &st);
	printf("stat: %d size %lld regular %d\n", r, (long long)st.st_size,
	       S_ISREG(st.st_mode));
	int fd = open(name, O_RDONLY);
	off_t end = lseek(fd, 0, SEEK_END);
	ssize_t pr = pread(fd, b, sizeof(b), 0);
	off_t still = lseek(fd, 0, SEEK_CUR);
	printf("lseek end %lld, pread %zd: %02x%02x%02x%02x, position %lld\n",
	       (long long)end, pr, b[0], b[1], b[2], b[3], (long long)still);
	printf("close: %d\n", close(fd));
	fd = open("libc_none.txt", O_RDONLY);
	printf("open of nothing: %d %s\n", fd, strerror(errno));
}

/* The program itself again, as another process: posix_spawn with its
   output into a pipe (dup2 in the child, the pipe's other end closed
   there), its code by waitpid - and a program that is not there,
   which the child's exec failing tells posix_spawn */
static void spawning(const char *self)
{
	int p[2];
	if (pipe(p)) {
		printf("spawn: no pipe\n");
		return;
	}
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, p[1], 1);
	posix_spawn_file_actions_addclose(&fa, p[0]);
	posix_spawn_file_actions_addclose(&fa, p[1]);
	char *args[] = { (char *)self, "child", "two", "three", 0 };
	pid_t pid = 0;
	int r = posix_spawn(&pid, self, &fa, 0, args, environ);
	close(p[1]);
	char said[128];
	int n = 0;
	ssize_t got;
	while (n < (int)sizeof(said) - 1 &&
	       (got = read(p[0], said + n, sizeof(said) - 1 - n)) > 0)
		n += (int)got;
	said[n] = 0;
	close(p[0]);
	int status = 0;
	pid_t w = waitpid(pid, &status, 0);
	printf("posix_spawn: %d, same pid %d, it said: %s", r, w == pid, said);
	printf("and ended with %d (exited %d)\n", WEXITSTATUS(status),
	       WIFEXITED(status));
	posix_spawn_file_actions_destroy(&fa);
	char *none[] = { "libc_nothing", 0 };
	r = posix_spawn(&pid, "libc_nothing", 0, 0, none, environ);
	printf("posix_spawn of nothing: %s\n", strerror(r));
}

static void processes(void)
{
	pid_t p = fork();
	if (p == 0) {
		printf("child: says hello\n");
		fflush(stdout);
		_exit(7);
	}
	int status = 0;
	pid_t w = waitpid(p, &status, 0);
	printf("fork/wait: same %d exited %d code %d\n", w == p,
	       WIFEXITED(status), WEXITSTATUS(status));
	printf("getpid: positive %d\n", getpid() > 0);
}

static void timing(void)
{
	struct timespec a, b;
	clock_gettime(CLOCK_MONOTONIC, &a);
	struct timespec nap = { 0, 300 * 1000000L };
	nanosleep(&nap, 0);
	clock_gettime(CLOCK_MONOTONIC, &b);
	long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
	printf("nanosleep 300 ms: slept enough %d, not far too long %d\n",
	       ms >= 290, ms < 1500);
	time_t now = time(0);
	struct tm tm;
	gmtime_r(&now, &tm);
	char date[32];
	strftime(date, sizeof(date), "%Y-%m-%d", &tm);
	printf("~date: %s\n", date);
	time_t fixed = 1900000000;
	gmtime_r(&fixed, &tm);
	strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S %A", &tm);
	printf("gmtime: %s\n", date);
	struct utsname u;
	uname(&u);
	printf("~uname: %s %s\n", u.sysname, u.machine);
	struct sysinfo si;
	if (sysinfo(&si))
		printf("~ram: none\n");
	else
		printf("~ram: %llu KiB, %llu free, up %ld s\n",
		       (unsigned long long)si.totalram * si.mem_unit / 1024,
		       (unsigned long long)si.freeram * si.mem_unit / 1024,
		       (long)si.uptime);
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "hold")) {
		/* For the test of what a crash leaves behind: a file unlinked
		   while open, held until the machine is switched off */
		int fd = open("libc_held.bin", O_CREAT | O_TRUNC | O_RDWR, 0644);
		static char block[5000];
		memset(block, 'h', sizeof(block));
		if (fd < 0 || write(fd, block, sizeof(block)) != (ssize_t)sizeof(block) ||
		    unlink("libc_held.bin"))
			return 1;
		printf("holding an unlinked file\n");
		fflush(stdout);
		for (;;)
			sleep(1);
	}
	if (argc > 1 && !strcmp(argv[1], "child")) {
		/* spawning()'s child: what it was given, and a code of its own */
		printf("child: %d arguments, the last \"%s\"\n", argc,
		       argv[argc - 1]);
		return 9;
	}
	int memory = argc > 2 && !strcmp(argv[1], "memory");
	atexit(bye);
	printf("args: %d [%s]\n", argc, argc > 1 ? argv[1] : "");
	numbers();
	strings();
	if (setjmp(jb) == 0)
		deep(1000);
	else
		printf("longjmp: back from 1000 calls deep\n");
	heap();
	if (memory) {
		reading(argv[2]);
		fflush(stdout);
		processes();
		spawning(argv[0]);
	} else {
		files();
		truncation();
		unlinked();
		mappings();
		fflush(stdout);
		processes();
	}
	timing();
	printf("~isatty: %d\n", isatty(1));
	fprintf(stderr, "stderr: also seen\n");
	printf("done\n");
	return 3;
}
