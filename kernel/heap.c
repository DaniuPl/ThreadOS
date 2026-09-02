/* The kernel heap - kmalloc/kfree
 *
 * A simple first-fit allocator over a list of blocks. The heap takes
 * 1 MiB of physically contiguous memory from the PMM (we are in the
 * identity mapping, so the physical address == the virtual one).
 *
 * Every block has a header with a size and a "free" flag. On
 * allocation a block is split if the remainder makes sense; on freeing
 * a block is merged with a free neighbour on the right (simple
 * fragmentation relief).
 */

#include "include/heap.h"
#include "include/pmm.h"
#include "include/printk.h"
#include "include/io.h"
#include "include/spinlock.h"

#define HEAP_SIZE (1024u * 1024)
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define MIN_SPLIT 16u   /* do not create free scraps smaller than this */

struct block {
    uint32_t size;          /* the data size after the header */
    uint32_t free;
    struct block *next;
};

static struct block *heap_head;
static uint32_t heap_start, heap_end;
static uint32_t used_bytes;

void heap_init(void)
{
    heap_start = pmm_alloc_contiguous(HEAP_SIZE / PAGE_SIZE);
    if (!heap_start) {
        printk("heap: brak %u KiB ciaglej pamieci!\n", HEAP_SIZE / 1024);
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    heap_end = heap_start + HEAP_SIZE;

    heap_head = (struct block *)(uintptr_t)heap_start;
    heap_head->size = HEAP_SIZE - sizeof(struct block);
    heap_head->free = 1;
    heap_head->next = (void *)0;
}

/* The heap is used both by threads and by IRQ handlers (the scheduler
   reaps zombies) - hence the interrupts being turned off. Since there
   is more than one core, a lock is needed too: the block list is one,
   and two cores splitting the same block at the same time would leave
   behind a list nobody could walk. */
static struct spinlock lock = SPINLOCK_FREE;

void *kmalloc(size_t size)
{
    if (!size)
        return (void *)0;
    size = ALIGN_UP(size, 8);

    irqflags_t flags = spin_lock_irq(&lock);
    void *result = (void *)0;

    for (struct block *b = heap_head; b; b = b->next) {
        if (!b->free || b->size < size)
            continue;

        /* Split the block if the remainder can hold a header and sensible data */
        if (b->size >= size + sizeof(struct block) + MIN_SPLIT) {
            struct block *rest =
                (struct block *)((uintptr_t)b + sizeof(struct block) + size);
            rest->size = b->size - size - sizeof(struct block);
            rest->free = 1;
            rest->next = b->next;
            b->size = size;
            b->next = rest;
        }
        b->free = 0;
        used_bytes += b->size;
        result = (void *)((uintptr_t)b + sizeof(struct block));
        break;
    }

    spin_unlock_irq(&lock, flags);
    return result;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;
    uint32_t addr = (uint32_t)(uintptr_t)ptr;
    if (addr < heap_start + sizeof(struct block) || addr >= heap_end)
        return;

    irqflags_t flags = spin_lock_irq(&lock);

    struct block *b = (struct block *)(addr - sizeof(struct block));
    /* A pointer that is not a block of ours: the header in front of it
       would have to be a size, a free flag and a link into the heap.
       Freeing it would put rubbish into the list and the next kfree
       would walk off into nowhere - which is exactly what happened,
       and took a while to find, because the crash was in kfree and the
       fault was somewhere else entirely. Saying so is worth more than
       carrying on. */
    if (b->free > 1 || (b->next && ((uint32_t)(uintptr_t)b->next < heap_start ||
                                    (uint32_t)(uintptr_t)b->next >= heap_end))) {
        printk("[sterta] zly blok %x (free=%u next=%x), zwolniony z %x\n",
               (uint32_t)(uintptr_t)b, b->free, (uint32_t)(uintptr_t)b->next,
               (uint32_t)(uintptr_t)__builtin_return_address(0));
        spin_unlock_irq(&lock, flags);
        return;
    }
    if (!b->free) {
        b->free = 1;
        used_bytes -= b->size;

        /* Merge with a free successor */
        while (b->next && b->next->free) {
            b->size += sizeof(struct block) + b->next->size;
            b->next = b->next->next;
        }
    }

    spin_unlock_irq(&lock, flags);
}

uint32_t heap_total_bytes(void) { return HEAP_SIZE; }
uint32_t heap_used_bytes(void)  { return used_bytes; }
