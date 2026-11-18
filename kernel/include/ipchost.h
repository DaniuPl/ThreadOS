#ifndef THREADOS_IPCHOST_H
#define THREADOS_IPCHOST_H

/* What kernel/ipc.c needs of the kernel it is built into - by the same
 * names on both:
 *
 *   struct ipc_task, ipc_self()        who is running
 *   ipc_block_prepare(), ipc_block(t)  sleep until ipc_wake, or t ticks
 *   ipc_wake(task)                     (0 = no limit); a wake that comes
 *                                      between the two is not lost
 *   ipc_interrupted()                  the wait is to end (a signal, or
 *                                      the program ending)
 *   struct fd_entry, FDK_*             a descriptor, and the ipc_fd_*
 *                                      calls on the caller's table
 *   ipc_user_readable/_writable        the program's own memory
 *   ipc_map_area, ipc_map_page,        pages at a fresh address of the
 *   ipc_unmap_page, IPC_MAP_BASE/MAX   program's mapping area, shared
 *                                      (fork does not copy them)
 *   phys_ptr(frame)                    where the kernel reaches a frame
 */
#if defined(__aarch64__)
#include "../../kernelarm64/ipchost.h"
#else
#include "../../kernel64/ipchost64.h"
#endif

#endif
