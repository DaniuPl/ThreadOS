#ifndef THREADOS_PIPE64_H
#define THREADOS_PIPE64_H

#include <stdint.h>

/* Pipes - a buffer between two processes
 *
 * A pipe is not a file and has nothing to do with the disk: it is a
 * pair of connected ends in kernel memory (kernel/ipc.c), and its whole
 * substance comes from who is still holding them - when the last
 * writer disappears, the reader gets end of data instead of waiting
 * forever. The numbers here are ends, one per side.
 */

int pipe64_new(int *reader, int *writer);   /* 0, or -1 when there is no room */
void pipe64_ref(int end);
void pipe64_unref(int end);

/* They block, but can be interrupted by a signal (-1). A read returns
   0 when there is nobody left to write - that is end of data, not an
   error. A write with nobody left to read is -1. */
int pipe64_read(int end, char *buf, uint32_t len);
int pipe64_write(int end, const char *buf, uint32_t len);

#endif
