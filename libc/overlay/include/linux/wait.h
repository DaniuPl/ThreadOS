#ifndef _LINUX_WAIT_H
#define _LINUX_WAIT_H

/* <linux/wait.h> for programs written for Linux. What they want of it
   is P_PIDFD, for waitid() on a pidfd (src/threados/socket.c), and
   musl's <sys/wait.h> has it in its idtype_t - defining it again here
   as Linux's header does would break that enum when this header comes
   first. GLib looks for this header to know pidfds are there. */
#include <sys/wait.h>

#endif
