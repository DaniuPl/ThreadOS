#ifndef THREADOS_PROC64_H
#define THREADOS_PROC64_H

#include <stdint.h>

struct regs;

/* Program arguments.
 *
 * We copy them into the kernel, because at the moment a new image is
 * started the old memory - the one they lie in - no longer exists.
 * They are kept packed, one string after another, each with its zero,
 * in a block on the heap just as large as they are. A table of fixed
 * slots did for a browser handing httpget an address and a few
 * options, but a compiler hands its linker thirty paths, and make
 * hands a compiler every file of a target: slots enough for those
 * would not fit the 16 KiB kernel stack a copy is made on.
 *
 * ARG_BYTES is what a program may pass in all - Linux allows more,
 * but it is half of what clang lets a command line grow to before it
 * moves the arguments into a response file (LLVM's
 * commandLineFitsWithinSystemLimits), so a compiler never gets that
 * far. A set that does not fit is refused, never cut short: a program
 * told half of what it was asked to do does the wrong thing quietly. */
#define ARG_BYTES (64 * 1024)

struct args64 {
    int argc;
    uint32_t used;              /* bytes of buf taken, the zeros too */
    uint32_t cap;               /* bytes of buf there are */
    char buf[];
};

/* An empty set with room for cap bytes of strings, or 0 */
struct args64 *args64_new(uint32_t cap);
/* Adds the first len bytes of s as an argument: 0, or -1 when it does
   not fit (the set is left as it was) */
int args64_add(struct args64 *a, const char *s, uint32_t len);
/* A copy of a, or 0 */
struct args64 *args64_dup(const struct args64 *a);
void args64_free(struct args64 *a);

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

/* Replaces the process image with a program - named, or given by its
   path (kernel/prog.c says where each is looked for). On success
   there is nothing to return to - the frame 'r' already describes the
   new program. -1 when there is no such program or it cannot be read,
   -2 when its arguments are more than ARG_BYTES. */
int64_t proc64_exec(struct regs *r, const char *name, uint64_t len,
                    uint64_t argv_ptr);

/* Copies argv from program memory into the kernel: a set on the heap
   in *out, and 0; -1 when the table or a string is not the program's
   memory, -2 when they are more than ARG_BYTES */
int proc64_copy_args(uint64_t argv_ptr, struct args64 **out);

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

/* SYS64_CLONE: a thread on the caller's own stack and thread pointer */
struct clone64 {
    uint64_t entry;       /* where it starts, in ring 3 */
    uint64_t stack;       /* its RSP at that instruction */
    uint64_t tls;         /* its FS base (0 = none) */
    uint64_t parent_tid;  /* where to write its number (0 = nowhere) */
    uint64_t clear_tid;   /* zeroed and woken when it ends (0 = nothing) */
};
int proc64_clone(const struct clone64 *c);

uint64_t proc64_shm(int key, uint64_t size);

/* Maps a file from disk into the process's memory (read-only, the
   pages arrive under its fingers). Returns the address or 0. */
uint64_t proc64_mmap_file(const char *name, uint64_t *size_out,
                           uint64_t write);

/* Flushes to their files the dirty pages of the current process's
   writable mappings. Returns the number of pages flushed. */
uint64_t proc64_msync(void);

#endif
