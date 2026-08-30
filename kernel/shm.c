/* Memory shared between processes (regions by key)
 *
 * A region is a contiguous block of physical frames from the PMM,
 * allocated on the first use of a given key. Every process can map it
 * into its own space (process_shm in exec.c), so different processes
 * see the same memory. Regions live until the system stops - cleaning
 * up a process passes their frames by (shm_is_shared).
 */

#include <stdint.h>
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
            return p;                  /* an existing region */
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
        /* The region lies in the identity mapping, so the physical
           address is also the virtual one here - in both kernels */
        memset((void *)(uintptr_t)phys, 0, pages * 4096);
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
