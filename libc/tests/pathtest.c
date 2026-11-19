/* pathtest - long paths and directory listings through the C library
 *
 * The same ground as user64/longpath64.c, but the way a ported program
 * walks it: mkdir, fopen, rename, stat, opendir, getcwd and remove from
 * musl, with paths of several hundred bytes.
 *
 * And one thing only threads show. opendir() and a stat() of a
 * directory used to go into that directory and come back, because the
 * kernel could list and test only the working directory: for that
 * moment the whole process stood somewhere else, and another thread
 * opening a relative path opened it in the wrong place. Here one
 * thread lists and stats a subdirectory over and over while another
 * opens a file that exists only where the process really stands; every
 * one of its opens has to work.
 *
 * Prints "[pathtest] ..." lines; anything wrong is a "FAIL" line and
 * exit code 1.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DEPTH 6
#define NAME  50

static int failures;

static void check(const char *what, int ok)
{
	printf("[pathtest] %s: %s\n", ok ? "ok" : "FAIL", what);
	if (!ok)
		failures++;
}

/* the first k directories of the chain */
static void chain(char *out, int k)
{
	size_t n = 0;
	for (int d = 0; d < k; d++) {
		if (d)
			out[n++] = '/';
		memset(out + n, 'p' + d, NAME - 1);
		n += NAME - 1;
		out[n++] = (char)('1' + d);
	}
	out[n] = 0;
}

static void long_paths(void)
{
	static char p[1024], q[1024], cwd[1024];
	int ok = 1;
	for (int k = 1; k <= DEPTH; k++) {
		chain(p, k);
		ok &= mkdir(p, 0755) == 0;
	}
	chain(p, DEPTH);
	check("mkdir down a chain of six 50-byte names", ok);

	strcat(p, "/a-file.dat");
	FILE *f = fopen(p, "w");
	const char *text = "a file at the bottom of a long path\n";
	int wrote = f && fputs(text, f) >= 0;
	if (f)
		fclose(f);
	char msg[160];
	snprintf(msg, sizeof(msg), "fopen and write by a path of %zu bytes",
	         strlen(p));
	check(msg, wrote);

	struct stat st;
	check("stat by the long path: a regular file of the right size",
	      stat(p, &st) == 0 && S_ISREG(st.st_mode) &&
	      st.st_size == (off_t)strlen(text));
	chain(q, DEPTH);
	check("stat of the deepest directory says directory",
	      stat(q, &st) == 0 && S_ISDIR(st.st_mode));

	/* the inode number by name and through a descriptor agree, as a
	   database checks its file was not replaced under it */
	int fd = open(p, O_RDONLY);
	struct stat byfd;
	check("fstat and stat give one inode number",
	      fd >= 0 && fstat(fd, &byfd) == 0 && stat(p, &st) == 0 &&
	      byfd.st_ino == st.st_ino && st.st_ino > 2);
	if (fd >= 0)
		close(fd);

	chain(q, 2);
	strcat(q, "/moved-up.dat");
	check("rename between long paths", rename(p, q) == 0);
	char got[128] = "";
	f = fopen(q, "r");
	if (f) {
		if (!fgets(got, sizeof(got), f))
			got[0] = 0;
		fclose(f);
	}
	check("the renamed file reads back", !strcmp(got, text) &&
	      access(p, F_OK) != 0);

	chain(p, DEPTH);
	check("chdir by a long path", chdir(p) == 0);
	check("getcwd says the same path",
	      getcwd(cwd, sizeof(cwd)) && cwd[0] == '/' && !strcmp(cwd + 1, p));
	check("and back to the root", chdir("/") == 0);

	ok = unlink(q) == 0;
	for (int k = DEPTH; k >= 1; k--) {
		chain(p, k);
		ok &= rmdir(p) == 0;
	}
	check("unlink and rmdir all of it by long paths", ok);
}

/* --- listing without moving the process ------------------------------ */

static volatile int stop;
static int lister_rounds, lister_bad;

static void *lister(void *arg)
{
	(void)arg;
	while (!stop) {
		DIR *d = opendir("sub");
		int seen = 0;
		struct dirent *e;
		if (d) {
			while ((e = readdir(d)))
				if (!strcmp(e->d_name, "inner.txt"))
					seen = 1;
			closedir(d);
		}
		struct stat st;
		if (!seen || stat("sub", &st) != 0 || !S_ISDIR(st.st_mode))
			lister_bad++;
		lister_rounds++;
	}
	return NULL;
}

static void listing_stays_put(void)
{
	char here[256];
	if (!getcwd(here, sizeof(here)))
		here[0] = 0;
	mkdir("race", 0755);
	chdir("race");
	mkdir("sub", 0755);
	FILE *f = fopen("sub/inner.txt", "w");
	if (f) {
		fputs("inside\n", f);
		fclose(f);
	}
	f = fopen("here.txt", "w");
	if (f) {
		fputs("here\n", f);
		fclose(f);
	}

	pthread_t t;
	stop = 0;
	pthread_create(&t, NULL, lister, NULL);
	int opens = 0, misses = 0;
	time_t until = time(NULL) + 3;
	while (time(NULL) < until || opens < 200) {
		int fd = open("here.txt", O_RDONLY);
		if (fd < 0)
			misses++;
		else
			close(fd);
		opens++;
		if (opens > 200000)
			break;
	}
	stop = 1;
	pthread_join(t, NULL);
	char msg[200];
	snprintf(msg, sizeof(msg), "while another thread listed and stat'ed "
	         "a subdirectory %d times, %d of %d relative opens missed",
	         lister_rounds, misses, opens);
	check(msg, misses == 0 && lister_rounds > 10);
	check("and every listing saw the subdirectory's file", lister_bad == 0);

	unlink("sub/inner.txt");
	rmdir("sub");
	unlink("here.txt");
	chdir("..");
	rmdir("race");
	char now[256];
	check("the working directory is where it was",
	      getcwd(now, sizeof(now)) && !strcmp(now, here));
}

int main(void)
{
	long_paths();
	listing_stays_put();
	printf("[pathtest] %s\n", failures ? "FAILED" : "all done");
	return failures ? 1 : 0;
}
