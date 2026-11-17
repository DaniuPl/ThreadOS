/* PMM - physical memory management
 *
 * A bitmap of 4 KiB frames: a set bit = the frame is taken. Using the
 * Multiboot memory map we mark the available areas as free, and then
 * reserve the first 1 MiB (BIOS, VGA) and the kernel image.
 *
 * Memory comes in two parts. The low gigabyte is identity-mapped -
 * a frame's address is where the kernel finds it - and everything the
 * kernel itself keeps comes from there: page tables, the heap, buffers
 * devices read and write by physical address. It cannot reach higher:
 * programs lie from 1 GiB (USER64_BASE in kernel64/elf64.h), where the
 * identity map would be. Above it, up to MANAGED_LIMIT, are frames for
 * programs' own pages only (pmm_alloc_user_frame), which a program
 * sees through its page tables and the kernel, when it must, through
 * the direct map of all memory (phys_ptr, kernel64/paging64.h). That is
 * where a web engine's hundreds of megabytes go.
 *
 * Frames are 32-bit addresses, so MANAGED_LIMIT is 4 GiB; memory above
 * it is counted but not used. The bitmap and the owner counters are
 * static for that bound; how many frames really exist is stated only
 * by the memory map from the bootloader.
 */

#include "include/pmm.h"
#include "include/string.h"
#include "include/printk.h"
#include "include/io.h"
#include "include/spinlock.h"

#define MANAGED_LIMIT (4ULL << 30)
#define MAX_FRAMES    ((uint32_t)(MANAGED_LIMIT / PAGE_SIZE))
#define LOW_LIMIT     (1u << 30)          /* the identity-mapped part */
#define LOW_FRAMES    (LOW_LIMIT / PAGE_SIZE)

/* Symbols from kernel64/linker64.ld marking out the kernel image in memory */
extern char _kernel_start[], _kernel_end[];

static uint32_t bitmap[MAX_FRAMES / 32];
/* A frame's owner count. An ordinary frame has 1; with copy-on-write
   (fork) the same physical frame ends up in several address spaces at
   once and is freed only when the last one gives it back. */
static uint16_t refs[MAX_FRAMES];
static uint32_t total_frames;   /* frames available to the PMM */
static uint32_t used_frames;
static uint32_t low_total, low_used;   /* the same, of the low gigabyte */
static uint32_t ram_kib;        /* all detected memory */
static uint64_t managed_top;    /* the address just past the last managed frame */

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
            low_used += f < LOW_FRAMES;
        }
    }
}

void pmm_init(const struct multiboot_info *mbi)
{
    /* Everything taken, until the memory map says otherwise */
    memset(bitmap, 0xFF, sizeof(bitmap));

    if (!(mbi->flags & MULTIBOOT_FLAG_MMAP)) {
        printk("PMM: no memory map from the boot loader!\n");
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
                uint32_t f_start = (uint32_t)((start + PAGE_SIZE - 1) / PAGE_SIZE);
                uint32_t f_end = (uint32_t)(end / PAGE_SIZE);
                for (uint32_t f = f_start; f < f_end; f++) {
                    frame_clear(f);
                    total_frames++;
                    low_total += f < LOW_FRAMES;
                }
                if ((uint64_t)f_end * PAGE_SIZE > managed_top)
                    managed_top = (uint64_t)f_end * PAGE_SIZE;
            }
        }
        entry_addr += e->size + 4;
    }

    used_frames = 0;
    low_used = 0;

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

/* Where the last search stopped.
 *
 * Without it every search starts from frame zero, so handing out N
 * frames costs N squared - and the lock is held for all of it. It is
 * not a correctness question but it is not a detail either: a program
 * touching six thousand pages spent more time walking a bitmap it had
 * already walked than doing anything else.
 *
 * A hint, not a promise: freeing does not move it back, so a search
 * that finds nothing ahead starts over from the beginning. */
static uint32_t cursor, high_cursor = LOW_FRAMES;

/* One free frame in [from, to), the search starting at '*at' and going
   round once. A word of the bitmap with all 32 taken is passed over
   whole: a full gigabyte is 8192 words, not 262144 bits. Called with
   the lock held. */
static uint32_t take_one(uint32_t from, uint32_t to, uint32_t *at)
{
    uint32_t span = to - from;
    uint32_t f = *at < from || *at >= to ? from : *at;
    for (uint32_t seen = 0; seen < span; ) {
        if ((f & 31) == 0 && bitmap[f / 32] == 0xFFFFFFFFu && f + 32 <= to) {
            f += 32;
            seen += 32;
        } else {
            if (!frame_test(f)) {
                frame_set(f);
                refs[f] = 1;
                used_frames++;
                low_used += f < LOW_FRAMES;
                *at = f + 1 < to ? f + 1 : from;
                return f * PAGE_SIZE;
            }
            f++;
            seen++;
        }
        if (f >= to)
            f = from;
    }
    return 0;
}

static uint32_t alloc_contiguous_raw(uint32_t n)
{
    irqflags_t flags = spin_lock_irq(&lock);

    /* One frame is what almost every caller wants, and for one frame a
       run of consecutive frames is not needed - so start where the
       last one was found and go round once. */
    if (n == 1) {
        uint32_t addr = take_one(0, LOW_FRAMES, &cursor);
        spin_unlock_irq(&lock, flags);
        return addr;
    }

    /* A run of several frames in a row - a device buffer, and rare
       enough that walking the whole map is not worth avoiding */
    uint32_t run = 0;
    for (uint32_t f = 0; f < LOW_FRAMES; f++) {
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
            low_used += n;                  /* (runs are all low) */
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

/* What of the low gigabyte programs leave to the kernel on a machine
   with memory above it. There, evicting a program's page to disk may
   well free a high frame, which is no use to a page table - so the
   kernel's own allocations keep this much that programs never take.
   (With all memory low, a program's evicted page frees a low frame,
   and no reserve is needed: the machine behaves as it always did.) */
#define LOW_RESERVE ((16u * 1024 * 1024) / PAGE_SIZE)

static uint32_t user_raw(void)
{
    uint32_t top = (uint32_t)(managed_top / PAGE_SIZE);
    uint32_t addr = 0;
    irqflags_t flags = spin_lock_irq(&lock);
    if (top > LOW_FRAMES)
        addr = take_one(LOW_FRAMES, top, &high_cursor);
    if (!addr && (top <= LOW_FRAMES || low_total - low_used > LOW_RESERVE))
        addr = take_one(0, LOW_FRAMES, &cursor);
    spin_unlock_irq(&lock, flags);
    return addr;
}

/* A frame for a page of a program: from above the low gigabyte while
   there is any, so the low one stays for what only it can hold. When
   there is none, pages go to the swap file as for any frame. */
uint32_t pmm_alloc_user_frame(void)
{
    uint32_t addr = user_raw();
    for (uint32_t tries = 0; !addr && reclaim && tries < 4; tries++) {
        if (!reclaim())
            break;
        addr = user_raw();
    }
    return addr;
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
        printk("[PMM] an attempt to free a frame of the kernel image (%x) - refused\n",
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
            low_used -= frame < LOW_FRAMES;
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
uint64_t pmm_managed_top(void)  { return managed_top; }
uint64_t pmm_low_top(void)
{
    return managed_top < LOW_LIMIT ? managed_top : LOW_LIMIT;
}
