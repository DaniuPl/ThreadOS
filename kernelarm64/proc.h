#ifndef THREADOS_ARM64_PROC_H
#define THREADOS_ARM64_PROC_H

#include <stdint.h>
#include "arm64.h"
#include "../kernel/include/spinlock.h"

/* Programs on the ARM64 kernel: each in an address space of its own, at
 * EL0, started from an ELF file in the initrd (kernelarm64/proc.c).
 *
 * A program's addresses are the top half of the 39-bit space TTBR0
 * covers - 256 to 512 GiB, the entries of level 1 from 256 on - which no
 * mapping of the kernel's reaches: RAM on virt ends long before. Its
 * file is linked at the bottom (userarm64/user.ld, libc/libcarm64.ld),
 * its heap follows the file, what SYS64_MMAP hands out starts at
 * MMAP_BASE and its stack is at the top. Only the file and the stack's
 * top page are made at the start; the heap, the mappings and the rest
 * of the stack are made a page at a time as the program first touches
 * them - a C library reserves far more than it uses.
 */
#define USER_L1_FIRST 256
#define USER_BASE   (256UL << 30)
#define USER_TOP    (512UL << 30)
#define USER_STACK  (8UL << 20)            /* its size, at USER_TOP */
#define MMAP_BASE   (272UL << 30)
#define MMAP_TOP    (USER_TOP - (1UL << 30))
#define BRK_MAX     (1UL << 30)            /* the most the heap grows */

#define UMAP_WRITE  1
#define UMAP_EXEC   2
#define UMAP_SHARED 4                /* shared memory: fork shares it */
#define UMAP_COW    8                /* shared after fork until written */

/* mmu.c */
uint64_t mmu_kernel_ttbr0(void);
uint64_t *uspace_new(void);
int uspace_map(uint64_t *t, uint64_t va, uint64_t pa, int flags);
uint64_t uspace_lookup(uint64_t *t, uint64_t va, int *flags);
/* The page gone from the table and from every core's TLB: its frame,
   or 0 when there was none */
uint64_t uspace_unmap(uint64_t *t, uint64_t va);
/* fork: every page of 'from' in 'to' too - shared memory as it is, the
   rest read-only on both sides until one writes (uspace_cow). -1 when
   memory ran out for the tables, 'to' then partly filled. */
int uspace_fork(uint64_t *from, uint64_t *to);
/* A write to a page shared after fork: a copy of its own, or the page
   itself when nobody else has it any more. 1 done, 0 not such a page,
   -1 no memory. */
int uspace_cow(uint64_t *t, uint64_t va);
void uspace_free(uint64_t *t);
void cache_sync_code(uint64_t start, uint64_t len);

#define PROC_THREADS 64              /* threads of one program at once */
#define FD_MAX       256             /* descriptors, as on the PC */
#define FD_FIRST     3               /* 0, 1 and 2 are the console */

struct thread;
struct fd_entry;

struct uproc {
    int pid;                     /* 0 = a free slot */
    int parent;                  /* the program that started it, 0 the
                                    kernel (or its parent has ended) */
    char name[16];
    uint64_t *table;             /* its TTBR0 */
    uint64_t entry, sp, argv;    /* where it starts */
    int argc;
    uint64_t brk_start, brk;     /* its heap (SYS64_SBRK) */
    uint64_t mmap_next;          /* the next address SYS64_MMAP gives */
    /* Its table's entries, its threads and its files change under this
       lock: two threads touching new pages on two cores at once must
       not both make the table the pages hang from */
    struct spinlock lock;
    struct thread *threads[PROC_THREADS];  /* until each is given back */
    int live;                    /* how many have not ended */
    volatile int exiting;        /* SYS64_EXIT_GROUP: all of them end */
    struct fd_entry *fds;        /* FD_MAX of them, shared by its threads */
    volatile int done;
    int reaping;                 /* being collected: by nobody else */
    int code;                    /* what it ended with; -1 killed */
    /* SYS64_SIGNAL: the C library's one handler for every signal, and
       the code the handler returns to, which calls SYS64_SIGRETURN */
    uint64_t sig_handler, sig_restorer;
};

/* A program from the initrd, with its arguments (the name first, then
   words separated by spaces): its pid, or below 0 - not there (-1), not
   an AArch64 program this kernel runs (-2), no memory (-3), too many
   programs (-4) */
int proc_spawn(const char *name, const char *args);
/* Waits for the program to end and gives back all it had: its code */
int proc_wait(int pid);
/* Waits for every program left whose parent is gone, and gives back
   what they had */
void proc_wait_orphans(void);
/* Is there a program by that number that has not ended (the network
   stack's question about a socket's owner) */
int proc_alive(int pid);

/* exceptions.c, for what comes from EL0 */
void proc_syscall(struct trap_frame *f);
/* A translation fault of the program's: 1 if the page was one it may
   have, now made, and the instruction is to be tried again */
int proc_page_in(uint64_t addr, int write);
/* A write to a page shared since a fork: 1 if it has a copy of its own
   now, and the instruction is to be tried again */
int proc_cow(uint64_t addr);
/* On every way back to EL0: a program that is ending ends its threads
   here, whatever they were doing */
void proc_return(void);
/* And then: a signal for this thread that it does not hold back is
   delivered - the frame becomes a call of the program's handler */
void proc_signals(struct trap_frame *f);
void proc_fault(struct trap_frame *f, const char *what, uint64_t addr,
                const char *why) __attribute__((noreturn));

/* The code a program a fault ended has, as a shell would say 128 plus
   SIGSEGV */
#define PROC_KILLED 139

#endif
