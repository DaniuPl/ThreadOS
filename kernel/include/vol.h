#ifndef THREADOS_VOL_H
#define THREADOS_VOL_H

#include <stdint.h>

/* Volumes other than the system's - a stick's FAT - for programs
   (SYS64_VOLUME, kernel/vol.c). The structures travel to ring 3 as they
   are: user64/usys64.h has the same two. */

struct vol_info {
    int32_t disk;              /* which disk, from 0 ('disks' counts from 1) */
    uint32_t lba;              /* where the volume starts on it */
    uint32_t kib_total, kib_free;
    char label[16];            /* the volume's label, or "" */
    char kind[8];              /* "FAT32", "FAT16", "FAT12" */
    char where[24];            /* the driver behind the disk: "USB", ... */
};

struct vol_entry {
    uint32_t size;
    int32_t is_dir;
    char name[64];
};

/* FAT volumes on disks other than the system's, at most 'max' -> how
   many. Leaves none mounted. */
int vol_list(struct vol_info *out, int max);
/* 0 mounted, -1 not FAT (or the system's own disk) */
int vol_mount(int disk, uint32_t lba);
void vol_umount(void);
/* 1 there, 0 not there, -1 nothing mounted */
int vol_stat(const char *path, struct vol_entry *out);
/* the index-th entry of a folder: 1 one, 0 the end, -1 no such folder */
int vol_dir(const char *path, int index, struct vol_entry *out);
/* bytes read, or -1 */
int vol_read(const char *path, uint32_t off, void *buf, uint32_t len);
/* a whole file, made or replaced: 0 or -1 */
int vol_write(const char *path, const void *data, uint32_t len);
/* with every folder missing on the way: 0 or -1 */
int vol_mkdir(const char *path);
int vol_unlink(const char *path);

#endif
