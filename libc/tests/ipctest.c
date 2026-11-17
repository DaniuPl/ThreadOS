/* ipctest - processes talking: Unix socket pairs, descriptors sent
 * over them, and shared memory (kernel64/ipc64.c, libc/overlay's
 * unix.c)
 *
 * Plain POSIX and Linux C: scripts/tests/t_ipc.py builds it a second
 * time on the test machine against glibc, and the two have to print
 * the same lines. Lines that start with '~' are measurements, which
 * only ThreadOS's run is held to.
 *
 * What it goes through is what a web engine split into processes
 * does: a pair made before fork; descriptors - a pipe's end, a memfd,
 * another socket's end - sent with SCM_RIGHTS to a child that did not
 * have them; both sides mapping the memfd and seeing each other's
 * writes; seqpacket messages kept whole and their truncation told;
 * a stream gathering what came; a megabyte between processes; poll
 * waking when there is something, sleeping when not; shutdown, EOF,
 * EPIPE, and how fast a message goes there and back. And pipes, which
 * a main loop waits on the same way: what poll says of each end, a
 * pipe that may not wait, a hundred of them at once, a megabyte
 * through one from a child.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failed;

static void say(const char *what, int ok)
{
    printf("%s: %s\n", what, ok ? "yes" : "no");
    failed |= !ok;
}

static long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

static const char *events(short r)
{
    /* (two in one printf: each call its own buffer) */
    static char ring[4][32];
    static int turn;
    char *s = ring[turn++ & 3];
    s[0] = 0;
    if (r & POLLIN)
        strcat(s, " IN");
    if (r & POLLOUT)
        strcat(s, " OUT");
    if (r & POLLHUP)
        strcat(s, " HUP");
    if (r & POLLERR)
        strcat(s, " ERR");
    return s[0] ? s + 1 : "none";
}

/* Descriptors with a few bytes, over a socket */
static int send_fds(int sock, const char *text, const int *fds, int n)
{
    char ctl[CMSG_SPACE(8 * sizeof(int))];
    memset(ctl, 0, sizeof(ctl));
    struct iovec v = { (void *)text, strlen(text) };
    struct msghdr m = { 0 };
    m.msg_iov = &v;
    m.msg_iovlen = 1;
    if (n) {
        m.msg_control = ctl;
        m.msg_controllen = CMSG_SPACE(n * sizeof(int));
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(n * sizeof(int));
        memcpy(CMSG_DATA(c), fds, n * sizeof(int));
    }
    return (int)sendmsg(sock, &m, MSG_NOSIGNAL);
}

/* What arrives, and up to 'room' descriptors with it; how many came
   through '*n', the message's flags through '*flags' */
static int recv_fds(int sock, char *buf, int len, int *fds, int room, int *n,
                    int *flags, socklen_t ctl_room)
{
    char ctl[CMSG_SPACE(8 * sizeof(int))];
    struct iovec v = { buf, (size_t)len };
    struct msghdr m = { 0 };
    m.msg_iov = &v;
    m.msg_iovlen = 1;
    m.msg_control = ctl;
    m.msg_controllen = ctl_room ? ctl_room : CMSG_SPACE(room * sizeof(int));
    int r = (int)recvmsg(sock, &m, 0);
    *n = 0;
    *flags = m.msg_flags;
    if (r < 0)
        return r;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int k = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            memcpy(fds + *n, CMSG_DATA(c), k * sizeof(int));
            *n += k;
        }
    if (r < len)
        buf[r] = 0;
    return r;
}

static void streams(void)
{
    int sv[2];
    say("socketpair(AF_UNIX, SOCK_STREAM)",
        socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    char buf[256];

    write(sv[0], "hello", 5);
    int r = (int)read(sv[1], buf, sizeof(buf));
    printf("stream read: %d bytes, \"%.*s\"\n", r, r > 0 ? r : 0, buf);

    /* two writes come out of one read, as a stream does */
    write(sv[1], "abc", 3);
    write(sv[1], "def", 3);
    r = (int)read(sv[0], buf, sizeof(buf));
    printf("two writes, one read: %d bytes, \"%.*s\"\n", r, r > 0 ? r : 0, buf);

    /* and one write in two reads */
    write(sv[0], "0123456789", 10);
    r = (int)read(sv[1], buf, 4);
    printf("one write, a read of 4: \"%.*s\"", r > 0 ? r : 0, buf);
    r = (int)read(sv[1], buf, sizeof(buf));
    printf(", then the rest: \"%.*s\"\n", r > 0 ? r : 0, buf);

    /* peeking leaves it there */
    send(sv[0], "peekaboo", 8, 0);
    r = (int)recv(sv[1], buf, 4, MSG_PEEK);
    printf("peek: \"%.*s\"", r > 0 ? r : 0, buf);
    r = (int)recv(sv[1], buf, sizeof(buf), 0);
    printf(", read: \"%.*s\"\n", r > 0 ? r : 0, buf);

    /* nothing there: a call that may not wait says so */
    errno = 0;
    r = (int)recv(sv[1], buf, sizeof(buf), MSG_DONTWAIT);
    say("an empty socket with MSG_DONTWAIT says EAGAIN", r < 0 && errno == EAGAIN);
    int fl = fcntl(sv[1], F_GETFL);
    fcntl(sv[1], F_SETFL, fl | O_NONBLOCK);
    errno = 0;
    r = (int)read(sv[1], buf, sizeof(buf));
    say("and so does read() once O_NONBLOCK is set", r < 0 && errno == EAGAIN);
    say("fcntl(F_GETFL) shows O_NONBLOCK",
        (fcntl(sv[1], F_GETFL) & O_NONBLOCK) != 0);
    fcntl(sv[1], F_SETFL, fl);

    /* readv and writev */
    struct iovec out[3] = { { "one ", 4 }, { "two ", 4 }, { "three", 5 } };
    writev(sv[0], out, 3);
    char a[5], b[20];
    struct iovec in[2] = { { a, 5 }, { b, sizeof(b) } };
    r = (int)readv(sv[1], in, 2);
    printf("writev of three, readv into two: %d bytes, \"%.5s\" + \"%.*s\"\n",
           r, a, r > 5 ? r - 5 : 0, b);

    int n = 0;
    write(sv[0], "1234567", 7);
    ioctl(sv[1], FIONREAD, &n);
    printf("FIONREAD after 7 bytes: %d\n", n);
    read(sv[1], buf, sizeof(buf));

    int type = 0;
    socklen_t len = sizeof(type);
    getsockopt(sv[0], SOL_SOCKET, SO_TYPE, &type, &len);
    printf("SO_TYPE: %d (SOCK_STREAM is %d)\n", type, SOCK_STREAM);
    struct stat st;
    fstat(sv[0], &st);
    say("fstat says a socket", S_ISSOCK(st.st_mode));

    /* full: a write that may not wait stops */
    char block[4096];
    memset(block, 'x', sizeof(block));
    long sent = 0;
    for (int i = 0; i < 4096; i++) {
        r = (int)send(sv[0], block, sizeof(block), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (r < 0)
            break;
        sent += r;
    }
    say("a full socket with MSG_DONTWAIT says EAGAIN", r < 0 && errno == EAGAIN);
    printf("~queued before it was full: %ld bytes\n", sent);
    long got = 0;
    while (got < sent) {
        r = (int)recv(sv[1], block, sizeof(block), MSG_DONTWAIT);
        if (r <= 0)
            break;
        got += r;
    }
    say("and all of it reads back out", got == sent);

    /* shutdown: one way closed, the other still open */
    shutdown(sv[0], SHUT_WR);
    r = (int)read(sv[1], buf, sizeof(buf));
    printf("after shutdown(SHUT_WR) the other end reads: %d\n", r);
    write(sv[1], "back", 4);
    r = (int)read(sv[0], buf, sizeof(buf));
    printf("while the other way still works: \"%.*s\"\n", r > 0 ? r : 0, buf);
    close(sv[0]);
    close(sv[1]);
}

static void seqpackets(void)
{
    int sv[2];
    say("socketpair(AF_UNIX, SOCK_SEQPACKET)",
        socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
    char buf[512], big[300];
    memset(big, 'b', sizeof(big));

    send(sv[0], "first", 5, 0);
    send(sv[0], big, sizeof(big), 0);
    send(sv[0], "", 0, 0);
    send(sv[0], "last", 4, 0);
    printf("seqpacket messages read one by one:");
    for (int i = 0; i < 4; i++)
        printf(" %d", (int)recv(sv[1], buf, sizeof(buf), 0));
    printf("\n");

    /* a message longer than the room: cut, and said to be */
    char hundred[100];
    memset(hundred, 'h', sizeof(hundred));
    send(sv[0], hundred, sizeof(hundred), 0);
    send(sv[0], "next", 4, 0);
    struct iovec v = { buf, 40 };
    struct msghdr m = { 0 };
    m.msg_iov = &v;
    m.msg_iovlen = 1;
    int r = (int)recvmsg(sv[1], &m, 0);
    printf("100 bytes into 40: %d, MSG_TRUNC %s\n", r,
           m.msg_flags & MSG_TRUNC ? "set" : "not set");
    r = (int)recv(sv[1], buf, sizeof(buf), 0);
    printf("and the next message is whole: %d \"%.*s\"\n", r, r > 0 ? r : 0, buf);
    send(sv[0], hundred, sizeof(hundred), 0);
    r = (int)recv(sv[1], buf, 10, MSG_TRUNC);
    printf("recv(MSG_TRUNC) tells the real length: %d\n", r);

    /* writev is one message */
    struct iovec out[3] = { { "a", 1 }, { "bb", 2 }, { "ccc", 3 } };
    writev(sv[0], out, 3);
    r = (int)recv(sv[1], buf, sizeof(buf), 0);
    printf("writev of three is one message: %d \"%.*s\"\n", r, r > 0 ? r : 0, buf);

    int type = 0;
    socklen_t len = sizeof(type);
    getsockopt(sv[0], SOL_SOCKET, SO_TYPE, &type, &len);
    printf("SO_TYPE: %d (SOCK_SEQPACKET is %d)\n", type, SOCK_SEQPACKET);
    close(sv[0]);
    close(sv[1]);
}

static void memfds(void)
{
    int fd = memfd_create("ipctest", MFD_CLOEXEC);
    say("memfd_create", fd >= 0);
    write(fd, "abc", 3);
    struct stat st;
    fstat(fd, &st);
    printf("after writing 3 bytes: a regular file %s, size %ld\n",
           S_ISREG(st.st_mode) ? "yes" : "no", (long)st.st_size);
    lseek(fd, 0, SEEK_SET);
    char buf[16] = { 0 };
    int r = (int)read(fd, buf, sizeof(buf) - 1);
    printf("read back from the start: %d \"%s\"\n", r, buf);

    say("ftruncate to 64 KiB", ftruncate(fd, 65536) == 0);
    fstat(fd, &st);
    printf("size now: %ld\n", (long)st.st_size);
    char *one = mmap(0, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    char *two = mmap(0, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    say("mapped twice, MAP_SHARED", one != MAP_FAILED && two != MAP_FAILED);
    if (one == MAP_FAILED || two == MAP_FAILED)
        return;
    printf("the mapping starts with what was written: \"%.3s\", then %d\n",
           one, one[3]);
    strcpy(one + 5000, "through the first");
    printf("written through one, read through the other: \"%s\"\n", two + 5000);

    char *mine = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 4096);
    say("mapped MAP_PRIVATE from 4096", mine != MAP_FAILED);
    if (mine != MAP_FAILED) {
        printf("it sees what is there: \"%s\"\n", mine + 904);
        strcpy(mine + 904, "private!");
        printf("its own writes stay its own: \"%s\"\n", two + 5000);
        munmap(mine, 8192);
    }

    munmap(one, 65536);
    printf("after munmap of one, the other still reads: \"%s\"\n", two + 5000);
    lseek(fd, 5000, SEEK_SET);
    memset(buf, 0, sizeof(buf));
    read(fd, buf, 7);
    printf("and read() sees the mapping's write: \"%s\"\n", buf);
    munmap(two, 65536);
    close(fd);
}

/* --- across fork ---------------------------------------------------- */

static void child(int sock)
{
    char buf[128];
    int fds[8], n, flags;
    int r = recv_fds(sock, buf, sizeof(buf), fds, 8, &n, &flags, 0);
    if (r <= 0 || n != 3)
        _exit(10 + n);
    int pipe_w = fds[0], mem = fds[1], other = fds[2];
    struct stat st;
    fstat(mem, &st);
    char *shared = mmap(0, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        mem, 0);
    if (shared == MAP_FAILED)
        _exit(20);
    /* what the parent left there, answered beside it */
    char seen[64];
    snprintf(seen, sizeof(seen), "%s", shared);
    snprintf(shared + 4096, 128, "the child read \"%s\" (%ld bytes)",
             seen, (long)st.st_size);
    const char *line = "the child wrote this into the pipe";
    write(pipe_w, line, strlen(line));
    close(pipe_w);
    /* over the socket that came in the message */
    send(other, "over the socket that was sent", 29, MSG_NOSIGNAL);
    close(other);

    /* a megabyte, in pieces of every size */
    static char mb[1 << 20];
    unsigned seed = 7;
    for (int i = 0; i < (int)sizeof(mb); i++) {
        seed = seed * 1103515245 + 12345;
        mb[i] = (char)(seed >> 16);
    }
    int at = 0;
    for (int k = 1; at < (int)sizeof(mb); k = k * 7 % 65521 + 1) {
        int part = k < (int)sizeof(mb) - at ? k : (int)sizeof(mb) - at;
        r = (int)write(sock, mb + at, part);
        if (r <= 0)
            _exit(30);
        at += r;
    }

    /* ping-pong */
    for (;;) {
        r = (int)read(sock, buf, 1);
        if (r <= 0)
            break;
        write(sock, buf, 1);
    }
    munmap(shared, st.st_size);
    _exit(0);
}

static void *later(void *arg)
{
    int fd = *(int *)arg;
    struct timespec t = { 0, 150 * 1000 * 1000 };
    nanosleep(&t, 0);
    write(fd, "!", 1);
    return 0;
}

static void processes(void)
{
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        close(sv[0]);
        child(sv[1]);
    }
    close(sv[1]);
    int sock = sv[0];

    /* made after the fork: the child has these only by being sent them */
    int p[2], other[2];
    pipe(p);
    socketpair(AF_UNIX, SOCK_STREAM, 0, other);
    int mem = memfd_create("shared", 0);
    ftruncate(mem, 3 * 4096);
    char *shared = mmap(0, 3 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mem, 0);
    strcpy(shared, "the parent was here");
    int give[3] = { p[1], mem, other[1] };
    int r = send_fds(sock, "three descriptors", give, 3);
    printf("sent 3 descriptors with %d bytes\n", r);
    /* the parent's own copies go: what the child got keeps them alive */
    close(p[1]);
    close(other[1]);
    close(mem);

    char buf[256];
    int got = 0;
    while ((r = (int)read(p[0], buf + got, sizeof(buf) - 1 - got)) > 0)
        got += r;
    buf[got] = 0;
    printf("the pipe, until the child closed its end: \"%s\"\n", buf);
    r = (int)read(other[0], buf, sizeof(buf) - 1);
    printf("the sent socket: \"%.*s\"\n", r > 0 ? r : 0, buf);
    close(p[0]);
    close(other[0]);

    static char mb[1 << 20], want[1 << 20];
    unsigned seed = 7;
    for (int i = 0; i < (int)sizeof(want); i++) {
        seed = seed * 1103515245 + 12345;
        want[i] = (char)(seed >> 16);
    }
    int at = 0;
    while (at < (int)sizeof(mb)) {
        r = (int)read(sock, mb + at, sizeof(mb) - at);
        if (r <= 0)
            break;
        at += r;
    }
    printf("a megabyte from the child: %d bytes, %s\n", at,
           memcmp(mb, want, sizeof(mb)) ? "different" : "intact");
    /* by now the child has written into the shared memory, too */
    printf("in the shared memory: %s\n", shared + 4096);

    long t0 = now_us();
    int rounds = 0;
    for (; rounds < 500; rounds++) {
        if (write(sock, "p", 1) != 1 || read(sock, buf, 1) != 1)
            break;
    }
    long per = (now_us() - t0) / (rounds ? rounds : 1);
    printf("500 messages there and back: %s\n", rounds == 500 ? "yes" : "no");
    printf("~round trip: %ld us\n", per);

    /* poll, where the child is gone */
    struct pollfd pf = { sock, POLLIN | POLLOUT, 0 };
    poll(&pf, 1, 0);
    printf("poll with the child still there: %s\n", events(pf.revents));
    close(sock);                 /* the child's read ends, and it with it */
    int status = 0;
    waitpid(pid, &status, 0);
    printf("the child ended with %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    munmap(shared, 3 * 4096);
}

static void polling(void)
{
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    struct pollfd pf = { sv[1], POLLIN | POLLOUT, 0 };
    poll(&pf, 1, 0);
    printf("poll on a fresh pair: %s\n", events(pf.revents));
    write(sv[0], "x", 1);
    poll(&pf, 1, 0);
    printf("with a byte waiting: %s\n", events(pf.revents));
    char c;
    read(sv[1], &c, 1);

    pf.events = POLLIN;
    long t0 = now_us();
    int r = poll(&pf, 1, 120);
    long took = (now_us() - t0) / 1000;
    printf("poll for input with nothing coming: %d\n", r);
    say("and it waited for its timeout", took >= 100 && took < 2000);
    printf("~poll(120 ms) took: %ld ms\n", took);

    pthread_t t;
    pthread_create(&t, 0, later, &sv[0]);
    t0 = now_us();
    r = poll(&pf, 1, -1);
    took = (now_us() - t0) / 1000;
    printf("poll with no timeout, woken by a write from a thread: %d, %s\n",
           r, events(pf.revents));
    say("soon after the write", took >= 100 && took < 1000);
    printf("~woken after: %ld ms (the write came at 150)\n", took);
    pthread_join(t, 0);
    read(sv[1], &c, 1);

    close(sv[0]);
    pf.events = POLLIN | POLLOUT;
    poll(&pf, 1, 0);
    printf("poll once the other end is closed: %s\n", events(pf.revents));
    r = (int)read(sv[1], &c, 1);
    printf("read there: %d\n", r);
    errno = 0;
    r = (int)send(sv[1], "x", 1, MSG_NOSIGNAL);
    say("send there says EPIPE", r < 0 && errno == EPIPE);
    close(sv[1]);
}

static void truncated_rights(void)
{
    int sv[2];
    socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv);
    int p[2];
    pipe(p);
    int give[2] = { p[0], p[1] };
    send_fds(sv[0], "two", give, 2);
    char buf[16];
    int fds[8], n, flags;
    int r = recv_fds(sv[1], buf, sizeof(buf), fds, 1, &n, &flags,
                     CMSG_LEN(sizeof(int)));
    printf("two descriptors, room for one: %d bytes, %d descriptor, "
           "MSG_CTRUNC %s\n", r, n, flags & MSG_CTRUNC ? "set" : "not set");
    if (n == 1) {
        /* the one that came is the pipe's read end, and it works */
        write(p[1], "ok", 2);
        r = (int)read(fds[0], buf, 2);
        printf("the one received reads the pipe: \"%.*s\"\n", r > 0 ? r : 0, buf);
        close(fds[0]);
    }
    close(p[0]);
    close(p[1]);
    close(sv[0]);
    close(sv[1]);
}

static void *write_later(void *arg)
{
    int fd = *(int *)arg;
    struct timespec t = { 0, 150 * 1000 * 1000 };
    nanosleep(&t, 0);
    write(fd, "!", 1);
    return 0;
}

static void pipes(void)
{
    int p[2];
    say("pipe", pipe(p) == 0);
    struct pollfd pf[2] = { { p[0], POLLIN | POLLOUT, 0 },
                            { p[1], POLLIN | POLLOUT, 0 } };
    poll(pf, 2, 0);
    printf("poll on a fresh pipe: read end %s, write end %s\n",
           events(pf[0].revents), events(pf[1].revents));
    write(p[1], "data", 4);
    poll(pf, 2, 0);
    printf("with bytes in it: read end %s, write end %s\n",
           events(pf[0].revents), events(pf[1].revents));
    char buf[4096];
    int r = (int)read(p[0], buf, sizeof(buf));
    printf("read: %d \"%.*s\"\n", r, r > 0 ? r : 0, buf);

    fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
    errno = 0;
    r = (int)read(p[0], buf, sizeof(buf));
    say("an empty pipe with O_NONBLOCK says EAGAIN", r < 0 && errno == EAGAIN);
    fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) & ~O_NONBLOCK);

    /* poll sleeps until a thread writes */
    pthread_t t;
    pf[0].events = POLLIN;
    pthread_create(&t, 0, write_later, &p[1]);
    long t0 = now_us();
    r = poll(pf, 1, 5000);
    long took = (now_us() - t0) / 1000;
    printf("poll on a read end, woken by a write from a thread: %d, %s\n", r,
           events(pf[0].revents));
    say("soon after the write", took >= 100 && took < 1000);
    pthread_join(t, 0);
    read(p[0], buf, 1);

    close(p[1]);
    pf[0].events = POLLIN | POLLOUT;
    poll(pf, 1, 0);
    printf("once the write end is closed the read end says: %s, "
           "and reads %d\n", events(pf[0].revents), (int)read(p[0], buf, 1));
    close(p[0]);

    pipe(p);
    close(p[0]);
    pf[1].fd = p[1];
    pf[1].events = POLLIN | POLLOUT;
    poll(&pf[1], 1, 0);
    printf("once the read end is closed the write end says: %s\n",
           events(pf[1].revents));
    signal(SIGPIPE, SIG_IGN);
    errno = 0;
    r = (int)write(p[1], "x", 1);
    say("and a write there says EPIPE", r < 0 && errno == EPIPE);
    signal(SIGPIPE, SIG_DFL);
    close(p[1]);

    /* one that may not wait, filled */
    say("pipe2(O_NONBLOCK)", pipe2(p, O_NONBLOCK) == 0);
    memset(buf, 'p', sizeof(buf));
    long held = 0;
    for (int i = 0; i < 4096; i++) {
        r = (int)write(p[1], buf, sizeof(buf));
        if (r < 0)
            break;
        held += r;
    }
    say("a full pipe that may not wait says EAGAIN", r < 0 && errno == EAGAIN);
    printf("~a pipe holds: %ld bytes\n", held);
    close(p[0]);
    close(p[1]);

    /* a hundred at once (there used to be eight in the whole system) */
    int many[100][2], made = 0;
    for (int i = 0; i < 100; i++)
        made += pipe(many[i]) == 0;
    int through = 0;
    for (int i = 0; i < made; i++) {
        char c = (char)i, back = 0;
        write(many[i][1], &c, 1);
        through += read(many[i][0], &back, 1) == 1 && back == c;
    }
    for (int i = 0; i < made; i++) {
        close(many[i][0]);
        close(many[i][1]);
    }
    printf("a hundred pipes open at once, a byte through each: %d of %d\n",
           through, made);

    /* a megabyte from a child, and a message there and back */
    int down[2], up[2];
    pipe(down);
    pipe(up);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        close(down[0]);
        close(up[1]);
        static char mb[1 << 20];
        for (int i = 0; i < (int)sizeof(mb); i++)
            mb[i] = (char)(i * 7 + i / 4096);
        int at = 0;
        while (at < (int)sizeof(mb)) {
            r = (int)write(down[1], mb + at, sizeof(mb) - at);
            if (r <= 0)
                _exit(1);
            at += r;
        }
        char c;
        while (read(up[0], &c, 1) == 1)
            write(down[1], &c, 1);
        _exit(0);
    }
    close(down[1]);
    close(up[0]);
    static char mb[1 << 20];
    int at = 0;
    while (at < (int)sizeof(mb)) {
        r = (int)read(down[0], mb + at, sizeof(mb) - at);
        if (r <= 0)
            break;
        at += r;
    }
    int same = at == (int)sizeof(mb);
    for (int i = 0; i < at && same; i++)
        same = mb[i] == (char)(i * 7 + i / 4096);
    printf("a megabyte through a pipe from a child: %d bytes, %s\n", at,
           same ? "intact" : "different");
    t0 = now_us();
    int rounds = 0;
    for (; rounds < 500; rounds++)
        if (write(up[1], "p", 1) != 1 || read(down[0], buf, 1) != 1)
            break;
    printf("500 bytes there and back through two pipes: %s\n",
           rounds == 500 ? "yes" : "no");
    printf("~pipe round trip: %ld us\n", (now_us() - t0) / (rounds ? rounds : 1));
    close(up[1]);
    r = (int)read(down[0], buf, 1);
    printf("the child's end of the data: %d\n", r);
    int status = 0;
    waitpid(pid, &status, 0);
    printf("the child ended with %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    close(down[0]);
}

int main(void)
{
    setvbuf(stdout, 0, _IOLBF, 0);
    streams();
    seqpackets();
    memfds();
    truncated_rights();
    polling();
    processes();
    pipes();
    printf("everything as expected: %s\n", failed ? "no" : "yes");
    return failed;
}
