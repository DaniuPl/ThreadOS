#ifndef THREADOS_TFS4_H
#define THREADOS_TFS4_H

#include <stdint.h>
#include "fsbackend.h"

/* TFS4 (kernel/tfs4.c): the ext2 format, revision 1, written from its
   specification. Read-only for now (docs/tfs4-plan.md, stage 2). */

extern const struct fs_backend tfs4_backend;

/* Finds TFS4 on the disk: a ThreadOS partition (GPT type THREADOS-TFS)
   holding ext2, or a whole disk without a table whose ext2 is labelled
   "ThreadOS". Anybody else's ext2 is left alone. 1 when mounted. */
int tfs4_mount(void);

#endif
