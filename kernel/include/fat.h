#ifndef PROTONOS_FAT_H
#define PROTONOS_FAT_H

#include <stdint.h>

/* FAT12/16/32 - somebody else's filesystem, and the only one that
 * matters here.
 *
 * ProtonOS keeps its own files in PFS3 and has no reason to want
 * anything else - except for one place. The EFI system partition is
 * FAT by decree: the firmware of every UEFI machine knows how to read
 * FAT and knows how to read nothing else, so a boot loader that is not
 * on a FAT filesystem will never be started. Installing this system
 * next to another one therefore means writing into a FAT partition
 * that already exists and already has somebody else's boot loader in
 * it.
 *
 * Hence the shape of what follows: reading everything, writing only
 * what an installer needs, and never formatting. The partition we are
 * going to write into belongs to a machine that boots without us.
 *
 * Long names are read and written. They are not a nicety here: the
 * loader looks for 'protonos64.bin', which does not fit into eight
 * characters and a dot, and bending the name to the filesystem would
 * be letting the filesystem decide what the system's files are called.
 */

#define FAT_NAME_MAX 64

struct fat_entry {
    char name[FAT_NAME_MAX];   /* the long name when there is one */
    uint32_t size;             /* bytes; 0 for a directory */
    uint32_t clus;             /* first cluster (0 = an empty file) */
    int is_dir;
};

/* Mounts the filesystem lying in the partition that starts at 'lba'.
   1 = mounted, 0 = there is no FAT there (and then nothing is
   touched). */
int fat_mount(uint32_t lba);
void fat_umount(void);
int fat_mounted(void);

/* "FAT12" / "FAT16" / "FAT32", or "-" */
const char *fat_kind(void);
/* The volume label from the boot sector, or "" */
const char *fat_label(void);
uint32_t fat_total_kib(void);
uint32_t fat_free_kib(void);
uint32_t fat_cluster_bytes(void);

/* Resolves a path ("/", "/EFI/BOOT") to the directory's first cluster;
   zero means the root. 1 = it is a directory. */
int fat_dir(const char *path, uint32_t *clus);

/* The index-th entry of a directory ('clus' zero = the root).
   1 = there is one, 0 = the end. Volume labels and the '.'/'..'
   entries are skipped. */
int fat_readdir(uint32_t clus, int index, struct fat_entry *out);

/* What lies under a path. 1 = it exists. */
int fat_stat(const char *path, struct fat_entry *out);

/* Reads 'len' bytes of a file from offset 'off'. Returns how many were
   read, or -1. */
int fat_read(const char *path, uint32_t off, void *buf, uint32_t len);

/* --- Writing ---
 *
 * Only what putting a boot loader onto an existing EFI partition
 * needs: a directory, a file, its contents. Nothing here creates a
 * filesystem and nothing here removes anybody else's files.
 */

/* Creates a directory, with every missing step of the path along the
   way ("/EFI/BOOT"). 0 = it exists afterwards, -1 = it does not. */
int fat_mkdir(const char *path);

/* Writes a whole file: creates it when it is not there, and replaces
   its contents when it is. 0 or -1. */
int fat_write_file(const char *path, const void *data, uint32_t len);

/* Removes a file (not a directory). 0 or -1. */
int fat_unlink(const char *path);

/* --- Making one -----------------------------------------------------
 *
 * The one place this system creates a FAT rather than writing into
 * somebody's: a machine on which ProtonOS is to be the only system
 * needs an EFI partition, and an EFI partition has to be FAT or the
 * firmware will not read it.
 *
 * FAT32 and only FAT32, because that is what an EFI partition is
 * everywhere, and because the count of clusters decides the format -
 * so a partition too small to hold 65525 of them cannot be one, and
 * this says so instead of making something else and calling it FAT32.
 *
 * This destroys whatever was in the partition. 0 = done, -1 = it did
 * not work.
 */
int fat_mkfs(uint32_t lba, uint32_t sectors, const char *label);

#endif
