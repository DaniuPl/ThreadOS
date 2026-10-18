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
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
}

int main(int argc, char **argv)
{
	atexit(bye);
	printf("args: %d [%s]\n", argc, argc > 1 ? argv[1] : "");
	numbers();
	strings();
	if (setjmp(jb) == 0)
		deep(1000);
	else
		printf("longjmp: back from 1000 calls deep\n");
	heap();
	files();
	fflush(stdout);
	processes();
	timing();
	printf("~isatty: %d\n", isatty(1));
	fprintf(stderr, "stderr: also seen\n");
	printf("done\n");
	return 3;
}
