#ifndef PROTONOS_PART_H
#define PROTONOS_PART_H

#include <stdint.h>

/* The partition table of the disk we booted next to.
 *
 * Until now this system owned whatever disk it found, from sector zero
 * up: PFS3's superblock IS sector zero. That is the right shape for a
 * machine whose whole disk is ours and the wrong one for a laptop,
 * where sector zero belongs to a partition table and the rest belongs
 * to somebody's Windows.
 *
 * So: reading first. Nothing here writes anything. Knowing what is on
 * a disk is what makes it possible to say "this partition" later, and
 * it is worth having on its own - a machine that boots from a stick
 * and cannot say what is in the laptop is a machine one is guessing
 * about.
 */

#define PART_MAX 16
#define PART_NAME 24

struct partition {
    uint64_t start;            /* first sector */
    uint64_t sectors;          /* how many */
    char kind[PART_NAME];      /* what it is, as far as we can tell */
    char label[PART_NAME];     /* the name it carries, when GPT gives one */
};

/* Reads the table. Returns how many partitions were found, or -1 when
   there is no disk. Zero means a disk with no table on it - which is
   what our own disks have looked like so far. */
int part_scan(void);

/* 1 = the table is GPT, 0 = the old MBR one. Meaningless before a
   scan finds something. */
int part_is_gpt(void);

/* One entry, 0-based. 1 = there is one. */
int part_get(int index, struct partition *out);

#endif
