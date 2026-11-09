/* The page allocator of the ARM64 kernel
 *
 * The x86 kernels build theirs from the Multiboot memory map; here the
 * memory comes from the device tree, as one range. Everything else is
 * the same promise kernel/include/pmm.h makes - which is the point: the
 * shared kernel/heap.c is built on it unchanged.
 *
 * One bit per 4 KiB page. Taken from the start: the device tree, the
 * gap below the kernel, the kernel image with its .bss - all of it up
 * to first_free. The interface speaks 32-bit addresses, so only RAM
 * below 4 GiB is handed out; on virt that is the first three gigabytes,
 * which is more than this kernel will want for a long time.
 */

#include <stdint.h>
#include "arm64.h"
#include "../kernel/include/pmm.h"
#include "../kernel/include/spinlock.h"

#define MAX_FRAMES (3u << 18)            /* 3 GiB of 4 KiB pages */

static uint8_t bitmap[MAX_FRAMES / 8];
static uint64_t base;                    /* the first page's address */
static uint32_t frames, used;
static uint32_t ram_kib;
static struct spinlock lock = SPINLOCK_FREE;

static int taken(uint32_t i)
{
    return bitmap[i / 8] & (1u << (i % 8));
}

static void mark(uint32_t i, int on)
{
    if (on)
        bitmap[i / 8] |= (uint8_t)(1u << (i % 8));
    else
        bitmap[i / 8] &= (uint8_t)~(1u << (i % 8));
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
    return pmm_alloc_contiguous(1);
}

void pmm_free_frame(uint32_t addr)
{
    if (addr < base || addr % PAGE_SIZE)
        return;
    uint32_t i = (uint32_t)((addr - base) / PAGE_SIZE);
    irqflags_t f = spin_lock_irq(&lock);
    if (i < frames && taken(i)) {
        mark(i, 0);
        used--;
    }
    spin_unlock_irq(&lock, f);
}

uint32_t pmm_total_frames(void) { return frames; }
uint32_t pmm_used_frames(void)  { return used; }
uint32_t pmm_ram_kib(void)      { return ram_kib; }
