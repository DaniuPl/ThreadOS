/* The page allocator of the ARM64 kernel
 *
 * The x86 kernels build theirs from the Multiboot memory map; here the
 * memory comes from the device tree, as one range. Everything else is
 * the same promise kernel/include/pmm.h makes - which is the point: the
 * shared kernel/heap.c is built on it unchanged.
 *
 * A count of owners per 4 KiB page, 0 for a free one: a page shared
 * after fork, copied on the first write, and the pages of shared memory
 * mapped by several programs have more than one, and go back only with
 * the last (pmm_frame_ref). Taken from the start: the device tree, the
 * gap below the kernel, the kernel image with its .bss - all of it up
 * to first_free. The interface speaks 32-bit addresses, so only RAM
 * below 4 GiB is handed out; on virt that is the first three gigabytes,
 * which is more than this kernel will want for a long time.
 *
 * A single page is looked for from where the last one was found, not
 * from the start: a fork takes thousands, one after another, and the
 * pages near the start are the ones least likely to be free.
 */

#include <stdint.h>
#include "arm64.h"
#include "../kernel/include/pmm.h"
#include "../kernel/include/spinlock.h"

#define MAX_FRAMES (3u << 18)            /* 3 GiB of 4 KiB pages */

static uint16_t refs[MAX_FRAMES];
static uint64_t base;                    /* the first page's address */
static uint32_t frames, used, hint;
static uint32_t ram_kib;
static struct spinlock lock = SPINLOCK_FREE;

static int taken(uint32_t i)
{
    return refs[i] != 0;
}

static void mark(uint32_t i, int on)
{
    refs[i] = on ? 1 : 0;
}

static uint32_t index_of(uint32_t addr)
{
    if (addr < base || addr % PAGE_SIZE)
        return ~0u;
    uint32_t i = (uint32_t)((addr - base) / PAGE_SIZE);
    return i < frames ? i : ~0u;
}

void pmm_init_range(uint64_t ram_base, uint64_t ram_size, uint64_t first_free)
{
    ram_kib = (uint32_t)(ram_size / 1024);
    base = ram_base;
    uint64_t top = ram_base + ram_size;
    if (top > 0x100000000ULL)
        top = 0x100000000ULL;
    uint64_t n = top > ram_base ? (top - ram_base) / PAGE_SIZE : 0;
    frames = n > MAX_FRAMES ? MAX_FRAMES : (uint32_t)n;

    uint64_t reserved = (first_free - ram_base + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint32_t i = 0; i < frames && i < reserved; i++)
        mark(i, 1);
    used = reserved < frames ? (uint32_t)reserved : frames;
}

uint32_t pmm_alloc_contiguous(uint32_t n)
{
    if (!n)
        return 0;
    irqflags_t f = spin_lock_irq(&lock);
    uint32_t run = 0;
    for (uint32_t i = 0; i < frames; i++) {
        run = taken(i) ? 0 : run + 1;
        if (run == n) {
            uint32_t first = i + 1 - n;
            for (uint32_t k = first; k <= i; k++)
                mark(k, 1);
            used += n;
            spin_unlock_irq(&lock, f);
            return (uint32_t)(base + (uint64_t)first * PAGE_SIZE);
        }
    }
    spin_unlock_irq(&lock, f);
    return 0;
}

uint32_t pmm_alloc_frame(void)
{
    irqflags_t f = spin_lock_irq(&lock);
    for (uint32_t k = 0; k < frames; k++) {
        uint32_t i = hint + k < frames ? hint + k : hint + k - frames;
        if (!taken(i)) {
            mark(i, 1);
            used++;
            hint = i + 1 < frames ? i + 1 : 0;
            spin_unlock_irq(&lock, f);
            return (uint32_t)(base + (uint64_t)i * PAGE_SIZE);
        }
    }
    spin_unlock_irq(&lock, f);
    return 0;
}

/* RAM is RAM here: no zone below a gigabyte to keep for devices */
uint32_t pmm_alloc_user_frame(void)
{
    return pmm_alloc_frame();
}

void pmm_free_frame(uint32_t addr)
{
    uint32_t i = index_of(addr);
    if (i == ~0u)
        return;
    irqflags_t f = spin_lock_irq(&lock);
    if (taken(i) && --refs[i] == 0)
        used--;
    spin_unlock_irq(&lock, f);
}

void pmm_frame_ref(uint32_t addr)
{
    uint32_t i = index_of(addr);
    if (i == ~0u)
        return;
    irqflags_t f = spin_lock_irq(&lock);
    if (taken(i) && refs[i] < 0xFFFF)
        refs[i]++;
    spin_unlock_irq(&lock, f);
}

uint32_t pmm_frame_refs(uint32_t addr)
{
    uint32_t i = index_of(addr);
    return i == ~0u ? 0 : refs[i];
}

uint32_t pmm_total_frames(void) { return frames; }
uint32_t pmm_used_frames(void)  { return used; }
uint32_t pmm_ram_kib(void)      { return ram_kib; }
