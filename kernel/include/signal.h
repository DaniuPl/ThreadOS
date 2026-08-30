#ifndef PROTONOS_SIGNAL_H
#define PROTONOS_SIGNAL_H

#include <stdint.h>

/* Signals - must agree with user/usys.h */
#define SIGINT   2      /* interrupt from the keyboard (Ctrl+C) */
#define SIGKILL  9      /* an uncatchable kill */
#define SIGTERM 15      /* a request to terminate */
#define SIGCHLD 17      /* a child has finished */
#define NSIG    20

/* Special values in place of a handler address */
#define SIG_DFL 0       /* the kernel's default reaction */
#define SIG_IGN 1       /* ignore the signal */

struct regs;

/* Delivery of a pending signal on return to ring 3. When the process
   has a handler of its own, the kernel pushes a frame of the
   interrupted state onto its stack and points eip at the handler. */
void signal_deliver(struct regs *r);

/* Return from a handler (the sigreturn syscall): restores the state
   saved on the user stack at delivery time. */
void signal_return(struct regs *r);

/* Signal name for kernel messages */
const char *signal_name(int sig);

#endif
