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

/* Where a TFS3 is (tfs3_probe): from sector 'base', 'sectors' long,
   'partition' counted from one (0: the whole disk), its blocks in use
   'bytes' long from its start */
struct tfs3_where {
    uint32_t base, sectors;
    int partition;
    uint32_t blocks, bytes;
};
/* 1 when the disk holds a TFS3, the whole disk or a partition; nothing
   is mounted or written */
int tfs3_probe(struct tfs3_where *w);
/* Mounts the TFS3 at base (to run on it, as before TFS4) */
int tfs3_mount_at(uint32_t base, uint32_t sectors);
/* The TFS3 of a copy in memory, 'len' bytes from its first block, to
   read through tfs3_backend (the migration to TFS4); tfs3_close lets
   it go */
int tfs3_open_image(const void *img, uint32_t len);
void tfs3_close(void);
/* Formats with TFS3: the whole disk (0) or partition N. Kept so that
   the migration can be tested on a TFS3 the system itself wrote
   ('format3'); new disks are TFS4's. */
int tfs3_format(int partition);

#endif
