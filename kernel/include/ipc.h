#ifndef THREADOS_IPC64_H
#define THREADOS_IPC64_H

#include <stdint.h>

/* Talking between processes the way a program built in several of them
   does (WebKit: a process for the window, one for each page, one for
   the network): pairs of connected Unix sockets that carry descriptors
   along with the bytes, and shared memory with a descriptor of its own
   that can be sent over them and mapped on the other side. The calls
   come through SYS64_IPC (kernel64/syscall64.c); libc/overlay turns
   socketpair, sendmsg, recvmsg, poll, memfd_create and mmap into them. */

/* SYS64_IPC's operations */
#define IPC_PAIR     0   /* (type, int sv[2]) -> 0: SOCK_STREAM 1 or SOCK_SEQPACKET 5 */
#define IPC_SEND     1   /* (fd, struct ipc_io *) -> bytes */
#define IPC_RECV     2   /* (fd, struct ipc_io *) -> bytes, 0 at the end */
#define IPC_POLL     3   /* (fd) -> IPC_IN | IPC_OUT | IPC_HUP */
#define IPC_WAIT     4   /* (int fds[], n, ms) -> until one is ready, or the time;
                            each entry is fd | (IPC_IN | IPC_OUT wanted) << 16 */
#define IPC_SHUTDOWN 5   /* (fd, how) */
#define IPC_MEMFD    6   /* () -> a descriptor for shared memory of size 0 */
#define IPC_MAP      7   /* (fd, offset, length) -> its pages at an address */
#define IPC_UNMAP    8   /* (address, length) -> those pages let go */
#define IPC_NULL     9   /* () -> a descriptor that is /dev/null */

#define IPC_IN   1                 /* a read would not wait: data, or the end */
#define IPC_OUT  2
#define IPC_HUP  4                 /* the peer is gone */
#define IPC_DATA 8                 /* bytes are waiting (a pipe's POLLIN) */

/* One send or receive. 'fds' are descriptor numbers: those to send,
   or room for those received; 'nfds' how many, in and out. 'len' is
   the room in 'buf' going in, and after a receive the length of the
   whole message, which a seqpacket's can exceed (MSG_TRUNC). */
struct ipc_io {
    uint64_t buf;
    uint32_t len;
    uint32_t flags;                /* IPC_DONTWAIT, IPC_PEEK */
    uint64_t fds;
    uint32_t nfds;
    uint32_t truncated;            /* out: descriptors that did not fit */
};
#define IPC_DONTWAIT 1
#define IPC_PEEK     2

/* The most bytes one message carries, and descriptors with it */
#define IPC_MSG_MAX  (64 * 1024)
#define IPC_FDS_MAX  16

/* Errors come back as the negated Linux numbers, for libc/overlay */
#define IPC_EINTR    4
#define IPC_EBADF    9
#define IPC_EAGAIN   11
#define IPC_ENOMEM   12
#define IPC_EINVAL   22
#define IPC_EMFILE   24
#define IPC_EPIPE    32
#define IPC_EMSGSIZE 90

long ipc64_call(uint64_t op, uint64_t a, uint64_t b, uint64_t c);

/* A pipe: a stream pair whose first end only reads and second only
   writes (pipe64.c); 0, or -IPC_ENOMEM */
int usock64_pipe(int *reader, int *writer);

/* A socket end through read() and write(): bytes only */
long usock64_read(int end, void *buf, uint32_t len);
long usock64_write(int end, const void *buf, uint32_t len);

/* The holds descriptors keep on them (task64.c) */
void usock64_ref(int end);
void usock64_unref(int end);
void memfd64_ref(int m);
void memfd64_unref(int m);

/* What fstat and FIONREAD say of an end: bytes waiting, SOCK_STREAM 1
   or SOCK_SEQPACKET 5 */
void usock64_info(int end, uint32_t *queued, int *type);

/* A memfd's size: what fstat says, what ftruncate changes */
uint64_t memfd64_size(int m);
int memfd64_resize(int m, uint64_t size);

/* read() and write() at 'pos' of a memfd */
long memfd64_rw(int m, uint32_t pos, void *buf, uint32_t len, int write);

#endif
