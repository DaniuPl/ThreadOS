#ifndef PROTONOS_DYNLINK_H
#define PROTONOS_DYNLINK_H

#include <stdint.h>

/* Shared library and symbol binding at load time
 *
 * Until now every program carried its own copy of everything it used.
 * With crypto and TLS that meant tens of kilobytes of identical code
 * in each of several programs - once on disk, once in the initrd and
 * once in memory, separately for every process.
 *
 * A shared library solves this the way it is solved everywhere: the
 * code lies once, at a fixed address, the same in every address
 * space - the same physical frames. The program does not contain it;
 * it only holds a table of empty slots (the GOT) which the kernel
 * fills in at load time with addresses from the library.
 *
 * We bind EVERYTHING at once, when the program starts, rather than on
 * first call. Lazy binding would require part of the linker to sit in
 * the process's address space and be called from a PLT stub - a whole
 * separate mechanism just to save microseconds at startup.
 *
 * This file knows nothing about paging. Symbol lookup and walking the
 * relocations are identical in both kernels; what differs is only WHO
 * writes the result and WHERE - and that stays with the caller.
 */

#ifdef __x86_64__
typedef uint64_t dyn_addr;
#else
typedef uint32_t dyn_addr;
#endif

/* Loads the library from the initrd (or from disk). Called once, at
   startup. 1 = it is there and usable. */
int dyn_init(void);

/* Is the library loaded */
int dyn_present(void);

/* The library file name - for messages */
const char *dyn_name(void);

/* How many pages the library takes and how many of them are shared */
uint32_t dyn_pages(void);

/* Permissions of a library segment - the ones the ELF itself states */
#define DYN_WRITE   1
#define DYN_EXEC   2

/* Library segments to be mapped into a process's address space.
   Returns 0 when there is no segment with that number. */
int dyn_segment(int i, dyn_addr *vaddr, uint32_t *memsz, uint32_t *filesz,
                int *right, const uint8_t **data);

/* Physical frame of a read-only library page (shared by all
   processes). 0 = no such page. */
uint32_t dyn_frame(dyn_addr vaddr);

/* Does the program image need the library at all (does it have relocations) */
int dyn_is_dynamic(const uint8_t *image, uint32_t size);

/* Walks the program's relocations and calls 'store' for each one with
   the address of the slot in the process's address space and the value
   that belongs there. Returns 0 when everything resolved, or -1. */
int dyn_relocate(const uint8_t *image, uint32_t size, void *ctx,
                int (*store)(void *ctx, dyn_addr addr, dyn_addr value));

#endif
