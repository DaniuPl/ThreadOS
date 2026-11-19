#ifndef THREADOS_FSBACKEND_H
#define THREADOS_FSBACKEND_H

#include <stdint.h>
#include "fs.h"

/* A format behind the file system's front (kernel/fs.c). Each call has
   the meaning of the fs_* call of fs.h with the same name, for that
   format alone; the front has already checked that the file system is
   ready and that a path fits FS_PATH_MAX. */
struct fs_backend {
    const char *name;
    int (*open)(int cwd, const char *path, int create);
    int (*mkdir)(int cwd, const char *path);
    int (*chdir)(int cwd, const char *path, int *newcwd);
    int (*unlink)(int cwd, const char *path);
    int (*rename)(int cwd, const char *from, const char *to);
    int (*getpath)(int slot, char *buf, uint32_t cap);
    int (*stat_path)(int cwd, const char *path, struct fs_stat *st);
    int (*stat_slot)(int slot, struct fs_stat *st);
    int (*readdir_at)(int dir, uint64_t *cursor, struct fs_dirent *e);
    int64_t (*size)(int slot);
    int (*owner)(int slot);
    int (*can_write)(int slot);
    int (*set_owner)(int slot, int uid);
    int (*truncate_to)(int slot, uint64_t size);
    void (*slot_closed)(int slot);
    int (*read_at)(int slot, uint64_t off, void *buf, uint32_t len);
    int (*write_at)(int slot, uint64_t off, const void *buf, uint32_t len);
    uint32_t (*file_block)(int slot, uint32_t index, int alloc);
    uint32_t (*block_lba)(uint32_t block);
    uint32_t (*block_size)(void);
    void (*usage)(uint64_t *used, uint64_t *total);
};

#endif
