#ifndef THREADOS_PROC64_H
#define THREADOS_PROC64_H

#include <stdint.h>

struct regs;

/* Program arguments.
 *
 * We copy them into the kernel, because at the moment a new image is
 * started the old memory - the one they lie in - no longer exists. The
 * limits are modest and deliberately so: this is meant to be argv, not
 * a channel for shipping data. An argument can still be as long as an
 * address on the web is - the browser hands one to httpget for every
 * page, picture and form it fetches - and the whole set, 2 KiB, sits
 * comfortably on the 16 KiB kernel stack the copy is made on. */
#define ARG_MAX 8
#define ARG_LEN 256

struct args64 {
    int argc;
    char v[ARG_MAX][ARG_LEN];
};

/* Duplicates the current process.
 *
 * In the parent it returns the child's task number, in the child -
 * zero. The child gets a copy of the whole address space and a copy of
 * the register frame, so it returns to ring 3 where the parent does
 * and with the same state. A negative value means it failed. */
int64_t proc64_fork(struct regs *r);

/* How many frames the last fork cost - with shared pages these are
   page tables only, without a single page of the program's memory */
uint32_t proc64_last_fork_cost(void);

/* Replaces the process image with a program from the initrd. On
   success there is nothing to return to - the frame 'r' already
   describes the new program. */
int64_t proc64_exec(struct regs *r, const char *name, uint64_t len,
                    uint64_t argv_ptr);

/* Copies argv from program memory into the kernel */
int proc64_copy_args(uint64_t argv_ptr, struct args64 *out);

/* Runs a program from the initrd as a new process. Returns the task
   number, or -1 when there is no such file. The program finds 'arg' in
   RDI. */
int proc64_spawn(const char *name, const struct args64 *args, int quiet);

/* The same, but the process may be run by any core */
int proc64_spawn_anycore(const char *name, const struct args64 *args,
                          int quiet);

/* Maps the shared memory region with the given key into the current
   process's space. Returns the address at which it is visible, or 0. */
/* Fresh, zeroed memory in the process's mapping area: only the
   addresses are reserved, the pages appear on the first touch.
   Returns the address or 0. */
uint64_t proc64_mmap(uint64_t size);

/* A thread of the current process: the same address space, its own
   stack. Returns its task number or -1. */
int proc64_thread(uint64_t entry, uint64_t arg);

uint64_t proc64_shm(int key, uint64_t size);

/* Maps a file from disk into the process's memory (read-only, the
   pages arrive under its fingers). Returns the address or 0. */
uint64_t proc64_mmap_file(const char *name, uint64_t *size_out,
                           uint64_t write);

/* Flushes to their files the dirty pages of the current process's
   writable mappings. Returns the number of pages flushed. */
uint64_t proc64_msync(void);

#endif
