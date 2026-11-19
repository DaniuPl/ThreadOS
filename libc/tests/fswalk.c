/* fswalk - everything under a directory, with a SHA-256 of every file
 *
 * t_tfs4read builds a tree on the host, puts it into an ext2 image with
 * Linux's mke2fs and compares what this prints with what Python's
 * hashlib says of the same files. Through musl's opendir, readdir, stat
 * and read, so the C library's way to the file system is the one
 * checked.
 *
 *   D <path>                      a directory
 *   F <size> <sha256> <path>      a file
 *   M <path> <count> <sha256>     a directory of many files, folded into
 *                                 one line: the SHA-256 of its lines
 *                                 "<name> <sha256>\n", sorted by name
 *   END dirs=<n> files=<n>
 *
 * A directory is folded when it holds more than FOLD entries - the
 * serial line would take minutes over ten thousand lines.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define THREADOS_UAPI_H              /* usha256.h without the ring-3 API */
#include "../../user/usha256.h"

#define FOLD 200

static long ndirs, nfiles;
static unsigned char chunk[65536];

static void hexof(const uint8_t *d, char *out)
{
	for (int i = 0; i < SHA256_SIZE; i++)
		sprintf(out + 2 * i, "%02x", d[i]);
}

/* The file's SHA-256 in hex; its size into *size. -1 when unreadable */
static int hash_file(const char *path, char *hex, long long *size)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	struct sha256 c;
	sha256_init(&c);
	long long total = 0;
	ssize_t n;
	while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
		sha256_update(&c, chunk, (uint32_t)n);
		total += n;
	}
	close(fd);
	if (n < 0)
		return -1;
	uint8_t d[SHA256_SIZE];
	sha256_final(&c, d);
	hexof(d, hex);
	*size = total;
	return 0;
}

static int by_name(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static void walk(const char *dir)
{
	DIR *d = opendir(dir);
	if (!d) {
		printf("ERROR opendir %s\n", dir);
		return;
	}
	char **names = NULL;
	size_t n = 0, cap = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") ||
		    !strcmp(e->d_name, "lost+found"))
			continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 64;
			names = realloc(names, cap * sizeof(*names));
		}
		names[n++] = strdup(e->d_name);
	}
	closedir(d);
	qsort(names, n, sizeof(*names), by_name);
	ndirs++;
	printf("D %s\n", dir);

	static char path[4096];
	struct sha256 fold;
	sha256_init(&fold);
	int folding = n > FOLD;
	long folded = 0;
	for (size_t i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "%s/%s", strcmp(dir, "/") ? dir : "",
		         names[i]);
		struct stat st;
		if (stat(path, &st) < 0) {
			printf("ERROR stat %s\n", path);
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			char *sub = strdup(path);
			walk(sub);
			free(sub);
			continue;
		}
		char hex[2 * SHA256_SIZE + 1];
		long long size;
		if (access(path, R_OK) < 0 && errno == EACCES) {
			/* not this account's to read (the system's swap file):
			   said, not counted */
			printf("P %s\n", path);
			continue;
		}
		if (hash_file(path, hex, &size) < 0) {
			printf("ERROR read %s\n", path);
			continue;
		}
		nfiles++;
		if (size != (long long)st.st_size)
			printf("ERROR size %s stat %lld read %lld\n", path,
			       (long long)st.st_size, size);
		if (folding) {
			char line[512];
			int l = snprintf(line, sizeof(line), "%s %s\n", names[i], hex);
			sha256_update(&fold, line, (uint32_t)l);
			folded++;
		} else {
			printf("F %lld %s %s\n", size, hex, path);
		}
	}
	if (folding) {
		uint8_t dg[SHA256_SIZE];
		char hex[2 * SHA256_SIZE + 1];
		sha256_final(&fold, dg);
		hexof(dg, hex);
		printf("M %s %ld %s\n", dir, folded, hex);
	}
	for (size_t i = 0; i < n; i++)
		free(names[i]);
	free(names);
}

int main(int argc, char **argv)
{
	const char *top = argc > 1 ? argv[1] : "/";
	walk(top);
	printf("END dirs=%ld files=%ld\n", ndirs, nfiles);
	return 0;
}
