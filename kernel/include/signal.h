#ifndef THREADOS_SIGNAL_H
#define THREADOS_SIGNAL_H

#include <stdint.h>

/* Signals - must agree with user64/usys64.h */
#define SIGINT   2      /* interrupt from the keyboard (Ctrl+C) */
#define SIGKILL  9      /* an uncatchable kill */
#define SIGTERM 15      /* a request to terminate */
#define SIGCHLD 17      /* a child has finished */
#define NSIG    20

/* Special values in place of a handler address */
#define SIG_DFL 0       /* the kernel's default reaction */
#define SIG_IGN 1       /* ignore the signal */

#endif
