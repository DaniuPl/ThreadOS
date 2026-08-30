/* A trial of the locks: the frame map and the kernel heap under fire
 * from every core
 *
 * A lock guards against something that cannot be seen except by
 * trying. Turning interrupts off was enough while there was one core -
 * nobody could step into the middle of an operation. Now two cores can
 * really be inside the same loop looking for a free frame at the same
 * time, find the same one and both take it for their own.
 *
 * Hence the shape of this trial. It does not ask "did it crash",
 * because the answer to that is sometimes accidental. Every allocation
 * gets a pattern that depends on the unit number and is read back
 * immediately after: a frame handed out twice has two writers and the
 * pattern will not match. At the end we also count the frames and the
 * heap bytes - the same before as after, otherwise something leaked or
 * came back twice.
 *
 * The work runs through smp_work, so on a secondary core there are
 * neither interrupts nor an exception table. Only things that do not
 * sleep and do not wait for hardware may be called here: memory
 * allocation yes, disk or network no.
 */

#include <stdint.h>
#include "include/smpmem.h"
#include "include/smp.h"
#include "include/pmm.h"
#include "include/heap.h"
#include "include/printk.h"
#include "include/string.h"

#define UNITS   64
#define RETRIES   48
#define SLOW        (4096 / 4)      /* words in a frame */

struct attempt {
    volatile uint32_t nerrors;       /* pattern mismatches */
    volatile uint32_t failed;   /* allocations that failed */
    volatile uint32_t allocs;
};

static uint32_t pattern(uint32_t number, uint32_t step)
{
    return number * 2654435761u + step * 40503u + 1u;
}

static void unit(uint32_t number, void *data)
{
    struct attempt *p = data;

    for (uint32_t i = 0; i < RETRIES; i++) {
        __atomic_add_fetch(&p->allocs, 1, __ATOMIC_RELAXED);

        /* --- a physical frame --- */
        uint32_t frame = pmm_alloc_frame();
        if (!frame) {
            __atomic_add_fetch(&p->failed, 1, __ATOMIC_RELAXED);
        } else {
            uint32_t w = pattern(number, i);
            volatile uint32_t *s = (volatile uint32_t *)(uintptr_t)frame;
            for (uint32_t k = 0; k < SLOW; k++)
                s[k] = w + k;
            /* The second round reads the same thing. If the same frame had
               also gone to another core, it would have written its own
               pattern into it - and this is exactly where that would
               show. */
            for (uint32_t k = 0; k < SLOW; k++)
                if (s[k] != w + k) {
                    __atomic_add_fetch(&p->nerrors, 1, __ATOMIC_RELAXED);
                    break;
                }
            pmm_free_frame(frame);
        }

        /* --- a heap block --- */
        uint32_t ile = 32 + (number * 7 + i * 13) % 192;
        uint8_t *b = kmalloc(ile);
        if (!b) {
            __atomic_add_fetch(&p->failed, 1, __ATOMIC_RELAXED);
            continue;
        }
        uint8_t ch = (uint8_t)(number * 31 + i);
        for (uint32_t k = 0; k < ile; k++)
            b[k] = (uint8_t)(ch + k);
        for (uint32_t k = 0; k < ile; k++)
            if (b[k] != (uint8_t)(ch + k)) {
                __atomic_add_fetch(&p->nerrors, 1, __ATOMIC_RELAXED);
                break;
            }
        kfree(b);
    }
}

void smpmem_test(void)
{
    struct attempt p = { 0, 0, 0 };

    uint32_t frames_at_start = pmm_used_frames();
    uint32_t heap_before = heap_used_bytes();

    smp_work(unit, &p, UNITS);

    uint32_t frames_after = pmm_used_frames();
    uint32_t heap_after = heap_used_bytes();

    printk("Rdzeni: %d (obudzonych %d), jednostek %u po %u przydzialow\n",
           smp_cpus(), smp_started(), (uint32_t)UNITS,
           (uint32_t)RETRIES);
    printk("Udzial:");
    for (int i = 0; i < smp_cpus(); i++)
        printk(" rdzen%d=%u", i, smp_share(i));
    printk("\n");
    printk("Przydzialow: %u, nieudanych: %u\n", p.allocs, p.failed);
    printk("Wzorzec: %s (bledow %u)\n",
           p.nerrors ? "NIE ZGADZA SIE" : "zgadza sie", p.nerrors);
    printk("Ramki: %u -> %u, sterta: %u -> %u B\n",
           frames_at_start, frames_after, heap_before, heap_after);
    printk("Nic nie wycieklo: %s\n",
           (frames_at_start == frames_after && heap_before == heap_after)
               ? "tak" : "NIE");
    printk("Liczylo rdzeni: %d\n", smp_workers());
}
