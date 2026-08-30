/* PMM - physical memory management
 *
 * A bitmap of 4 KiB frames: a set bit = the frame is taken. Using the
 * Multiboot memory map we mark the available areas as free, and then
 * reserve the first 1 MiB (BIOS, VGA) and the kernel image.
 *
 * We manage memory up to PAGING_MANAGED_MAX - that is as much as
 * paging can cover with the identity mapping without stepping into
 * user space (see paging.c). The bitmap and the owner counters are
 * static for that upper bound; how many frames really exist is stated
 * only by the memory map from the bootloader.
 */

#include "include/pmm.h"
#include "include/paging.h"
#include "include/string.h"
#include "include/printk.h"
#include "include/io.h"
#include "include/spinlock.h"

#define MANAGED_LIMIT PAGING_MANAGED_MAX
#define MAX_FRAMES    (MANAGED_LIMIT / PAGE_SIZE)

/* Symbols from linker.ld marking out the kernel image in memory */
extern char _kernel_start[], _kernel_end[];

static uint32_t bitmap[MAX_FRAMES / 32];
/* A frame's owner count. An ordinary frame has 1; with copy-on-write
   (fork) the same physical frame ends up in several address spaces at
   once and is freed only when the last one gives it back. */
static uint16_t refs[MAX_FRAMES];
static uint32_t total_frames;   /* frames available to the PMM */
static uint32_t used_frames;
static uint32_t ram_kib;        /* all detected memory */
static uint32_t managed_top;    /* the address just past the last managed frame */

static void frame_set(uint32_t frame)   { bitmap[frame / 32] |=  (1u << (frame % 32)); }
static void frame_clear(uint32_t frame) { bitmap[frame / 32] &= ~(1u << (frame % 32)); }
static int  frame_test(uint32_t frame)  { return bitmap[frame / 32] & (1u << (frame % 32)); }

static void reserve_range(uint32_t start, uint32_t end)
{
    for (uint32_t f = start / PAGE_SIZE; f < (end + PAGE_SIZE - 1) / PAGE_SIZE; f++) {
        if (f >= MAX_FRAMES)
            break;
        if (!frame_test(f)) {
            frame_set(f);
            refs[f] = 1;
            used_frames++;
        }
    }
}

void pmm_init(const struct multiboot_info *mbi)
{
    /* Everything taken, until the memory map says otherwise */
    memset(bitmap, 0xFF, sizeof(bitmap));

    if (!(mbi->flags & MULTIBOOT_FLAG_MMAP)) {
        printk("PMM: brak mapy pamieci od bootloadera!\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }

    uint32_t entry_addr = mbi->mmap_addr;
    uint32_t mmap_end = mbi->mmap_addr + mbi->mmap_length;

    while (entry_addr < mmap_end) {
        const struct multiboot_mmap_entry *e =
            (const struct multiboot_mmap_entry *)(uintptr_t)entry_addr;

        if (e->type == MULTIBOOT_MEMORY_AVAILABLE) {
            ram_kib += (uint32_t)(e->len / 1024);

            uint64_t start = e->addr;
            uint64_t end = e->addr + e->len;
            if (start < MANAGED_LIMIT) {
                if (end > MANAGED_LIMIT)
                    end = MANAGED_LIMIT;
                /* whole frames within the area only */
                uint32_t f_start = ((uint32_t)start + PAGE_SIZE - 1) / PAGE_SIZE;
                uint32_t f_end = (uint32_t)(end / PAGE_SIZE);
                for (uint32_t f = f_start; f < f_end; f++) {
                    frame_clear(f);
                    total_frames++;
                }
                if (f_end * PAGE_SIZE > managed_top)
                    managed_top = f_end * PAGE_SIZE;
            }
        }
        entry_addr += e->size + 4;
    }

    used_frames = 0;

    /* The first 1 MiB: BIOS vectors, the VGA buffer, bootloader structures */
    reserve_range(0, 1024 * 1024);
    /* The kernel image (code + data + bss) */
    reserve_range((uint32_t)(uintptr_t)_kernel_start,
                  (uint32_t)(uintptr_t)_kernel_end);

    /* Bootloader modules (the initrd) - they must not be overwritten */
    if (mbi->flags & MULTIBOOT_FLAG_MODS) {
        const struct multiboot_mod *mods =
            (const struct multiboot_mod *)(uintptr_t)mbi->mods_addr;
        reserve_range(mbi->mods_addr,
                      mbi->mods_addr + mbi->mods_count * sizeof(*mods));
        for (uint32_t i = 0; i < mbi->mods_count; i++)
            reserve_range(mods[i].start, mods[i].end);
    }
}

uint32_t pmm_alloc_frame(void)
{
    return pmm_alloc_contiguous(1);
}

/* Allocation and freeing also run from IRQ context (exec, cleaning up
   zombie address spaces) - hence the interrupts being turned off.
   Since there is more than one core, that is no longer enough: two
   cores can really enter here at the same time and hand out the same
   frame twice. Hence the lock. The frame map is one for the whole
   machine, so the lock is one too. */
static struct spinlock lock = SPINLOCK_FREE;

/* Whom to ask for memory when it runs out (set by the swapper). The
   PMM knows nothing about processes or disks - it is only given a
   function that has to free at least one frame. */
static int (*reclaim)(void);

void pmm_set_reclaim(int (*fn)(void))
{
    reclaim = fn;
}

static uint32_t alloc_contiguous_raw(uint32_t n)
{
    irqflags_t flags = spin_lock_irq(&lock);
    uint32_t run = 0;
    for (uint32_t f = 0; f < MAX_FRAMES; f++) {
        if (frame_test(f)) {
            run = 0;
            continue;
        }
        if (++run == n) {
            uint32_t first = f - n + 1;
            for (uint32_t i = first; i <= f; i++) {
                frame_set(i);
                refs[i] = 1;
            }
            used_frames += n;
            spin_unlock_irq(&lock, flags);
            return first * PAGE_SIZE;
        }
    }
    spin_unlock_irq(&lock, flags);
    return 0;
}

uint32_t pmm_alloc_contiguous(uint32_t n)
{
    uint32_t addr = alloc_contiguous_raw(n);
    if (addr || !reclaim)
        return addr;

    /* No free frames: try to reclaim as many as are needed by
       evicting unused process pages to disk. When reclaiming stops
       yielding anything, there really is no memory. */
    for (uint32_t tries = 0; tries < n + 4; tries++) {
        if (!reclaim())
            break;
        addr = alloc_contiguous_raw(n);
        if (addr)
            return addr;
    }
    return 0;
}

void pmm_free_frame(uint32_t addr)
{
    irqflags_t flags = spin_lock_irq(&lock);
    uint32_t frame = addr / PAGE_SIZE;
    /* A safety catch: the kernel image never goes back into the pool.
       If some page table pointed at the kernel's own code or data, the
       frame would end up in the first process to come along - better
       to protest loudly than to quietly hand the system over to be
       rewritten. */
    if (addr >= (uint32_t)(uintptr_t)_kernel_start &&
        addr < (uint32_t)(uintptr_t)_kernel_end) {
        spin_unlock_irq(&lock, flags);
        printk("[PMM] proba zwolnienia ramki obrazu jadra (%x) - odmawiam\n",
               addr);
        return;
    }
    if (frame < MAX_FRAMES && frame_test(frame)) {
        if (refs[frame] > 1) {
            refs[frame]--;              /* the frame still has owners */
        } else {
            refs[frame] = 0;
            frame_clear(frame);
            used_frames--;
        }
    }
    spin_unlock_irq(&lock, flags);
}

/* Adds an owner to a frame (fork: the child inherits the parent's pages) */
void pmm_frame_ref(uint32_t addr)
{
    irqflags_t flags = spin_lock_irq(&lock);
    uint32_t frame = addr / PAGE_SIZE;
    if (frame < MAX_FRAMES && frame_test(frame) && refs[frame] < 0xFFFF)
        refs[frame]++;
    spin_unlock_irq(&lock, flags);
}

/* How many owners a frame has (0 = free) */
uint32_t pmm_frame_refs(uint32_t addr)
{
    uint32_t frame = addr / PAGE_SIZE;
    return frame < MAX_FRAMES ? refs[frame] : 0;
}

uint32_t pmm_total_frames(void) { return total_frames; }
uint32_t pmm_used_frames(void)  { return used_frames; }
uint32_t pmm_ram_kib(void)      { return ram_kib; }
uint32_t pmm_managed_top(void)  { return managed_top; }
