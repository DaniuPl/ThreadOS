#ifndef PROTONOS_ELF64_H
#define PROTONOS_ELF64_H

#include <stdint.h>

/* Loading ELF64 programs into user space
 *
 * The 32-bit kernel does the same thing in exec.c, but there every
 * process gets its own address space. Here the space is for now a
 * single common one, so loading comes down to three things: check the
 * header, allocate frames for the segments and map them with the
 * permissions the ELF itself supplies.
 *
 * The permissions come straight from the file: a segment without the
 * "executable" bit gets execution forbidden (the NX bit), a segment
 * without the "writable" bit stays read-only. So a program's code is
 * not writable and its data is not executable - and the processor sees
 * to that.
 */

/* User space: from 1 GiB upwards. The stack sits at its top and grows
   downwards, so the program's segments have to end below it -
   otherwise the first deeper call would trample its own code.

   The top stands halfway through the gigabyte allocated to the program
   (the PAGING64_USER_PDPT entry) rather than just above the image:
   that leaves half a gigabyte below it for the heap. The eight
   megabytes that were here before were enough as long as there was no
   page swapping - and swapping only makes sense once a program can ask
   for more memory than the machine has RAM. */
#define USER64_BASE        0x0000000040000000ULL
#define USER64_STACK_TOP   0x0000000060000000ULL
/* This many stack pages a program gets to start with. The rest arrive
   by themselves when it touches them: the stack grows on demand, just
   like the heap. Guessing up front how much it needs always ends
   either in wasted memory or in an exception in the middle of a deep
   call - and an attempted certificate verification computing on
   two-thousand-bit numbers reaches deeper than anyone would bet. */
#define USER64_STACK_PAGES 4

/* How far the stack is allowed to grow. The limit is there so that a
   wild pointer does not quietly turn into a mapped page: below it a
   page fault stays a fault. */
#define USER64_STACK_MAX   (256 * 1024ULL)

/* Between the heap and the stack lies an area for mappings: shared
   memory and whatever comes after it. The heap ends at its threshold
   and the mapping area at the threshold of the room for the stack -
   none of the three has any way of running into another. */
#define USER64_MMAP_BASE   0x0000000050000000ULL
#define USER64_MMAP_MAX    (USER64_STACK_TOP - USER64_STACK_MAX)

/* The shared library lies just below the mapping area, at a fixed
   address - the same in every process, because only then can its code
   really be a single copy. The heap and the program's segments end at
   its threshold; user64/libproton64.ld has to agree with this. */
#define USER64_LIB_BASE    0x000000004F000000ULL
#define USER64_TOP         USER64_LIB_BASE

/* Loads an ELF64 image. Returns 0 and sets *entry along with the start
   of the heap (the first free page past the image), or -1. */
int elf64_load(const void *image, uint32_t size, uint64_t *entry,
               uint64_t *brk_base);

/* Allocates and maps the program's stack. Returns its top or 0. */
uint64_t elf64_make_stack(void);

#endif
