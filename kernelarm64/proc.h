#ifndef THREADOS_ARM64_PROC_H
#define THREADOS_ARM64_PROC_H

#include <stdint.h>
#include "arm64.h"

/* Programs on the ARM64 kernel: each in an address space of its own, at
 * EL0, started from an ELF file in the initrd (kernelarm64/proc.c).
 *
 * A program's addresses are the top half of the 39-bit space TTBR0
 * covers - 256 to 512 GiB, the entries of level 1 from 256 on - which no
 * mapping of the kernel's reaches: RAM on virt ends long before. Its
 * file is linked there (userarm64/user.ld) and its stack is at the top.
 */
#define USER_L1_FIRST 256
#define USER_BASE   (256UL << 30)
#define USER_TOP    (512UL << 30)
#define USER_STACK  (256UL << 10)          /* its size, at USER_TOP */

#define UMAP_WRITE  1
#define UMAP_EXEC   2

/* mmu.c */
uint64_t mmu_kernel_ttbr0(void);
uint64_t *uspace_new(void);
int uspace_map(uint64_t *t, uint64_t va, uint64_t pa, int flags);
uint64_t uspace_lookup(uint64_t *t, uint64_t va, int *flags);
void uspace_free(uint64_t *t);
void cache_sync_code(uint64_t start, uint64_t len);

struct thread;
struct uproc {
    int pid;                     /* 0 = a free slot */
    char name[16];
    uint64_t *table;             /* its TTBR0 */
    uint64_t entry, sp, argv;    /* where it starts */
    int argc;
    uint64_t brk_start, brk;     /* its heap (SYS64_SBRK) */
    struct thread *thread;
    volatile int done;
    int code;                    /* what it ended with */
};

/* A program from the initrd, with its arguments (the name first, then
   words separated by spaces): its pid, or below 0 - not there (-1), not
   an AArch64 program this kernel runs (-2), no memory (-3), too many
   programs (-4) */
int proc_spawn(const char *name, const char *args);
/* Waits for the program to end and gives back all it had: its code */
int proc_wait(int pid);

/* exceptions.c, for what comes from EL0 */
void proc_syscall(struct trap_frame *f);
void proc_fault(struct trap_frame *f, const char *what, uint64_t addr,
                const char *why) __attribute__((noreturn));

/* The code a program a fault ended has, as a shell would say 128 plus
   SIGSEGV */
#define PROC_KILLED 139

#endif
