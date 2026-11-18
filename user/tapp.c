/* tapp - application packages (.tapp, scripts/mktapp.py)
 *
 *   tapp                 the packages the loader brought: their names,
 *   tapp list            versions and sizes
 *   tapp info FILE       what a package file says it is, and what it holds
 *   tapp verify NAME     whether a package is signed by the key that signs
 *                        ThreadOS (user/usignkey.h) - NAME is a package
 *                        the system has, or a package file
 *   tapp install FILE    a package file, signed by that key, onto the
 *                        installed system: into /EFI/ThreadOS/apps, where
 *                        the loader finds it, and running at once where
 *                        it can (an administrator's to do: sudo)
 *
 * Reading and checking a package is user/utapp.h's, shared with gtapp,
 * the window a package opens in from the Files app.
 *
 * verify ends with 0 for a package signed by the system's key, 1 for
 * one that is not (or has no signature), 2 for a file that is not a
 * package. install ends with 0 when the package is on the disk, and
 * says whether it runs now or from the next start.
 */

#include "utapp.h"

static struct tapp_pkg pkg;

static void say(const char *a, const char *b, const char *c)
{
    print(a);
    if (b)
        print(b);
    if (c)
        print(c);
}

/* 0 signed by the key, 1 not, 2 not a package */
static int verify(struct tapp_src *s, const char *what)
{
    const char *why;
    if (tapp_open(s, &pkg, &why) != 0) {
        say(what, ": not a package - ", why);
        print("\n");
        return 2;
    }
    int from;
    switch (tapp_verify(s, &pkg, &from)) {
    case TAPP_SIGNED:
        say(what, ": signed by the system's key",
            from == SIGNKEY_DISK ? " (update.key)\n" : "\n");
        return 0;
    case TAPP_NOT_SIGNED:
        say(what, ": NOT signed by the system's key",
            from == SIGNKEY_DISK ? " (update.key)\n" : "\n");
        return 1;
    case TAPP_UNSIGNED:
        say(what, ": has no signature\n", 0);
        return 1;
    default:
        say(what, ": cannot be read to its end\n", 0);
        return 2;
    }
}

static int list(void)
{
    struct tapp_info t;
    int n = 0;
    for (; sys_package(n, &t) == 0; n++) {
        say(t.id, " ", 0);
        print_dec(t.version);
        say(" - ", t.name, 0);
        if (t.name_pl[0])
            say(" / ", t.name_pl, 0);
        print(", ");
        print_dec(t.size / 1024);
        say(" KiB, runs ", t.exec, t.dock ? ", in the dock\n" : "\n");
    }
    if (!n)
        print("no packages\n");
    return 0;
}

static int info(const char *path)
{
    struct tapp_src s = { -1, (int)sys_open(path, O_READ), 0, 0 };
    if (s.fd < 0) {
        say("tapp: cannot open ", path, "\n");
        return 2;
    }
    const char *why;
    if (tapp_open(&s, &pkg, &why) != 0) {
        say(path, ": not a package - ", why);
        print("\n");
        sys_close(s.fd);
        return 2;
    }
    print(pkg.manifest);
    for (int i = 1; i < pkg.count; i++) {
        say("  ", pkg.entries[i].name, " ");
        print_dec(pkg.entries[i].size);
        print(" B\n");
    }
    sys_close(s.fd);
    return 0;
}

/* The whole file read into memory, checked, and - only when it is signed
   by the system's key - handed to the kernel */
static int install(const char *path)
{
    const char *why;
    uint8_t *buf = tapp_load(path, &pkg, &why);
    if (!buf) {
        say(path, ": not a package - ", why);
        print("\n");
        return 2;
    }
    struct tapp_src m = { -1, -1, buf, pkg.length };
    if (verify(&m, path) != 0) {
        say(path, ": not installed - only a package signed by the "
            "system's key is\n", 0);
        return 1;
    }
    switch (sys_package_install(buf, pkg.length)) {
    case 0:
        say(path, ": installed - its programs run now\n", 0);
        return 0;
    case 1:
        say(path, ": installed - it starts with the next start of "
            "the system\n", 0);
        return 0;
    case -2:
        say("tapp: only an administrator installs packages "
            "(sudo run tapp install ", path, ")\n");
        return 1;
    case -3:
        print("tapp: this is not a system started from its disk - a "
              "package goes onto the disk the system starts from\n");
        return 1;
    case -4:
        say(path, ": not a package for this machine\n", 0);
        return 1;
    case -5:
        print("tapp: there is no room for it on the EFI partition\n");
        return 1;
    case -6:
        say(path, ": that version or a newer one is installed already\n",
            0);
        return 1;
    default:
        say(path, ": writing it failed\n", 0);
        return 1;
    }
}

void _start(int argc, char **argv)
{
    if (argc < 2 || tapp_streq(argv[1], "list"))
        sys_exit(list());
    if (tapp_streq(argv[1], "info") && argc == 3)
        sys_exit(info(argv[2]));
    if (tapp_streq(argv[1], "install") && argc == 3)
        sys_exit(install(argv[2]));
    if (tapp_streq(argv[1], "verify") && argc == 3) {
        struct tapp_info t;
        for (int i = 0; sys_package(i, &t) == 0; i++) {
            if (tapp_streq(t.id, argv[2])) {
                struct tapp_src s = { i, -1, 0, 0 };
                sys_exit(verify(&s, t.id));
            }
        }
        struct tapp_src s = { -1, (int)sys_open(argv[2], O_READ), 0, 0 };
        if (s.fd < 0) {
            say("tapp: no package and no file called ", argv[2], "\n");
            sys_exit(2);
        }
        int rc = verify(&s, argv[2]);
        sys_close(s.fd);
        sys_exit(rc);
    }
    print("usage: tapp [list] | tapp info FILE | tapp verify NAME | "
          "tapp install FILE\n");
    sys_exit(2);
}
