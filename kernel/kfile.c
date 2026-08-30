/* The file descriptor layer (the Unix model)
 *
 * It joins four worlds under one read/write interface:
 *   fd 0       - the keyboard (a blocking read)
 *   fd 1, 2    - the console (VGA plus the serial port)
 *   fd >= 3    - files on the PFS disk (each with its own offset)
 *   any fd     - an end of an inter-process pipe (FD_PIPE)
 *
 * Thanks to that a program writes the same way to the screen
 * (write(1,...)), to a file (write(fd,...)) and to a pipe - all that
 * differs is the descriptor's kind.
 */

#include "include/kfile.h"
#include "include/task.h"
#include "include/fs.h"
#include "include/keyboard.h"
#include "include/pipe.h"
#include "include/printk.h"

static void console_write(const char *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        printk("%c", buf[i]);
}

int file_open(const char *name, int flags)
{
    struct file_desc *fds = task_fds();

    int fd = -1;
    for (int i = 3; i < TASK_MAX_FD; i++) {
        if (!fds[i].used) {
            fd = i;
            break;
        }
    }
    if (fd < 0)
        return -1;                       /* no free descriptor */

    int idx = fs_open(task_cwd(), name, flags & O_CREATE);
    if (idx < 0)
        return -1;
    if (flags & O_TRUNC)
        fs_truncate(idx);

    fds[fd].used = 1;
    fds[fd].kind = FD_FILE;
    fds[fd].inode = idx;
    fds[fd].off = 0;
    fds[fd].readable = (flags & O_READ) ? 1 : 0;
    fds[fd].writable = (flags & O_WRITE) ? 1 : 0;
    return fd;
}

/* Closes a descriptor, releasing its resources (a pipe end) */
static void release_fd(struct file_desc *fd)
{
    if (fd->kind == FD_PIPE) {
        if (fd->readable)
            pipe_close_reader(fd->pipe);
        if (fd->writable)
            pipe_close_writer(fd->pipe);
    }
    fd->used = 0;
}

int file_close(int fd)
{
    struct file_desc *fds = task_fds();
    if (fd < 3 || fd >= TASK_MAX_FD || !fds[fd].used)
        return -1;                       /* 0/1/2 stay open */
    release_fd(&fds[fd]);
    return 0;
}

/* Closes all of a task's descriptors - called when it ends, so that
   pipe ends are released (the other side gets EOF) */
void file_close_all(void)
{
    struct file_desc *fds = task_fds();
    for (int i = 0; i < TASK_MAX_FD; i++)
        if (fds[i].used)
            release_fd(&fds[i]);
}

int file_read(int fd, void *buf, uint32_t len)
{
    struct file_desc *fds = task_fds();
    if (fd < 0 || fd >= TASK_MAX_FD || !fds[fd].used || !fds[fd].readable)
        return -1;

    switch (fds[fd].kind) {
    case FD_CONSOLE:
        return keyboard_read(buf, len);  /* fd 0: the keyboard */
    case FD_PIPE:
        return pipe_read(fds[fd].pipe, buf, len);
    case FD_FILE: {
        int n = fs_read_at(fds[fd].inode, fds[fd].off, buf, len);
        if (n > 0)
            fds[fd].off += (uint32_t)n;
        return n;
    }
    }
    return -1;
}

int file_write(int fd, const void *buf, uint32_t len)
{
    struct file_desc *fds = task_fds();
    if (fd < 0 || fd >= TASK_MAX_FD || !fds[fd].used || !fds[fd].writable)
        return -1;

    switch (fds[fd].kind) {
    case FD_CONSOLE:                     /* fd 1/2: the console */
        console_write(buf, len);
        return (int)len;
    case FD_PIPE:
        return pipe_write(fds[fd].pipe, buf, len);
    case FD_FILE: {
        int n = fs_write_at(fds[fd].inode, fds[fd].off, buf, len);
        if (n > 0)
            fds[fd].off += (uint32_t)n;
        return n;
    }
    }
    return -1;
}

int file_seek(int fd, uint32_t off)
{
    struct file_desc *fds = task_fds();
    if (fd < 3 || fd >= TASK_MAX_FD || !fds[fd].used || fds[fd].kind != FD_FILE)
        return -1;
    fds[fd].off = off;
    return (int)off;
}

int file_redirect_stdout(const char *name, int append)
{
    int idx = fs_open(task_cwd(), name, O_CREATE);
    if (idx < 0)
        return -1;

    uint32_t off = 0;
    if (append) {
        int sz = fs_size(idx);
        off = sz > 0 ? (uint32_t)sz : 0;   /* append at the end of the file */
    } else {
        fs_truncate(idx);
    }

    struct file_desc *fds = task_fds();
    fds[1].used = 1;
    fds[1].kind = FD_FILE;
    fds[1].inode = idx;
    fds[1].off = off;
    fds[1].readable = 0;
    fds[1].writable = 1;
    return 0;
}

void file_set_pipe(int fd, int pipe_idx, int writable)
{
    struct file_desc *fds = task_fds();
    fds[fd].used = 1;
    fds[fd].kind = FD_PIPE;
    fds[fd].pipe = pipe_idx;
    fds[fd].readable = writable ? 0 : 1;
    fds[fd].writable = writable ? 1 : 0;
}
