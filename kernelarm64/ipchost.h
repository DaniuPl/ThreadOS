#ifndef THREADOS_ARM64_IPCHOST_H
#define THREADOS_ARM64_IPCHOST_H

/* The ARM64 kernel's side of kernel/include/ipchost.h: its threads, the
   descriptor tables of its programs (proc.c) and their page tables */

#include <stdint.h>
#include "arm64.h"
#include "thread.h"
#include "proc.h"

/* A descriptor, as the PC's (kernel64/task64.h): what it stands for,
   where reading stands, close on exec. The kinds have the PC's numbers
   too - the C library's layer asks them (SYS64_FDINFO). */
#define FDK_FREE    0
#define FDK_FILE    1                /* a file of the initrd, to read */
#define FDK_POTOK_R 2                /* a pipe's read end */
#define FDK_POTOK_W 3                /* and its write end */
#define FDK_USOCK   4                /* an end of a Unix socket pair */
#define FDK_MEMFD   5                /* shared memory with a descriptor */
#define FDK_NULL    6                /* /dev/null */

struct fd_entry {
    int kind;
    int slot;
    uint32_t pos;
    uint32_t cloexec;
};

#define ipc_task thread

static inline struct thread *ipc_self(void)
{
    return thread_current();
}

static inline void ipc_block_prepare(void)
{
    thread_current()->woken = 0;
}

/* Sleeps until ipc_wake or 'ticks' hundredths of a second (0: no
   limit) - not at all if woken since ipc_block_prepare */
void ipc_block(uint32_t ticks);

static inline void ipc_wake(struct thread *t)
{
    t->woken = 1;
    thread_wake(t);
}

/* A wait ends early for its program's end, or for a signal to take -
   then the call is made again after the handler (proc.c) */
static inline int ipc_interrupted(void)
{
    struct thread *t = thread_current();
    return t->interrupted || thread_signalled(t);
}

int ipc_fd_new(int kind, int slot);
int ipc_fd_kind(int fd);
int ipc_fd_slot(int fd);
int ipc_fd_copy(int fd, struct fd_entry *out);
int ipc_fd_install(const struct fd_entry *e);
void ipc_fd_drop(struct fd_entry *e);
int ipc_fd_close(int fd);

int ipc_user_readable(uint64_t va, uint64_t len);
int ipc_user_writable(uint64_t va, uint64_t len);

uint64_t ipc_map_area(uint64_t len);
int ipc_map_page(uint64_t va, uint32_t frame);
uint64_t ipc_unmap_page(uint64_t va);

#define IPC_MAP_BASE MMAP_BASE
#define IPC_MAP_MAX  MMAP_TOP

/* RAM is where it is: the kernel's tables map it one to one */
static inline void *phys_ptr(uint64_t frame)
{
    return (void *)(uintptr_t)frame;
}

#endif
