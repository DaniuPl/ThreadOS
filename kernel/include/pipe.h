#ifndef THREADOS_PIPE_H
#define THREADOS_PIPE_H

#include <stdint.h>

/* Inter-process pipes: a ring buffer in the kernel joining the
   producer's stdout to the consumer's stdin. A read blocks on an empty
   pipe while there are writers; once they close it returns 0 (EOF). */

int  pipe_alloc(void);                       /* a new pipe (1 reader, 1 writer) */
int  pipe_read(int idx, void *buf, uint32_t len);
int  pipe_write(int idx, const void *buf, uint32_t len);
void pipe_add_reader(int idx);               /* fork: one more reader */
void pipe_add_writer(int idx);               /* fork: one more writer */
void pipe_close_reader(int idx);
void pipe_close_writer(int idx);

#endif
