#ifndef PROTONOS_SWAP_H
#define PROTONOS_SWAP_H

#include <stdint.h>

/* Swapping pages to disk
 *
 * A store for pages evicted from memory. We reserve the space through
 * the filesystem (a file 'swap.sys') so that 'df' and 'fls' tell the
 * truth and so that nobody writes over it - but the pages themselves
 * travel back and forth directly through ATA, bypassing the PFS code.
 * PFS has one shared block buffer, and swapping can start in the
 * middle of any file operation; getting in its way there would end in
 * a broken disk.
 */

/* Creates the swap file (after fs_init, in a thread context). */
void swap_init(void);

/* Reserves a free slot for a page; -1 when there is none. */
int swap_alloc_slot(void);
void swap_free_slot(int slot);

/* Moves 4 KiB between a frame and the disk. 0 = OK, -1 = error. */
int swap_write(int slot, const void *frame);
int swap_read(int slot, void *frame);

/* Statistics for the 'mem' command */
void swap_stats(uint32_t *used, uint32_t *total,
                uint32_t *out, uint32_t *in);

#endif
