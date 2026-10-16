#ifndef THREADOS_EXEC_H
#define THREADOS_EXEC_H

#include <stdint.h>

/* Where the shared library sits in a process's address space.
   Below the mapping area (0xB0000000) and above the heap, which ends
   at its doorstep. Must agree with user/libthread.ld. */
#define USER_LIB_BASE  0xAF000000u

/* Runs an ELF program from the initrd as a user process (ring 3, its
   own address space). The argument is a command line (program plus
   arguments separated by spaces). Returns a pid or -1. */
int exec_spawn(const char *cmdline);

/* The same, but every core may run the process. Its kernel side stays
   under the big kernel lock; what runs in parallel is the computing it
   does in ring 3. */
int exec_spawn_anycore(const char *cmdline);

/* Like exec_spawn, but the process's stdout (fd 1) is redirected to
   the file outfile. append=0 truncates the file, append=1 appends (>>). */
int exec_spawn_redirect(const char *cmdline, const char *outfile, int append);

/* The same, plus a pipe into the program's stdin.
 *
 * The caller gets the write end and can put into it whatever a person
 * types - which is the whole difference between running a program and
 * talking to one. '*in_pipe' comes back as the pipe's index; the
 * caller is then its writer and has to close it (pipe_close_writer),
 * or the program will wait for more input forever. */
int exec_spawn_talk(const char *cmdline, const char *outfile, int *in_pipe);

/* Runs a multi-stage pipeline 'a | b | c | ...' (parallel processes).
   'pipeline' is the whole line with '|' separators. When outfile is
   not NULL, the last stage's stdout also goes to the file. Returns the
   pid of the last stage or -1. */
int exec_spawn_pipeline(const char *pipeline, const char *outfile);

/* fork: forks the current process. The child gets a clone of the
   parent's address space (copy-on-write) and returns from the syscall
   at the same point of the program. Returns the child's pid (in the
   parent), zero (in the child) or -1. 'r' is the syscall register
   frame. */
struct regs;
int process_fork(struct regs *r);

/* exec: replaces the current process's program (pid, descriptors and
   working directory stay). On success it does not return; -1 on error. */
int process_exec(const char *cmdline);

/* Demand paging: allocates a frame for an address belonging to the
   heap or the mmap area of the current process. 1 = the page appeared. */
int process_demand_page(uint32_t addr);

/* The same, but only for the causes that reach neither the disk nor
   the task list: heap, stack and anonymous memory in the mapping area.
   Tried BEFORE the big kernel lock - so a page fault, the commonest
   way into the kernel there is, does not close it for everybody. */
int process_demand_page_nolock(uint32_t addr);

/* sbrk: moves the end of the current process's heap by increment bytes
   (>=0). Returns the old brk or (uint32_t)-1 on error. */
uint32_t process_sbrk(int32_t increment);

/* mmap: maps 'size' bytes of fresh process memory, returns the address or 0 */
uint32_t process_mmap(uint32_t size);

/* Maps a file from disk into the process's memory (read-only, pages
   arrive under the program's fingers). Returns the address or 0. */
uint32_t process_mmap_file(const char *name, uint32_t *size_out,
                           uint32_t write);

/* Writes back to their files the dirty pages of the current process's
   writable mappings. Returns the number of pages written back. */
uint32_t process_msync(void);

/* How many pages have been read in from mapped files */
uint32_t process_mmap_pages(void);

/* shm: maps a shared memory region (by key); the address or 0 */
uint32_t process_shm(int key, uint32_t size);

/* Creates a thread in the current process's address space; tid or -1 */
int process_thread(uint32_t entry, uint32_t arg);

#endif
