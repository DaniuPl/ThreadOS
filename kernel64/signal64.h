#ifndef PROTONOS_SIGNAL64_H
#define PROTONOS_SIGNAL64_H

#include <stdint.h>

struct regs;

#define SIGINT   2      /* an interrupt from the keyboard (Ctrl+C) */
#define SIGKILL  9      /* a kill that cannot be caught */
#define SIGTERM 15      /* a request to finish */

/* Delivers a waiting signal, if there is one. Called just before the
   return to ring 3 - and only then, because only then does the frame
   in 'r' describe the program and not the kernel. */
void signal64_deliver(struct regs *r);

/* Return from a handler: restores the frame put down at delivery */
int signal64_return(struct regs *r);

#endif
