#ifndef THREADOS_IPCHOST64_H
#define THREADOS_IPCHOST64_H

/* The PC kernel's side of kernel/include/ipchost.h: its tasks, its
   descriptor tables, its page tables */

#include <stdint.h>
#include "task64.h"
#include "paging64.h"
#include "proc64.h"
#include "elf64.h"

#define ipc_task task64

static inline struct task64 *ipc_self(void) { return task64_self(); }
static inline void ipc_block_prepare(void) { task64_block_prepare(); }
static inline void ipc_block(uint32_t ticks) { task64_block(ticks); }
static inline void ipc_wake(struct task64 *t) { task64_wake(t); }
static inline int ipc_interrupted(void) { return task64_signal_pending(); }

static inline int ipc_fd_new(int kind, int slot) { return task64_fd_new(kind, slot); }
static inline int ipc_fd_kind(int fd) { return task64_fd_kind(fd); }
static inline int ipc_fd_slot(int fd) { return task64_fd_slot(fd); }
static inline int ipc_fd_copy(int fd, struct fd_entry *e) { return task64_fd_copy(fd, e); }
static inline int ipc_fd_install(const struct fd_entry *e) { return task64_fd_install(e); }
static inline void ipc_fd_drop(struct fd_entry *e) { task64_fd_drop(e); }
static inline int ipc_fd_close(int fd) { return task64_fd_close(fd); }

static inline int ipc_user_readable(uint64_t va, uint64_t len)
{
    return paging64_user_range(va, len);
}

static inline int ipc_user_writable(uint64_t va, uint64_t len)
{
    return paging64_user_writable(va, len);
}

static inline uint64_t ipc_map_area(uint64_t len) { return proc64_mmap(len); }

/* PG_FILE: a page fork shares rather than copies */
static inline int ipc_map_page(uint64_t va, uint32_t frame)
{
    return paging64_map(va, frame, PG_USER | PG_WRITE | PG_FILE | PG_NX);
}

static inline uint64_t ipc_unmap_page(uint64_t va) { return paging64_unmap(va); }

#define IPC_MAP_BASE USER64_MMAP_BASE
#define IPC_MAP_MAX  USER64_MMAP_MAX

#endif
