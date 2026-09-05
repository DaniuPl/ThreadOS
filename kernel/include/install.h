#ifndef PROTONOS_INSTALL_H
#define PROTONOS_INSTALL_H

#include <stdint.h>

/* Installing ProtonOS onto the machine's own disk
 *
 * What installing amounts to on a UEFI machine is smaller than the
 * word suggests: three files - the loader, the kernel and the initrd -
 * written onto the EFI partition that is already there. No partition
 * is created, no table is touched, nothing that belongs to the system
 * already living on this disk is moved or removed. The firmware finds
 * a loader on the EFI partition and starts it; that is the whole
 * mechanism.
 *
 * The three files come from the loader that started this machine: it
 * hands them over as Multiboot 2 modules, because the kernel in memory
 * is loaded segments and not the file it came from. A system started
 * some other way says so and offers nothing.
 *
 * Split in two on purpose. install_plan() reads and prints and changes
 * nothing; install_do() writes. Between the two sits a person saying
 * yes - which the shells ask for, because reading the keyboard is
 * theirs and this is not.
 */

/* What was found, in a shape a program in ring 3 can be handed.
 * The layout must match struct install_info in user/usys.h and
 * user64/usys64.h - it goes through a syscall.
 */
#define INST_NAME 24

/* Why it cannot be done. Numbers rather than a sentence, because the
   sentence belongs to whoever is showing it - the shell says it one
   way and a window says it another. */
#define INST_OK        0
#define INST_NO_DISK   1
#define INST_NO_FILES  2   /* the loader handed over no kernel image */
#define INST_NO_TABLE  3
#define INST_NO_ESP    4
#define INST_NOT_FAT   5
#define INST_NO_ROOM   6
#define INST_TOO_SMALL 7   /* the disk is too small for a table of its own */

/* Two ways to do it, and the difference between them is everything.
 *
 * ALONGSIDE writes three files into the EFI partition that is already
 * there and touches nothing else: the table stays, the other
 * partitions stay, every file that was on that EFI partition stays.
 *
 * WHOLE makes the disk ours: a new partition table, a new EFI
 * partition, a partition for ProtonOS - and everything that was on
 * that disk is gone. There is no third mode, and there is no mode
 * that resizes somebody's partition to make room; moving another
 * system's data is not a thing to do on the strength of a guess about
 * what it is.
 */
#define INST_ALONGSIDE 0
#define INST_WHOLE     1

struct install_info {
    uint32_t ok;                 /* 1 = it can be done */
    uint32_t problem;            /* one of INST_* above */
    uint32_t mode;               /* which of the two was asked about */
    uint32_t steps;              /* how many steps it takes */
    uint32_t can_wipe;           /* 1 = the whole-disk way is possible at all */
    uint32_t disk;               /* which disk this is about, counted from 0 */
    uint32_t disks;              /* how many the machine has */
    uint32_t esp_index;          /* the partition, counted from one */
    uint32_t esp_lba;
    uint32_t free_kib;
    uint32_t need_kib;
    uint32_t loader_size;
    uint32_t kernel_size;
    uint32_t initrd_size;
    uint32_t fallback_free;      /* 1 = \EFI\BOOT\BOOTX64.EFI is nobody's */
    uint32_t disk_mib;
    uint32_t parts;              /* how many partitions the disk has */
    char disk_name[INST_NAME];   /* the driver behind it */
    char kind[8];                /* FAT16 / FAT32 */
    char label[INST_NAME];       /* what the partition calls itself */
};

/* Reads a disk and fills the shape above for the way of installing
   being asked about. 'disk' is one of the machine's disks counted from
   zero, or -1 for the one the system is using. Changes nothing beyond
   which disk the block layer is pointed at. 1 = it can be done. */
int install_look(int mode, int disk, struct install_info *out);

/* How many steps each way takes, and what each of them is called. A
   person watching a bar move wants to know what is moving. */
#define INST_STEPS       5   /* alongside */
#define INST_STEPS_WHOLE 8   /* the whole disk */
const char *install_step_name(int n);

/* One step (0 .. steps-1) of the way install_look() was last asked
   about. 0 = done, 1 = there was nothing to do (the fallback name
   belongs to somebody), -1 = it did not work. */
int install_step(int n);

/* The two the text version uses: printing what would happen, and doing
   all the steps one after another. */
int install_plan(int mode, int disk);
int install_do(void);

/* Installing the way it is meant to be done: the whole screen, one
   program on it and nothing else within reach. Returns 1 when the
   screen was taken, 0 when it could not be (the desktop is already
   running - and a desktop with windows open cannot become a kiosk). */
int install_graphical(void);

#endif
