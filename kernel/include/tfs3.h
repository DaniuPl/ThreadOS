#ifndef THREADOS_TFS3_H
#define THREADOS_TFS3_H

#include <stdint.h>
#include "fsbackend.h"

/* TFS3 (kernel/tfs3.c): 1 KiB blocks allocated from a bitmap, 64
   inodes, names of 63 bytes. A file has 12 direct blocks, an indirect
   block (256 pointers) and a doubly indirect block (256 x 256) - up to
   64 MiB. The format is frozen: TFS4 replaces it, and this stays to
   read the disks it is on. scripts/ptest.py reads these numbers. */
#define TFS3_NAME_MAX  63
#define TFS3_MAX_FILES 64
#define TFS3_BLOCK     1024u
#define TFS3_DIRECT    12
#define TFS3_INDIRECT  (TFS3_BLOCK / 4)       /* pointers in an indirect block */
#define TFS3_DINDIRECT (TFS3_INDIRECT * TFS3_INDIRECT)
#define TFS3_MAX_SIZE  ((uint32_t)(TFS3_DIRECT + TFS3_INDIRECT + \
                                   TFS3_DINDIRECT) * TFS3_BLOCK)  /* ~64 MiB */

extern const struct fs_backend tfs3_backend;

/* Finds TFS3 on the disk (or formats a blank disk with it): 1 when it
   is there to use, 0 when not */
int tfs3_mount(void);
/* Formats with TFS3: the whole disk (0) or partition N */
int tfs3_format(int partition);

#endif
