#ifndef THREADOS_FATCMD_H
#define THREADOS_FATCMD_H

/* The commands that look into a FAT partition.
 *
 * Both shells have them, so they live once. What they are for is a
 * person standing in front of a machine whose EFI partition belongs to
 * a system that boots it - being able to read what is there before
 * anything writes there is the difference between installing next to
 * somebody and installing over them.
 */

void fatcmd_mount(const char *arg);    /* partition number, or "" for the ESP */
void fatcmd_ls(const char *path);
void fatcmd_cat(const char *path);
void fatcmd_put(const char *arg);      /* "<path> <text>" */
void fatcmd_mkdir(const char *path);
void fatcmd_rm(const char *path);
void fatcmd_sum(const char *path);     /* length and a checksum of a whole file */
void fatcmd_copy(const char *arg);     /* "<name in the initrd> <path on the FAT>" */

#endif
