/* Pipes
 *
 * A pipe is a pair of connected ends from kernel/ipc.c used one way: the
 * read end only reads and the write end only writes (the descriptor's
 * kind, FDK_POTOK_R or FDK_POTOK_W, says which, and kernel/ipc.c holds a
 * descriptor to it). So a pipe waits by sleeping until the other side
 * does something, not in ten-millisecond naps; poll() can wait on it
 * (SYS64_IPC); it holds a quarter of a megabyte; and there are as many
 * as there are free ends, not eight.
 *
 * What a pipe is made of is still who holds it: when the last writer
 * goes the reader gets the end of the data, and when the last reader
 * goes a writer gets an error instead of waiting for ever. A wait can
 * be interrupted by a signal - a signal is delivered only on the return
 * to ring 3, so a wait inside the kernel has to give way to it itself,
 * and is made again after the handler (syscall64.c).
 */

#include "pipe64.h"
#include "../kernel/include/ipc.h"

int pipe64_new(int *reader, int *writer)
{
    return usock64_pipe(reader, writer) < 0 ? -1 : 0;
}

void pipe64_ref(int end)
{
    usock64_ref(end);
}

void pipe64_unref(int end)
{
    usock64_unref(end);
}

int pipe64_read(int end, char *buf, uint32_t len)
{
    if (!len)
        return -1;
    long n = usock64_read(end, buf, len);
    return n == -IPC_EINTR ? PIPE64_EINTR : n < 0 ? -1 : (int)n;
}

int pipe64_write(int end, const char *buf, uint32_t len)
{
    if (!len)
        return -1;
    long n = usock64_write(end, buf, len);
    return n == -IPC_EINTR ? PIPE64_EINTR : n < 0 ? -1 : (int)n;
}
