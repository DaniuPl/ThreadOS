#ifndef THREADOS_FS_H
#define THREADOS_FS_H

#include <stdint.h>

/* TFS3: 1 KiB blocks allocated from a bitmap, 64 inodes, longer names.
   A file has 12 direct blocks, an indirect block (256 pointers) and a
   doubly indirect block (256 x 256) - that is up to 64 MiB, more than
   fits on a typical disk image. */
#define FS_NAME_MAX  63
#define FS_MAX_FILES 64
#define FS_BLOCK     1024u
#define FS_DIRECT    12
#define FS_INDIRECT  (FS_BLOCK / 4)          /* pointers in an indirect block */
#define FS_DINDIRECT (FS_INDIRECT * FS_INDIRECT)   /* through the doubly indirect one */
#define FS_MAX_SIZE  ((uint32_t)(FS_DIRECT + FS_INDIRECT + FS_DINDIRECT) \
                      * FS_BLOCK)                   /* ~64 MiB */

#define FS_ROOT (-1)              /* the root directory (a virtual inode) */

/* Detects the disk and reads the TFS filesystem; formats an empty
   disk. Returns 1 when ready, 0 when there is no disk. */
int fs_init(void);
int fs_ready(void);
/* Formats because somebody asked in so many words: 0 for the whole
   disk, or a partition number counted from one - and then the table
   and every other partition are left exactly as they were. Startup
   never does this to a disk that has anything on it - see fs.c. */
int fs_format_now(int partition);

/* --- Path operations (relative to the working directory cwd) ---
   cwd is a directory slot or FS_ROOT. A path may be absolute ("/a/b"),
   relative ("a/b") and contain "." and "..". */

/* Opens a file; create=1 creates it if it does not exist. Slot or -1. */
int fs_open(int cwd, const char *path, int create);

/* Creates a directory. Returns 0 or -1. */
int fs_mkdir(int cwd, const char *path);

/* Resolves a path to a directory and writes its slot into *newcwd.
   Returns 0 or -1 (does not exist / is not a directory). */
int fs_chdir(int cwd, const char *path, int *newcwd);

/* Removes a file or an empty directory (frees its blocks). 0 or -1. */
int fs_unlink(int cwd, const char *path);
/* A new name (and, if the paths differ in their directories, a new
   parent) for something that already exists. The blocks stay where
   they are - only the entry that names them changes. */
int fs_rename(int cwd, const char *from, const char *to);

/* Writes the absolute path of a slot into buf (>= 512 B). */
void fs_getpath(int slot, char *buf);

/* Enumerates directory 'dir' (FS_ROOT or a slot): the entry with the
   given index. Writes the name, the size and whether it is a
   directory. Returns 1 or 0. */
int fs_readdir(int dir, int index, char *name, uint32_t *size, int *is_dir);

/* --- Operations by inode slot --- */
int fs_size(int slot);
int fs_truncate(int slot);
int fs_read_at(int slot, uint32_t off, void *buf, uint32_t len);
/* The disk block under the 'index'-th block of a file (alloc=1
   allocates it); 0 = error. Plus the first ATA sector of that block -
   swapping reserves space through the filesystem but writes over it
   directly. */
uint32_t fs_file_block(int slot, uint32_t index, int alloc);
uint32_t fs_block_lba(uint32_t block);
int fs_write_at(int slot, uint32_t off, const void *buf, uint32_t len);

/* --- Convenience operations for the kernel shell (always in the root) --- */
int fs_write(const char *name, const void *data, uint32_t len);
int fs_read(const char *name, void *buf, uint32_t max);
void fs_list(void);

/* Disk usage: blocks used / total (the 'df' command) */
void fs_usage(uint32_t *used_blocks, uint32_t *total_blocks);

#endif
