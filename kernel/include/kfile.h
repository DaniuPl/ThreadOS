#ifndef PROTONOS_KFILE_H
#define PROTONOS_KFILE_H

#include <stdint.h>

/* Flags for file_open (matching usys.h) */
#define O_READ   1
#define O_WRITE  2
#define O_CREATE 4
#define O_TRUNC  8

/* The process file descriptor layer. fd 0/1/2 = keyboard/console,
   higher fds refer to PFS files. They operate on the fd table of the
   current task. */
int  file_open(const char *name, int flags);
int  file_close(int fd);
int  file_read(int fd, void *buf, uint32_t len);
int  file_write(int fd, const void *buf, uint32_t len);
int  file_seek(int fd, uint32_t off);

/* Replaces the current task's stdout (fd 1) with a file. append=0
   truncates the file, append=1 appends at the end. Used for
   'cmd > file' / '>>'. */
int  file_redirect_stdout(const char *name, int append);

/* Sets the given fd as a pipe end (writable=1 -> write/stdout,
   0 -> read/stdin). Used when building the pipeline 'cmd1 | cmd2'. */
void file_set_pipe(int fd, int pipe_idx, int writable);

/* Releases all descriptors of the current task (pipe ends) */
void file_close_all(void);

#endif
