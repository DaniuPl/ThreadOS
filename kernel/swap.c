/* Swapping pages to disk - the store
 *
 * The file 'swap.sys' is created once, at startup: we ask the
 * filesystem for successive blocks and remember their numbers. From
 * then on pages go to disk directly through ATA, by the remembered
 * block numbers.
 *
 * We do it this way because swapping starts at the worst moments -
 * when a frame ran out, and therefore potentially in the middle of a
 * page fault handler that itself landed in the middle of a file
 * operation. TFS has one shared block buffer; getting in its way there
 * would leave blocks on disk with a mixture of two writes. Reserving
 * space through TFS therefore happens once, at leisure, and after that
 * it is only raw sectors.
 */

#include <stdint.h>
#include "include/swap.h"
#include "include/fs.h"
#include "include/ata.h"
#include "include/blk.h"
#include "include/pmm.h"
#include "include/task.h"
#include "include/printk.h"

/* A page takes as many of the file system's blocks as fit in it: four
   of TFS3's 1 KiB, one of TFS4's 4 KiB. The table has room for the
   smallest blocks; what is kept is each block's first sector. */
#define BLOCKS_MAX    (PAGE_SIZE / 1024)
#define SLOTS_MAX     4096                            /* ceiling: 16 MiB */

static uint32_t slot_lba[SLOTS_MAX][BLOCKS_MAX];
static uint32_t per_slot;                 /* blocks in a page */
static uint32_t sect_per_block;
static uint8_t  slot_used[SLOTS_MAX];
static uint32_t slot_count;
static uint32_t slot_busy;
static uint32_t stat_out, stat_in;

void swap_init(void)
{
    if (!fs_ready())
        return;

    /* The previous boot's swap file goes first. Left in place until
       after the measuring, it counted as used space, so the size of each
       boot's file depended on the one before it and changed at every
       start of the same disk. */
    int f = fs_open(FS_ROOT, "swap.sys", 0);
    if (f >= 0)
        fs_truncate(f);

    /* We take a quarter of the free space - enough for swapping to make
       sense, too little for the disk to stop serving anything else */
    uint32_t bs = fs_block_size();
    if (bs < 1024 || bs > PAGE_SIZE) {
        printk("[ !! ] Swap: blocks of %u B do not fit a page\n", bs);
        return;
    }
    per_slot = PAGE_SIZE / bs;
    sect_per_block = bs / ATA_SECTOR_SIZE;
    uint64_t used, total;
    fs_usage(&used, &total);
    uint64_t room = (total - used) / 4 / per_slot;
    uint32_t slots = room > SLOTS_MAX ? SLOTS_MAX : (uint32_t)room;
    if (!slots) {
        printk("[ !! ] Swap: too little room on the disk\n");
        return;
    }

    f = fs_open(FS_ROOT, "swap.sys", 1);
    if (f < 0) {
        printk("[ !! ] Swap: I cannot create the swap file\n");
        return;
    }

    for (uint32_t s = 0; s < slots; s++) {
        for (uint32_t b = 0; b < per_slot; b++) {
            uint32_t blk = fs_file_block(f, s * per_slot + b, 1);
            if (!blk) {
                slots = s;              /* the disk ran out sooner */
                goto ready;
            }
            slot_lba[s][b] = fs_block_lba(blk);
        }
    }
ready:
    if (!slots) {
        printk("[ !! ] Swap: could not allocate the blocks\n");
        return;
    }
    /* Set the file size so that 'fls' and 'df' show the space as taken */
    char zero = 0;
    fs_write_at(f, (uint64_t)slots * PAGE_SIZE - 1, &zero, 1);
    slot_count = slots;

    pmm_set_reclaim(task_reclaim_page);
    printk("[ OK ] Page swapping: %u KiB on the disk (swap.sys)\n",
           slots * PAGE_SIZE / 1024);
}

int swap_alloc_slot(void)
{
    for (uint32_t s = 0; s < slot_count; s++) {
        if (!slot_used[s]) {
            slot_used[s] = 1;
            slot_busy++;
            return (int)s;
        }
    }
    return -1;
}

void swap_free_slot(int slot)
{
    if (slot >= 0 && (uint32_t)slot < slot_count && slot_used[slot]) {
        slot_used[slot] = 0;
        slot_busy--;
    }
}

int swap_write(int slot, const void *frame)
{
    if (slot < 0 || (uint32_t)slot >= slot_count)
        return -1;
    const uint8_t *p = frame;
    for (uint32_t b = 0; b < per_slot; b++)
        if (blk_write(slot_lba[slot][b], sect_per_block,
                      p + b * (PAGE_SIZE / per_slot)) < 0)
            return -1;
    stat_out++;
    return 0;
}

int swap_read(int slot, void *frame)
{
    if (slot < 0 || (uint32_t)slot >= slot_count)
        return -1;
    uint8_t *p = frame;
    for (uint32_t b = 0; b < per_slot; b++)
        if (blk_read(slot_lba[slot][b], sect_per_block,
                     p + b * (PAGE_SIZE / per_slot)) < 0)
            return -1;
    stat_in++;
    return 0;
}

void swap_stats(uint32_t *used, uint32_t *total, uint32_t *out, uint32_t *in)
{
    *used = slot_busy;
    *total = slot_count;
    *out = stat_out;
    *in = stat_in;
}
