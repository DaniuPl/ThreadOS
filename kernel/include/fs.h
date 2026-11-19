#ifndef THREADOS_FS_H
#define THREADOS_FS_H

#include <stdint.h>

/* The system's file system, whatever is on the disk (kernel/fs.c).
 *
 * The rest of the kernel sees files through these calls and nothing
 * else: a file or a directory is an int - a slot - that stays the same
 * for as long as the file exists, the root directory is FS_ROOT. Which
 * format stands behind it (TFS3 today, kernel/tfs3.c; TFS4, the ext2
 * format, next - docs/tfs4-plan.md) is the front's business, and the
 * limits below are the API's, not any one format's: a format may allow
 * less (TFS3 names are 63 bytes) and says no past its own. */

#define FS_NAME_LEN  255          /* one name in a directory, without the NUL */
#define FS_PATH_MAX  1024         /* a whole path, with its NUL */

#define FS_ROOT (-1)              /* the root directory (a virtual inode) */

/* Why the last call that failed did: Linux's errno numbers, so that a
   C library hands them on as they are */
#define FS_EPERM         1
#define FS_ENOENT        2
#define FS_EIO           5
#define FS_EACCES        13
#define FS_EEXIST        17
#define FS_EXDEV         18
#define FS_ENOTDIR       20
#define FS_EISDIR        21
#define FS_EINVAL        22
#define FS_ENOSPC        28
#define FS_EROFS         30
#define FS_EMLINK        31
#define FS_ENAMETOOLONG  36
#define FS_ENOSYS        38
#define FS_ENOTEMPTY     39
#define FS_ELOOP         40
int fs_errno(void);
void fs_set_errno(int e);

/* Detects the disk and reads the file system on it: moves a TFS3 to
   TFS4, formats an empty disk. Returns 1 when ready, 0 when there is
   no disk or it is somebody else's. */
int fs_init(void);
int fs_ready(void);
/* Formats with TFS4 because somebody asked in so many words: 0 for the
   whole disk, or a partition number counted from one - a GPT one, which
   becomes of ThreadOS's type, and the rest of the table and every other
   partition are left exactly as they were. Startup never does this to
   a disk that has anything on it - see fs_init. */
int fs_format_now(int partition);
/* The same with TFS3, the format before TFS4: only so that the move
   from it can be tested on a TFS3 this system wrote ('format3') */
int fs_format_tfs3(int partition);

/* --- Path operations (relative to the working directory cwd) ---
   cwd is a directory slot or FS_ROOT. A path may be absolute ("/a/b"),
   relative ("a/b") and contain "." and "..", up to FS_PATH_MAX. */

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

/* The absolute path of a directory slot into buf, which holds cap
   bytes: its length, or -1 when it does not fit (and buf holds "/"). */
int fs_getpath(int slot, char *buf, uint32_t cap);

/* What a slot is */
#define FS_T_FILE 1
#define FS_T_DIR  2
#define FS_T_LINK 3               /* a symbolic link (fs_lstat_path) */
struct fs_stat {
    int type;                     /* FS_T_* */
    int slot;
    uint64_t ino;                 /* the inode number a program sees: the
                                     same for every name of the file, 2
                                     for the root, as in ext2 */
    uint64_t size;
    int owner;                    /* the account that owns it */
    /* What a format keeps of POSIX's (TFS4); 0 where it keeps nothing
       (TFS3), and a C library supplies its own then */
    uint32_t mode;                /* the permission bits, 07777 */
    uint32_t gid;
    uint32_t nlink;
    uint32_t atime, mtime, ctime; /* seconds since 1970 */
    uint64_t blocks512;
};
/* As fs_stat_path, but a symbolic link at the end is itself described */
int fs_lstat_path(int cwd, const char *path, struct fs_stat *st);
/* What a slot is, as fs_stat_path says it: 0, or -1 */
int fs_stat_slot(int slot, struct fs_stat *st);
/* What stands at a path: 0 and *st, or -1 (nothing there, or not
   ours to look at) */
int fs_stat_path(int cwd, const char *path, struct fs_stat *st);

/* One entry of a directory */
struct fs_dirent {
    char name[FS_NAME_LEN + 1];
    uint64_t size;
    uint64_t ino;                 /* as struct fs_stat's */
    int slot;
    int is_dir;
};
/* The entry at or after *cursor in directory 'dir' (0 starts the
   listing): 1 with *e filled and *cursor moved past it, 0 at the end,
   -1 when dir is not a directory one may list. The cursor is the
   format's own number; a program gets it back and hands it in again. */
int fs_readdir_at(int dir, uint64_t *cursor, struct fs_dirent *e);

/* Enumerates directory 'dir' (FS_ROOT or a slot): the entry with the
   given index. Writes the name (FS_NAME_LEN + 1 bytes of room), the
   size and whether it is a directory. Returns 1 or 0. The front keeps
   where the last call ended, so a listing from 0 upwards does not
   start from the top every time. */
int fs_readdir(int dir, int index, char *name, uint64_t *size, int *is_dir);

/* --- Operations by slot --- */
int64_t fs_size(int slot);
int fs_owner(int slot);
/* Rechecks whether the current task may change this inode. */
int fs_can_write(int slot);
int fs_set_owner(int slot, int uid);
int fs_truncate(int slot);
/* The file made 'size' bytes long: cut back (the blocks past the end
   freed, the rest of the last block zeroed) or grown (a hole of zeros) */
int fs_truncate_to(int slot, uint64_t size);
/* A descriptor on the inode was closed: an unlinked file goes once the
   last one is. Each kernel's task code calls it, and answers in turn
   whether any descriptor still holds an inode (task64_slot_open). */
void fs_slot_closed(int slot);
int fs_read_at(int slot, uint64_t off, void *buf, uint32_t len);
int fs_write_at(int slot, uint64_t off, const void *buf, uint32_t len);

/* --- POSIX's metadata (TFS4; on TFS3 the changes are taken and kept
   nowhere, as they always were) --- */

/* Makes a file with these permission bits (the umask taken off) when
   create is set; otherwise fs_open */
int fs_open_mode(int cwd, const char *path, int create, uint32_t mode);
int fs_mkdir_mode(int cwd, const char *path, uint32_t mode);
int fs_chmod(int cwd, const char *path, uint32_t mode);
int fs_fchmod(int slot, uint32_t mode);
int fs_chown(int cwd, const char *path, int uid, int gid);  /* -1: keep */
#define FS_TIME_NOW  (-1)
#define FS_TIME_OMIT (-2)
int fs_utimes(int cwd, const char *path, int64_t atime, int64_t mtime);
int fs_futimes(int slot, int64_t atime, int64_t mtime);
/* A second name for a file (not a directory) */
int fs_link(int cwd, const char *from, const char *to);
/* A symbolic link at 'path' holding 'target' */
int fs_symlink(int cwd, const char *target, const char *path);
/* A symbolic link's target into buf: its length, or -1 */
int fs_readlink(int cwd, const char *path, char *buf, uint32_t cap);
/* May the current task read (4), write (2), enter or run (1) what is
   at path; 0 asks only that it exists. 0 or -1. */
int fs_access(int cwd, const char *path, int mode);

/* --- Raw blocks, for swap and file mappings ---
   They read and write a file's blocks straight through blk_*, past the
   file system: the block size, the block under the 'index'-th block of
   a file (alloc=1 allocates it; 0 = none, or an error), and the disk
   sector a block starts at. */
uint32_t fs_block_size(void);
uint32_t fs_file_block(int slot, uint32_t index, int alloc);
uint32_t fs_block_lba(uint32_t block);

/* --- Convenience operations for the kernel shell (always in the root) --- */
int fs_write(const char *name, const void *data, uint32_t len);
int fs_read(const char *name, void *buf, uint32_t max);
void fs_list(void);

/* Disk usage, in blocks of fs_block_size(): used / total ('df') */
void fs_usage(uint64_t *used_blocks, uint64_t *total_blocks);

#endif
