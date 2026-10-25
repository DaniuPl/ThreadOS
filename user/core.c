/* core - the everyday commands of a terminal, in one program (ring 3)
 *
 * A terminal a person can use answers 'ls', 'ps', 'free', 'date', 'mv'
 * the way every other one does. Until now those were the kernel shell's
 * own commands, and the terminal in a window - which starts programs -
 * had none of them: 'ls' said there was no such program.
 *
 * They are programs now, and one program: each would be a few
 * kilobytes of the same code, and whatever the initrd holds is memory
 * the machine never gets back. The terminal starts 'core ls -l' when
 * 'ls -l' is typed; the kernel's shell can do the same by hand.
 *
 * Everything goes through the system calls any program has, on both
 * kernels (uapi.h) - which is the point: what the terminal shows is what
 * a program is allowed to see, and a refusal is the kernel's, said in
 * words.
 */

#include "uapi.h"

#define BUF 512

static void say(const char *s) { print(s); }

static void num(uint32_t v) { print_dec(v); }

/* A number right-aligned in a column of 'w' */
static void col(uint32_t v, int w)
{
    char b[12];
    int n = 0;
    do {
        b[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    for (int i = n; i < w; i++)
        say(" ");
    while (n)
        sys_write(STDOUT, &b[--n], 1);
}

static int len(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void pad(const char *s, int w)
{
    say(s);
    for (int i = len(s); i < w; i++)
        say(" ");
}

/* --- what differs between the two kernels -------------------------- */

static int read_entry(int i, char *name, int max, uint32_t *size, int *dir)
{
#ifdef __x86_64__
    struct dirent64 e;
    if (sys_readdir(i, &e) != 0)
        return -1;
    int k = 0;
    while (e.name[k] && k < max - 1) {
        name[k] = e.name[k];
        k++;
    }
    name[k] = 0;
    *size = e.size;
    *dir = e.is_dir;
#else
    char n[FS_NAME_MAX + 1];
    int32_t r = sys_readdir(i, n);
    if (r == -1)
        return -1;
    int k = 0;
    while (n[k] && k < max - 1) {
        name[k] = n[k];
        k++;
    }
    name[k] = 0;
    *dir = r == -2;
    *size = *dir ? 0 : (uint32_t)r;
#endif
    return 0;
}

static void cwd(char *buf, int max)
{
#ifdef __x86_64__
    sys_getcwd(buf, (uint64_t)max);
#else
    (void)max;
    sys_getcwd(buf);
#endif
    if (!buf[0]) {
        buf[0] = '/';
        buf[1] = 0;
    }
}

static void memory(uint32_t *ram, uint32_t *used, uint32_t *up)
{
#ifdef __x86_64__
    struct sysinfo64 si;
#else
    struct sysinfo si;
#endif
    sys_sysinfo(&si);
    *ram = si.ram_kib;
    *used = si.used_kib;
    *up = si.uptime_s;
}

/* --- the commands --------------------------------------------------- */

/* ls [-l] [-a] [directory] */
static int c_ls(int argc, char **argv)
{
    int longer = 0, all = 0;
    const char *where = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            for (int k = 1; argv[i][k]; k++) {
                if (argv[i][k] == 'l')
                    longer = 1;
                else if (argv[i][k] == 'a')
                    all = 1;
            }
        } else {
            where = argv[i];
        }
    }
    char back[64];
    if (where) {
        cwd(back, sizeof(back));
        if (sys_chdir(where) < 0) {
            say("ls: there is no directory '");
            say(where);
            say("'\n");
            return 1;
        }
    }
    char name[64];
    uint32_t size;
    int dir, shown = 0;
    for (int i = 0; read_entry(i, name, sizeof(name), &size, &dir) == 0; i++) {
        /* Names with a dot in front are hidden, as everywhere */
        if (name[0] == '.' && !all)
            continue;
        if (longer) {
            if (dir)
                say("     dir  ");
            else {
                col(size, 8);
                say("  ");
            }
        }
        say(name);
        if (dir)
            say("/");
        say(longer ? "\n" : "  ");
        shown++;
    }
    if (!longer && shown)
        say("\n");
    if (where)
        sys_chdir(back);
    return 0;
}

static int c_pwd(void)
{
    char d[64];
    cwd(d, sizeof(d));
    say(d);
    say("\n");
    return 0;
}

static const char *const states[] = { "ready", "running", "sleeping",
                                      "waiting", "zombie" };

static int c_ps(void)
{
    struct psinfo p;
    say("  PID  PARENT  STATE     NAME\n");
    for (int i = 0; sys_ps(i, &p) == 0; i++) {
        col((uint32_t)p.pid, 5);
        col((uint32_t)(p.parent < 0 ? 0 : p.parent), 8);
        say("  ");
        pad(p.state >= 0 && p.state <= 4 ? states[p.state] : "?", 10);
        char n[PS_NAME_MAX + 1];
        int k = 0;
        while (k < PS_NAME_MAX && p.name[k]) {
            n[k] = p.name[k];
            k++;
        }
        n[k] = 0;
        say(n);
        if (!p.ring3)
            say(" [kernel]");
        say("\n");
    }
    return 0;
}

static int c_free(void)
{
    uint32_t ram, used, up;
    memory(&ram, &used, &up);
    say("Memory: ");
    num(ram);
    say(" KiB, ");
    num(used);
    say(" KiB used, ");
    num(ram > used ? ram - used : 0);
    say(" KiB free\n");
    return 0;
}

static int c_uptime(void)
{
    uint32_t ram, used, up;
    memory(&ram, &used, &up);
    say("up ");
    if (up >= 3600) {
        num(up / 3600);
        say(" h ");
    }
    num((up / 60) % 60);
    say(" min ");
    num(up % 60);
    say(" s\n");
    return 0;
}

static void two(uint32_t v)
{
    char b[2] = { (char)('0' + v / 10 % 10), (char)('0' + v % 10) };
    sys_write(STDOUT, b, 2);
}

static int c_date(void)
{
    struct utime t;
    sys_time(&t);
    num(t.year);
    say("-");
    two(t.month);
    say("-");
    two(t.day);
    say(" ");
    two(t.hour);
    say(":");
    two(t.min);
    say(":");
    two(t.sec);
    say("\n");
    return 0;
}

static int c_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            say(" ");
        say(argv[i]);
    }
    say("\n");
    return 0;
}

static int need(int argc, int n, const char *usage)
{
    if (argc >= n)
        return 1;
    say("usage: ");
    say(usage);
    say("\n");
    return 0;
}

static void refused(const char *cmd, const char *what)
{
    say(cmd);
    say(": no permission for '");
    say(what);
    say("' (try it through sudo)\n");
}

static int c_rm(int argc, char **argv)
{
    if (!need(argc, 2, "rm <file>..."))
        return 1;
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        int32_t rc = (int32_t)sys_unlink(argv[i]);
        if (rc == -2) {
            refused("rm", argv[i]);
            bad = 2;
        } else if (rc < 0) {
            say("rm: there is no file '");
            say(argv[i]);
            say("'\n");
            bad = 1;
        }
    }
    return bad;
}

static int c_mv(int argc, char **argv)
{
    if (!need(argc, 3, "mv <from> <to>"))
        return 1;
    int32_t rc = (int32_t)sys_rename(argv[1], argv[2]);
    if (rc == -2) {
        refused("mv", argv[1]);
        return 2;
    }
    if (rc < 0) {
        say("mv: cannot move '");
        say(argv[1]);
        say("' to '");
        say(argv[2]);
        say("'\n");
        return 1;
    }
    return 0;
}

static int c_mkdir(int argc, char **argv)
{
    if (!need(argc, 2, "mkdir <directory>..."))
        return 1;
    for (int i = 1; i < argc; i++)
        if (sys_mkdir(argv[i]) < 0) {
            say("mkdir: cannot make '");
            say(argv[i]);
            say("'\n");
            return 1;
        }
    return 0;
}

/* An empty file, or one left as it is */
static int c_touch(int argc, char **argv)
{
    if (!need(argc, 2, "touch <file>..."))
        return 1;
    for (int i = 1; i < argc; i++) {
        int fd = sys_open(argv[i], O_READ | O_CREATE);
        if (fd < 0) {
            say("touch: cannot make '");
            say(argv[i]);
            say("'\n");
            return 1;
        }
        sys_close(fd);
    }
    return 0;
}

static int c_cat(int argc, char **argv)
{
    char buf[BUF];
    if (argc < 2) {                /* what comes in, out: a pipe's end */
        int n;
        while ((n = sys_read(STDIN, buf, sizeof(buf))) > 0)
            sys_write(STDOUT, buf, (uint32_t)n);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        int fd = sys_open(argv[i], O_READ);
        if (fd < 0) {
            say("cat: there is no file '");
            say(argv[i]);
            say("'\n");
            return 1;
        }
        int n;
        while ((n = sys_read(fd, buf, sizeof(buf))) > 0)
            sys_write(STDOUT, buf, (uint32_t)n);
        sys_close(fd);
    }
    return 0;
}

static int c_cp(int argc, char **argv)
{
    if (!need(argc, 3, "cp <from> <to>"))
        return 1;
    int in = sys_open(argv[1], O_READ);
    if (in < 0) {
        say("cp: there is no file '");
        say(argv[1]);
        say("'\n");
        return 1;
    }
    int out = sys_open(argv[2], O_WRITE | O_CREATE);
    if (out < 0) {
        sys_close(in);
        refused("cp", argv[2]);
        return 2;
    }
    char buf[BUF];
    int n;
    while ((n = sys_read(in, buf, sizeof(buf))) > 0)
        sys_write(out, buf, (uint32_t)n);
    sys_close(in);
    sys_close(out);
    return 0;
}

/* head [-n N] [file]: the first lines */
static int c_head(int argc, char **argv)
{
    int lines = 10, i = 1;
    if (argc > 2 && same(argv[1], "-n")) {
        lines = (int)uatoi(argv[2]);
        i = 3;
    }
    int fd = STDIN;
    if (i < argc && (fd = sys_open(argv[i], O_READ)) < 0) {
        say("head: there is no file '");
        say(argv[i]);
        say("'\n");
        return 1;
    }
    char c;
    while (lines > 0 && sys_read(fd, &c, 1) == 1) {
        sys_write(STDOUT, &c, 1);
        if (c == '\n')
            lines--;
    }
    if (fd != STDIN)
        sys_close(fd);
    return 0;
}

/* wc [file]: lines, words and bytes */
static int c_wc(int argc, char **argv)
{
    int fd = STDIN;
    if (argc > 1 && (fd = sys_open(argv[1], O_READ)) < 0) {
        say("wc: there is no file '");
        say(argv[1]);
        say("'\n");
        return 1;
    }
    uint32_t lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    char buf[BUF];
    int n;
    while ((n = sys_read(fd, buf, sizeof(buf))) > 0)
        for (int k = 0; k < n; k++) {
            char c = buf[k];
            bytes++;
            if (c == '\n')
                lines++;
            int space = c == ' ' || c == '\n' || c == '\t' || c == '\r';
            if (!space && !in_word)
                words++;
            in_word = !space;
        }
    if (fd != STDIN)
        sys_close(fd);
    col(lines, 7);
    col(words, 8);
    col(bytes, 8);
    if (argc > 1) {
        say(" ");
        say(argv[1]);
    }
    say("\n");
    return 0;
}

/* kill <pid|name> [signal] */
static int c_kill(int argc, char **argv)
{
    if (!need(argc, 2, "kill <pid|name> [signal]"))
        return 1;
    int sig = argc > 2 ? (int)uatoi(argv[2]) : 15;
    int pid = (int)uatoi(argv[1]);
    if (argv[1][0] < '0' || argv[1][0] > '9') {
        struct psinfo p;
        pid = 0;
        for (int i = 0; sys_ps(i, &p) == 0; i++) {
            char n[PS_NAME_MAX + 1];
            int k = 0;
            while (k < PS_NAME_MAX && p.name[k]) {
                n[k] = p.name[k];
                k++;
            }
            n[k] = 0;
            if (p.ring3 && same(n, argv[1]))
                pid = p.pid;
        }
    }
    if (pid <= 0 || sys_kill(pid, sig) < 0) {
        say("kill: no such process '");
        say(argv[1]);
        say("'\n");
        return 1;
    }
    return 0;
}

static int c_uname(void)
{
#ifdef __x86_64__
    say("ThreadOS x86-64 (long mode)\n");
#else
    say("ThreadOS i386 (protected mode)\n");
#endif
    return 0;
}

static const char *setting(const char *name, struct cfg_entry *c)
{
    for (uint32_t i = 0; sys_cfg_get(i, c) == 0; i++)
        if (same(c->name, name))
            return c->value;
    return 0;
}

static int c_hostname(void)
{
    struct cfg_entry c;
    const char *v = setting("name", &c);
    say(v ? v : "thread");
    say("\n");
    return 0;
}

/* set: all of them; set <name>: one; set <name> <value>: change it */
static int c_set(int argc, char **argv)
{
    struct cfg_entry c;
    if (argc < 2) {
        for (uint32_t i = 0; sys_cfg_get(i, &c) == 0; i++) {
            pad(c.name, 12);
            say(" = ");
            say(c.value);
            say("\n");
        }
        return 0;
    }
    if (argc == 2) {
        const char *v = setting(argv[1], &c);
        if (!v) {
            say("set: there is no setting '");
            say(argv[1]);
            say("'\n");
            return 1;
        }
        say(argv[1]);
        say(" = ");
        say(v);
        say("\n");
        return 0;
    }
    /* The value is the rest of the line, spaces and all */
    char value[CFG_VAL_MAX];
    int n = 0;
    for (int i = 2; i < argc; i++) {
        if (i > 2 && n < CFG_VAL_MAX - 1)
            value[n++] = ' ';
        for (int k = 0; argv[i][k] && n < CFG_VAL_MAX - 1; k++)
            value[n++] = argv[i][k];
    }
    value[n] = 0;
    int32_t rc = sys_cfg_set(argv[1], value);
    if (rc == -2) {
        say("set: changing settings needs admin - 'sudo set ");
        say(argv[1]);
        say(" ...'\n");
        return 2;
    }
    if (rc < 0 && !same(setting(argv[1], &c) ? c.value : "", value)) {
        say("set: '");
        say(argv[1]);
        say("' does not take that\n");
        return 1;
    }
    say(argv[1]);
    say(" = ");
    say(value);
    say(rc == 0 ? " (saved)\n" : " (until the machine stops: no disk)\n");
    return 0;
}

static int c_help(void);

struct command {
    const char *name;
    int (*run)(int argc, char **argv);
    const char *what;
};

static int w_pwd(int c, char **v) { (void)c; (void)v; return c_pwd(); }
static int w_ps(int c, char **v) { (void)c; (void)v; return c_ps(); }
static int w_free(int c, char **v) { (void)c; (void)v; return c_free(); }
static int w_up(int c, char **v) { (void)c; (void)v; return c_uptime(); }
static int w_date(int c, char **v) { (void)c; (void)v; return c_date(); }
static int w_uname(int c, char **v) { (void)c; (void)v; return c_uname(); }
static int w_host(int c, char **v) { (void)c; (void)v; return c_hostname(); }
static int w_help(int c, char **v) { (void)c; (void)v; return c_help(); }

static const struct command commands[] = {
    { "ls", c_ls, "the files here, or in a directory (-l sizes, -a all)" },
    { "pwd", w_pwd, "where you are" },
    { "cat", c_cat, "what files hold" },
    { "head", c_head, "the first lines (-n N)" },
    { "wc", c_wc, "lines, words and bytes" },
    { "cp", c_cp, "a copy of a file" },
    { "mv", c_mv, "a file under a new name or in another directory" },
    { "rm", c_rm, "files away" },
    { "mkdir", c_mkdir, "a new directory" },
    { "touch", c_touch, "an empty file" },
    { "echo", c_echo, "the words back" },
    { "ps", w_ps, "what runs" },
    { "kill", c_kill, "a program stopped, by number or name" },
    { "free", w_free, "the memory" },
    { "uptime", w_up, "how long the machine has run" },
    { "date", w_date, "the date and the time" },
    { "uname", w_uname, "which system this is" },
    { "hostname", w_host, "the machine's name" },
    { "set", c_set, "the settings: all, one, or a new value" },
    { "help", w_help, "this list" },
};
#define NCOMMANDS ((int)(sizeof(commands) / sizeof(commands[0])))

static int c_help(void)
{
    for (int i = 0; i < NCOMMANDS; i++) {
        pad(commands[i].name, 10);
        say(commands[i].what);
        say("\n");
    }
    return 0;
}

void _start(int argc, char **argv)
{
    /* 'core ls -l': the command is the first word after our own name */
    if (argc < 2) {
        say("usage: core <command> [arguments] - 'core help' lists them\n");
        sys_exit(1);
    }
    for (int i = 0; i < NCOMMANDS; i++)
        if (same(argv[1], commands[i].name))
            sys_exit(commands[i].run(argc - 1, argv + 1));
    say("core: there is no command '");
    say(argv[1]);
    say("'\n");
    sys_exit(1);
}
