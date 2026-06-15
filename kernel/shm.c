/* Pamiec wspoldzielona miedzy procesami (regiony po kluczu)
 *
 * Region to ciagly blok ramek fizycznych z PMM, zaalokowany przy
 * pierwszym uzyciu danego klucza. Kazdy proces moze go zmapowac do
 * swojej przestrzeni (process_shm w exec.c), wiec rozne procesy widza
 * te sama pamiec. Regiony zyja do konca dzialania systemu - sprzatanie
 * procesu pomija ich ramki (shm_is_shared).
 */

#include "include/shm.h"
#include "include/pmm.h"
#include "include/string.h"
#include "include/io.h"

#define MAX_SHM 4

struct region {
    int used;
    int key;
    uint32_t phys;
    uint32_t pages;
};

static struct region regions[MAX_SHM];

uint32_t shm_get(int key, uint32_t size)
{
    uint32_t flags = irq_save();

    for (int i = 0; i < MAX_SHM; i++)
        if (regions[i].used && regions[i].key == key) {
            uint32_t p = regions[i].phys;
            irq_restore(flags);
            return p;                  /* istniejacy region */
        }

    if (!size)
        size = 4096;
    uint32_t pages = (size + 4095) / 4096;
    for (int i = 0; i < MAX_SHM; i++) {
        if (regions[i].used)
            continue;
        uint32_t phys = pmm_alloc_contiguous(pages);
        if (!phys)
            break;
        memset((void *)phys, 0, pages * 4096);   /* <64 MiB, identycznie */
        regions[i].used = 1;
        regions[i].key = key;
        regions[i].phys = phys;
        regions[i].pages = pages;
        irq_restore(flags);
        return phys;
    }

    irq_restore(flags);
    return 0;
}

int shm_is_shared(uint32_t frame)
{
    for (int i = 0; i < MAX_SHM; i++) {
        if (!regions[i].used)
            continue;
        if (frame >= regions[i].phys &&
            frame < regions[i].phys + regions[i].pages * 4096)
            return 1;
    }
    return 0;
}
