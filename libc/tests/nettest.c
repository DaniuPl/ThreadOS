/* nettest - BSD sockets, seen from a program
 *
 * Plain POSIX C. The other end of every conversation is the test's own
 * Python on the host (scripts/tests/t_socket.py), which checks what
 * arrived; this side checks what came back and prints one line per
 * finding.
 *
 *   nettest NAME TCPPORT UDPPORT LISTENPORT CLOSEDPORT
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint32_t fnv(const unsigned char *p, size_t n, uint32_t h)
{
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static unsigned char pattern(size_t i, int salt)
{
    return (unsigned char)((i * 31 + (i >> 8) * 7 + salt) & 255);
}

static long ms_since(const struct timespec *t0)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}

static int send_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n) {
        ssize_t k = send(fd, p, n, 0);
        if (k <= 0)
            return -1;
        p += k;
        n -= k;
    }
    return 0;
}

static ssize_t recv_line(int fd, char *buf, size_t max)
{
    size_t n = 0;
    while (n < max - 1) {
        ssize_t k = recv(fd, buf + n, 1, 0);
        if (k <= 0)
            break;
        if (buf[n++] == '\n')
            break;
    }
    buf[n] = 0;
    return n;
}

static struct sockaddr_in server;

/* One exchange: SUM n, n bytes; the server answers with the sum of
   what it got and sends back 'back' bytes of its own pattern */
static int exchange(const char *who, size_t n, size_t back, int salt)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&server, sizeof(server))) {
        printf("%s: connect failed: %s\n", who, strerror(errno));
        return -1;
    }
    unsigned char *data = malloc(n > back ? n : back);
    for (size_t i = 0; i < n; i++)
        data[i] = pattern(i, salt);
    char line[80];
    snprintf(line, sizeof(line), "SUM %zu %zu %d\n", n, back, salt);
    if (send_all(fd, line, strlen(line)) || send_all(fd, data, n)) {
        printf("%s: send failed: %s\n", who, strerror(errno));
        return -1;
    }
    uint32_t mine = fnv(data, n, 2166136261u);
    recv_line(fd, line, sizeof(line));
    unsigned theirs = 0;
    sscanf(line, "OK %x", &theirs);
    size_t got = 0;
    int intact = 1;
    for (;;) {
        ssize_t k = recv(fd, data, back, 0);
        if (k <= 0)
            break;
        for (ssize_t i = 0; i < k; i++)
            intact &= data[i] == pattern(got + i, salt + 1);
        got += k;
    }
    close(fd);
    free(data);
    printf("%s: sent %zu, the server's sum %s mine, got back %zu bytes%s\n",
           who, n, theirs == mine ? "matches" : "DIFFERS from", got,
           intact && got == back ? " intact" : " DAMAGED");
    return 0;
}

static void *beside(void *arg)
{
    (void)arg;
    exchange("a second thread", 50000, 30000, 77);
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        printf("usage: nettest NAME TCPPORT UDPPORT LISTENPORT CLOSEDPORT\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);

    /* A name, through the C library's resolver and the network's DNS */
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    int e = getaddrinfo(argv[1], argv[2], &hints, &res);
    if (e || !res) {
        printf("getaddrinfo %s: %s\n", argv[1], gai_strerror(e));
        return 1;
    }
    memcpy(&server, res->ai_addr, sizeof(server));
    char ip[32];
    inet_ntop(AF_INET, &server.sin_addr, ip, sizeof(ip));
    printf("resolved %s to %s port %d\n", argv[1], ip, ntohs(server.sin_port));
    freeaddrinfo(res);
    e = getaddrinfo("no.such.name.invalid", NULL, &hints, &res);
    printf("a name nobody knows: %s\n", e ? "not found" : "FOUND?");

    /* TCP both ways, and the same at once from a second thread */
    pthread_t t;
    pthread_create(&t, NULL, beside, NULL);
    exchange("tcp", 200000, 100000, 3);
    pthread_join(t, NULL);

    /* A connection that does not wait, then poll and SO_ERROR */
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int r = connect(fd, (struct sockaddr *)&server, sizeof(server));
    int was = errno;
    struct pollfd p = { fd, POLLOUT, 0 };
    int pr = poll(&p, 1, 10000);
    int soerr = -1;
    socklen_t sl = sizeof(soerr);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
    printf("nonblocking connect: %s, then poll %d %s, SO_ERROR %d\n",
           r < 0 && was == EINPROGRESS ? "EINPROGRESS" : "no EINPROGRESS",
           pr, p.revents & POLLOUT ? "writable" : "not writable", soerr);
    /* the server waits for a line; nothing comes, so a read must not wait */
    char c;
    ssize_t k = recv(fd, &c, 1, 0);
    printf("a read with nothing there: %s\n",
           k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) ? "EAGAIN" : "waited");
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(fd, &rs);
    struct timeval tv = { 0, 300000 };
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int sr = select(fd + 1, &rs, NULL, NULL, &tv);
    long waited = ms_since(&t0);
    printf("select on a quiet socket: %d after %s\n", sr,
           waited >= 250 && waited < 3000 ? "about 300 ms" : "the wrong time");
    close(fd);

    /* Nobody listens there */
    struct sockaddr_in closed = server;
    closed.sin_port = htons(atoi(argv[5]));
    fd = socket(AF_INET, SOCK_STREAM, 0);
    r = connect(fd, (struct sockaddr *)&closed, sizeof(closed));
    printf("a port nobody listens on: %s\n",
           r < 0 && errno == ECONNREFUSED ? "ECONNREFUSED" : strerror(errno));
    close(fd);

    /* UDP: three datagrams, three answers */
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in echo = server;
    echo.sin_port = htons(atoi(argv[3]));
    int answers = 0;
    for (int i = 1; i <= 3; i++) {
        char msg[32], back[64];
        snprintf(msg, sizeof(msg), "datagram %d", i);
        sendto(u, msg, strlen(msg), 0, (struct sockaddr *)&echo, sizeof(echo));
        struct pollfd up = { u, POLLIN, 0 };
        if (poll(&up, 1, 5000) != 1)
            continue;
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(u, back, sizeof(back) - 1, 0,
                             (struct sockaddr *)&from, &fl);
        back[n > 0 ? n : 0] = 0;
        char want[64];
        snprintf(want, sizeof(want), "echo: %s", msg);
        answers += n > 0 && !strcmp(back, want) &&
                   from.sin_port == echo.sin_port;
    }
    printf("udp: %d of 3 datagrams answered, from the port they went to\n",
           answers);
    struct pollfd up = { u, POLLIN, 0 };
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pr = poll(&up, 1, 300);
    waited = ms_since(&t0);
    printf("poll on a quiet datagram socket: %d after %s\n", pr,
           waited >= 250 && waited < 3000 ? "about 300 ms" : "the wrong time");
    close(u);

    /* A server: one connection from the host */
    int l = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in any = { .sin_family = AF_INET,
                               .sin_port = htons(atoi(argv[4])) };
    if (bind(l, (struct sockaddr *)&any, sizeof(any)) || listen(l, 4)) {
        printf("listen: %s\n", strerror(errno));
        return 1;
    }
    printf("listening on %s\n", argv[4]);
    struct sockaddr_in peer;
    socklen_t pl = sizeof(peer);
    int a = accept(l, (struct sockaddr *)&peer, &pl);
    char line[128];
    recv_line(a, line, sizeof(line));
    for (char *q = line; *q; q++)
        if (*q >= 'a' && *q <= 'z')
            *q -= 32;
    send_all(a, line, strlen(line));
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    struct sockaddr_in me;
    socklen_t ml = sizeof(me);
    getsockname(a, (struct sockaddr *)&me, &ml);
    printf("accepted a connection from %s, to my port %d\n", ip,
           ntohs(me.sin_port));
    close(a);
    close(l);
    printf("done\n");
    return 0;
}
