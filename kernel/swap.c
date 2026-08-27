/* Wymiana stron na dysk - magazyn
 *
 * Plik 'swap.sys' powstaje raz, przy starcie: prosimy system plikow
 * o kolejne bloki i zapamietujemy ich numery. Od tej pory strony ida
 * na dysk wprost przez ATA, po zapamietanych numerach blokow.
 *
 * Robimy tak dlatego, ze wymiana rusza w najgorszych momentach - gdy
 * zabraklo ramki, a wiec potencjalnie w srodku obslugi bledu strony,
 * ktory sam trafil sie w srodku operacji na pliku. PFS ma jeden wspolny
 * bufor na blok; wejscie mu tam w droge zostawiloby na dysku bloki
 * z mieszanka dwoch zapisow. Rezerwacja miejsca przez PFS dzieje sie
 * wiec raz, na spokojnie, a potem juz tylko surowe sektory.
 */

#include <stdint.h>
#include "include/swap.h"
#include "include/fs.h"
#include "include/ata.h"
#include "include/blk.h"
#include "include/pmm.h"
#include "include/task.h"
#include "include/printk.h"

#define SLOT_BLOCKS   (PAGE_SIZE / FS_BLOCK)          /* 4 bloki PFS na strone */
#define SECT_PER_BLOCK (FS_BLOCK / ATA_SECTOR_SIZE)   /* 2 sektory na blok */
#define SLOTS_MAX     4096                            /* sufit: 16 MiB */

static uint32_t slot_blk[SLOTS_MAX][SLOT_BLOCKS];
static uint8_t  slot_used[SLOTS_MAX];
static uint32_t slot_count;
static uint32_t slot_busy;
static uint32_t stat_out, stat_in;

void swap_init(void)
{
    if (!fs_ready())
        return;

    /* Bierzemy czwarta czesc wolnego miejsca - dosc, by wymiana miala
       sens, a za malo, by dysk przestal sluzyc do czegokolwiek innego */
    uint32_t used, total;
    fs_usage(&used, &total);
    uint32_t slots = (total - used) / 4 / SLOT_BLOCKS;
    if (slots > SLOTS_MAX)
        slots = SLOTS_MAX;
    if (!slots) {
        printk("[ !! ] Wymiana: za malo miejsca na dysku\n");
        return;
    }

    int f = fs_open(FS_ROOT, "swap.sys", 1);
    if (f < 0) {
        printk("[ !! ] Wymiana: nie moge zalozyc pliku wymiany\n");
        return;
    }
    fs_truncate(f);

    for (uint32_t s = 0; s < slots; s++) {
        for (uint32_t b = 0; b < SLOT_BLOCKS; b++) {
            uint32_t blk = fs_file_block(f, s * SLOT_BLOCKS + b, 1);
            if (!blk) {
                slots = s;              /* dysk sie skonczyl wczesniej */
                goto ready;
            }
            slot_blk[s][b] = blk;
        }
    }
ready:
    if (!slots) {
        printk("[ !! ] Wymiana: nie udalo sie przydzielic blokow\n");
        return;
    }
    /* Ustaw rozmiar pliku, zeby 'fls' i 'df' pokazywaly zajete miejsce */
    char zero = 0;
    fs_write_at(f, slots * PAGE_SIZE - 1, &zero, 1);
    slot_count = slots;

    pmm_set_reclaim(task_reclaim_page);
    printk("[ OK ] Wymiana stron: %u KiB na dysku (swap.sys)\n",
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
    for (uint32_t b = 0; b < SLOT_BLOCKS; b++)
        if (blk_write(fs_block_lba(slot_blk[slot][b]), SECT_PER_BLOCK,
                      p + b * FS_BLOCK) < 0)
            return -1;
    stat_out++;
    return 0;
}

int swap_read(int slot, void *frame)
{
    if (slot < 0 || (uint32_t)slot >= slot_count)
        return -1;
    uint8_t *p = frame;
    for (uint32_t b = 0; b < SLOT_BLOCKS; b++)
        if (blk_read(fs_block_lba(slot_blk[slot][b]), SECT_PER_BLOCK,
                     p + b * FS_BLOCK) < 0)
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
