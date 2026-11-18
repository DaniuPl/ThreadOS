#ifndef THREADOS_SIGNAL64_H
#define THREADOS_SIGNAL64_H

#include <stdint.h>

struct regs;

#define SIGINT   2      /* an interrupt from the keyboard (Ctrl+C) */
#define SIGKILL  9      /* a kill that cannot be caught */
#define SIGTERM 15      /* a request to finish */
#define SIGSTOP 19      /* no stopping here: it ends, as SIGKILL does */

/* Signal n is bit n-1 of a mask; these two no mask holds back */
#define SIG_BIT(n)  (1ULL << ((n) - 1))
#define SIG_ALWAYS  (SIG_BIT(SIGKILL) | SIG_BIT(SIGSTOP))

/* siginfo_t's si_code: who sent it */
#define SI_USER    0    /* kill */
#define SI_KERNEL  0x80 /* the kernel itself (Ctrl+C) */
#define SI_TKILL   (-6) /* tkill, to one thread */

/* Delivers a waiting signal, if there is one. Called just before the
   return to ring 3 - and only then, because only then does the frame
   in 'r' describe the program and not the kernel. */
void signal64_deliver(struct regs *r);

/* Return from a handler: restores the frame put down at delivery */
int signal64_return(struct regs *r);

#endif
