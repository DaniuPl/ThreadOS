#ifndef THREADOS_TFS3TO4_H
#define THREADOS_TFS3TO4_H

#include "tfs3.h"

/* Moving a disk from TFS3 to TFS4 in place (kernel/tfs3to4.c). Both
   answer 1 with TFS4 mounted; 0 when nothing on the partition was
   touched; -1 when it was and the move did not finish (the copy on the
   EFI partition, when there is one, is used again at the next start). */

/* The TFS3 tfs3_probe found, moved over: a copy on the EFI partition
   first when there is one */
int tfs3to4_migrate(const struct tfs3_where *w);

/* A move a start before this one began and did not finish, finished
   from the copy on the EFI partition; 0 also when there is none */
int tfs3to4_resume(void);

#endif
