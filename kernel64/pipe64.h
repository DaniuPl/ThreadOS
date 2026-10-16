#ifndef THREADOS_PIPE64_H
#define THREADOS_PIPE64_H

#include <stdint.h>

/* Pipes - a buffer between two processes
 *
 * A pipe is not a file and has nothing to do with the disk: it is a
 * ring in kernel memory plus two owner counters. Its whole substance
 * comes from who is still holding it - when the last writer
 * disappears, the reader gets end of data instead of waiting forever.
 */

int pipe64_new(void);                    /* the pipe number or -1 */
void pipe64_ref(int idx, int reader_task);
void pipe64_unref(int idx, int reader_task);

/* They block, but can be interrupted by a signal (-1). A read returns
   0 when there is nobody left to write - that is end of data, not an
   error. */
int pipe64_read(int idx, char *buf, uint32_t len);
int pipe64_write(int idx, const char *buf, uint32_t len);

#endif
