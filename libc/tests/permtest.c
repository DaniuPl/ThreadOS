/* permtest - POSIX's file rights and metadata on TFS4, step by step
 *
 * t_tfs4perm signs in as two accounts in turn and runs one step as
 * each; between them the disk is the only thing they share.
 *
 *   alice    makes /shared (0755) with "secret" (0600), "open" (0644),
 *            a hard link to secret, a symbolic link to it short enough
 *            for the inode and one long enough for a block, a file made
 *            under umask 077, sets open's times to 2001-09-09 01:46:40,
 *            and a file of her own in the root (everybody's to write in,
 *            sticky); then prints what stat says of each, for debugfs
 *            to agree with
 *   bob      may not read the secret, write the open file, remove it
 *            or change its mode, may not look into alice's home or
 *            remove her file in the root, may make his own there, reads
 *            the open file and the links' targets
 *   alice2   opens the secret up with chmod
 *   bob2     now reads it
 *
 * Prints "[permtest] ok|FAIL: ..." and "STAT <path> <mode> <uid> <gid>
 * <links> <size> <mtime>" lines, then "done".
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

static void check(const char *what, int ok)
{
	printf("[permtest] %s: %s\n", ok ? "ok" : "FAIL", what);
	if (!ok)
		failures++;
}

static void expect_errno(const char *what, int r, int want)
{
	char msg[200];
	snprintf(msg, sizeof(msg), "%s (got %s, want %s)", what,
	         r < 0 ? strerror(errno) : "success", strerror(want));
	check(msg, r < 0 && errno == want);
}

static int make(const char *path, const char *text, mode_t mode)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (fd < 0)
		return -1;
	ssize_t n = write(fd, text, strlen(text));
	close(fd);
	return n == (ssize_t)strlen(text) ? 0 : -1;
}

static int slurp(const char *path, char *out, size_t cap)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	ssize_t n = read(fd, out, cap - 1);
	close(fd);
	if (n < 0)
		return -1;
	out[n] = 0;
	return 0;
}

static void say_stat(const char *path)
{
	struct stat st;
	if (lstat(path, &st) < 0) {
		printf("STAT %s missing\n", path);
		return;
	}
	printf("STAT %s %o %u %u %u %lld %lld\n", path,
	       (unsigned)st.st_mode, (unsigned)st.st_uid, (unsigned)st.st_gid,
	       (unsigned)st.st_nlink, (long long)st.st_size,
	       (long long)st.st_mtime);
}

#define LONG_TARGET "/shared/./././././././././././././././././././././secret"

static void alice(void)
{
	umask(022);
	check("mkdir /shared 0755", mkdir("/shared", 0755) == 0);
	check("a 0600 secret", make("/shared/secret", "alice's secret\n",
	                            0600) == 0);
	check("a 0644 file for everybody",
	      make("/shared/open", "for everybody\n", 0644) == 0);
	check("a hard link to the secret",
	      link("/shared/secret", "/shared/secret-link") == 0);
	check("a short symbolic link", symlink("secret", "/shared/sym") == 0);
	check("a long symbolic link", symlink(LONG_TARGET,
	                                      "/shared/longsym") == 0);
	umask(077);
	check("a file made under umask 077",
	      make("/shared/masked", "masked\n", 0666) == 0);
	umask(022);
	struct timespec t[2] = { { 1000000000, 0 }, { 1000000000, 0 } };
	check("utimensat sets the open file's times",
	      utimensat(AT_FDCWD, "/shared/open", t, 0) == 0);
	check("a file of her own in the root",
	      make("/alice-root.txt", "alice was here\n", 0644) == 0);

	struct stat st, lst;
	check("the secret has two names now",
	      stat("/shared/secret", &st) == 0 && st.st_nlink == 2);
	char buf[256] = "";
	ssize_t n = readlink("/shared/sym", buf, sizeof(buf) - 1);
	check("readlink gives the short link's target",
	      n == 6 && !memcmp(buf, "secret", 6));
	n = readlink("/shared/longsym", buf, sizeof(buf) - 1);
	check("and the long link's", n == (ssize_t)strlen(LONG_TARGET) &&
	      !memcmp(buf, LONG_TARGET, (size_t)n));
	check("lstat says link, stat follows it to the secret",
	      lstat("/shared/sym", &lst) == 0 && S_ISLNK(lst.st_mode) &&
	      stat("/shared/sym", &st) == 0 && S_ISREG(st.st_mode) &&
	      st.st_size == 15);
	check("umask 077 made it 0600",
	      stat("/shared/masked", &st) == 0 && (st.st_mode & 0777) == 0600);
	check("access: she may write her file",
	      access("/shared/open", W_OK) == 0);
	check("her home is hers alone (0700)",
	      stat("/home/alice", &st) == 0 && (st.st_mode & 0777) == 0700);
	for (const char *p = "/shared"; p; p = NULL)
		say_stat(p);
	const char *all[] = { "/shared/secret", "/shared/open",
	                      "/shared/secret-link", "/shared/sym",
	                      "/shared/longsym", "/shared/masked",
	                      "/alice-root.txt" };
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
		say_stat(all[i]);
}

static void bob(void)
{
	char buf[256];
	struct stat st;
	errno = 0;
	expect_errno("he may not read the secret",
	             open("/shared/secret", O_RDONLY), EACCES);
	expect_errno("nor through its symbolic link",
	             open("/shared/sym", O_RDONLY), EACCES);
	check("he reads the open file",
	      slurp("/shared/open", buf, sizeof(buf)) == 0 &&
	      !strcmp(buf, "for everybody\n"));
	expect_errno("but may not write it",
	             open("/shared/open", O_WRONLY), EACCES);
	expect_errno("nor remove it from alice's directory",
	             unlink("/shared/open"), EACCES);
	expect_errno("nor change its mode", chmod("/shared/open", 0666), EPERM);
	expect_errno("nor give it away", chown("/shared/open", 1, 1), EPERM);
	expect_errno("access says the same",
	             access("/shared/secret", R_OK), EACCES);
	ssize_t n = readlink("/shared/sym", buf, sizeof(buf));
	check("the link's target is his to read", n == 6);
	check("and stat of the secret is his to ask (its bits say 0600)",
	      stat("/shared/secret", &st) == 0 && (st.st_mode & 0777) == 0600);
	DIR *d = opendir("/home/alice");
	expect_errno("he may not look into alice's home", d ? 0 : -1, EACCES);
	if (d)
		closedir(d);
	expect_errno("nor remove her file from the root (sticky)",
	             unlink("/alice-root.txt"), EPERM);
	check("he makes his own there", make("/bob-root.txt", "bob\n",
	                                     0644) == 0);
	say_stat("/bob-root.txt");
}

static void alice2(void)
{
	check("she opens the secret up", chmod("/shared/secret", 0644) == 0);
	say_stat("/shared/secret");
	say_stat("/shared/secret-link");       /* the same inode */
}

static void bob2(void)
{
	char buf[256];
	check("now he reads it",
	      slurp("/shared/secret", buf, sizeof(buf)) == 0 &&
	      !strcmp(buf, "alice's secret\n"));
}

int main(int argc, char **argv)
{
	const char *step = argc > 1 ? argv[1] : "";
	if (!strcmp(step, "alice"))
		alice();
	else if (!strcmp(step, "bob"))
		bob();
	else if (!strcmp(step, "alice2"))
		alice2();
	else if (!strcmp(step, "bob2"))
		bob2();
	else
		check("a step: alice, bob, alice2 or bob2", 0);
	printf("[permtest] %s\n", failures ? "FAILED" : "done");
	return failures ? 1 : 0;
}
