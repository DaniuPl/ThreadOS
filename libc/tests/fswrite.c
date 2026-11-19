/* fswrite - a day of writing, for e2fsck to judge afterwards
 *
 * t_tfs4write formats a disk with TFS4, runs this, and then checks the
 * disk on the host with Linux's e2fsck and reads it back with debugfs.
 * Every step below is something the writer has to get right in the
 * bitmaps, counts, link counts and directories, or e2fsck says so:
 *
 *   5000 files made in one directory (the directory grows to many
 *   blocks), every odd one removed again (entries merged into their
 *   neighbours, blocks and inodes given back)
 *   fifty of the rest moved into another directory, which is itself
 *   moved under a third (its ".." and three link counts change)
 *   a file written past its direct blocks, again past the doubly
 *   indirect one's start with a hole before, cut back, grown by
 *   ftruncate (a hole) and written at its end
 *   a file removed while open, still written and read through its
 *   descriptor, given back at close
 *
 * The contents are a function of the names, so the host knows what
 * every file must hold. Prints "[fswrite] ..." and "done" at the end.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FILES 5000

static int failures;

static void check(const char *what, int ok)
{
	printf("[fswrite] %s: %s\n", ok ? "ok" : "FAIL", what);
	if (!ok)
		failures++;
}

/* what file n holds: "file n\n" as many times as n % 13 + 1 */
static size_t body(int n, char *out)
{
	size_t len = 0;
	for (int i = 0; i < n % 13 + 1; i++)
		len += (size_t)sprintf(out + len, "file %d\n", n);
	return len;
}

static int write_file(const char *path, const void *data, size_t n)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;
	ssize_t w = write(fd, data, n);
	close(fd);
	return w == (ssize_t)n ? 0 : -1;
}

/* the byte at offset off of the big file, before any cutting */
static unsigned char pattern(long off)
{
	return (unsigned char)((off * 131 + off / 4096) & 0xFF);
}

static int fill(int fd, long at, long len)
{
	static unsigned char buf[65536];
	for (long done = 0; done < len;) {
		long n = len - done > (long)sizeof(buf) ? (long)sizeof(buf)
		                                       : len - done;
		for (long i = 0; i < n; i++)
			buf[i] = pattern(at + done + i);
		if (pwrite(fd, buf, (size_t)n, at + done) != n)
			return -1;
		done += n;
	}
	return 0;
}

int main(void)
{
	static char path[256], to[256], data[256];
	int ok = mkdir("/w", 0755) == 0 && mkdir("/w/a", 0755) == 0 &&
	         mkdir("/w/b", 0755) == 0 && mkdir("/w/c", 0755) == 0;
	check("directories made", ok);

	ok = 1;
	for (int n = 0; n < FILES && ok; n++) {
		snprintf(path, sizeof(path), "/w/a/f%04d", n);
		size_t len = body(n, data);
		ok = write_file(path, data, len) == 0;
	}
	check("5000 files made in one directory", ok);

	ok = 1;
	for (int n = 1; n < FILES; n += 2) {
		snprintf(path, sizeof(path), "/w/a/f%04d", n);
		ok &= unlink(path) == 0;
	}
	check("every odd one removed", ok);

	ok = 1;
	for (int n = 0; n < 100; n += 2) {
		snprintf(path, sizeof(path), "/w/a/f%04d", n);
		snprintf(to, sizeof(to), "/w/b/moved-%04d", n);
		ok &= rename(path, to) == 0;
	}
	check("fifty moved into another directory", ok);
	check("and that directory moved under a third",
	      rename("/w/b", "/w/c/b") == 0);
	struct stat st;
	check("the moved directory's '..' leads to its new parent",
	      stat("/w/c/b/../b/moved-0000", &st) == 0);

	/* the big file: 300 KiB, then 5 MiB at 10 MiB with a hole before */
	int fd = open("/w/grow.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
	ok = fd >= 0 && fill(fd, 0, 300 << 10) == 0 &&
	     fill(fd, 10L << 20, 5L << 20) == 0;
	check("a file written past its direct and into its doubly indirect "
	      "blocks", ok);
	ok = fd >= 0 && ftruncate(fd, 100 << 10) == 0 &&
	     ftruncate(fd, 200 << 10) == 0;
	static const char tail[] = "the end of a file grown by ftruncate\n";
	ok = ok && pwrite(fd, tail, sizeof(tail) - 1, (200 << 10)) ==
	                 (ssize_t)(sizeof(tail) - 1);
	check("cut back to 100 KiB, grown to 200 KiB, written at its end", ok);
	if (fd >= 0)
		close(fd);

	/* a file removed while open */
	fd = open("/w/tmpfile", O_RDWR | O_CREAT | O_TRUNC, 0600);
	ok = fd >= 0 && write(fd, "before", 6) == 6 &&
	     unlink("/w/tmpfile") == 0 && write(fd, " after", 6) == 6;
	char back[16] = "";
	ok = ok && pread(fd, back, 12, 0) == 12 && !memcmp(back, "before after",
	                                                   12);
	check("a file removed while open is still written and read", ok);
	if (fd >= 0)
		close(fd);
	check("and gone by its name", access("/w/tmpfile", F_OK) != 0);

	/* the SQL a database is built with, run by sqlite3 next: rows of
	   many sizes, a third taken out, the file rewritten by VACUUM */
	FILE *sql = fopen("/w/build.sql", "w");
	ok = sql != NULL;
	if (sql) {
		fprintf(sql, "CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, "
		        "pad BLOB);\nBEGIN;\n");
		for (int i = 1; i <= 2000; i++)
			fprintf(sql, "INSERT INTO t VALUES(%d,'row %d',"
			        "zeroblob(%d));\n", i, i, 100 + i % 400);
		fprintf(sql, "COMMIT;\nDELETE FROM t WHERE id %% 3 = 0;\n"
		        "VACUUM;\n");
		ok = fclose(sql) == 0;
	}
	check("the database's SQL written", ok);

	printf("[fswrite] %s\n", failures ? "FAILED" : "done");
	return failures ? 1 : 0;
}
