/* Sterta jądra - kmalloc/kfree
 *
 * Prosty alokator first-fit na liście bloków. Sterta zajmuje 1 MiB
 * ciągłej pamięci fizycznej z PMM (jesteśmy w mapowaniu
 * identycznościowym, więc adres fizyczny == wirtualny).
 *
 * Każdy blok ma nagłówek z rozmiarem i flagą "wolny". Przy alokacji
 * blok jest dzielony, jeśli reszta ma sens; przy zwalnianiu blok
 * skleja się z wolnym sąsiadem z prawej (proste łagodzenie fragmentacji).
 */

#include "include/heap.h"
#include "include/pmm.h"
#include "include/printk.h"

#define HEAP_SIZE (1024u * 1024)
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define MIN_SPLIT 16u   /* nie twórz wolnych ścinków mniejszych niż to */

struct block {
    uint32_t size;          /* rozmiar danych za nagłówkiem */
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

    heap_head = (struct block *)heap_start;
    heap_head->size = HEAP_SIZE - sizeof(struct block);
    heap_head->free = 1;
    heap_head->next = (void *)0;
}

void *kmalloc(size_t size)
{
    if (!size)
        return (void *)0;
    size = ALIGN_UP(size, 8);

    for (struct block *b = heap_head; b; b = b->next) {
        if (!b->free || b->size < size)
            continue;

        /* Podziel blok, jeśli reszta pomieści nagłówek i sensowne dane */
        if (b->size >= size + sizeof(struct block) + MIN_SPLIT) {
            struct block *rest =
                (struct block *)((uint32_t)b + sizeof(struct block) + size);
            rest->size = b->size - size - sizeof(struct block);
            rest->free = 1;
            rest->next = b->next;
            b->size = size;
            b->next = rest;
        }
        b->free = 0;
        used_bytes += b->size;
        return (void *)((uint32_t)b + sizeof(struct block));
    }
    return (void *)0;   /* brak miejsca */
}

void kfree(void *ptr)
{
    if (!ptr)
        return;
    uint32_t addr = (uint32_t)ptr;
    if (addr < heap_start + sizeof(struct block) || addr >= heap_end)
        return;

    struct block *b = (struct block *)(addr - sizeof(struct block));
    if (b->free)
        return;
    b->free = 1;
    used_bytes -= b->size;

    /* Sklej z wolnym następnikiem */
    while (b->next && b->next->free) {
        b->size += sizeof(struct block) + b->next->size;
        b->next = b->next->next;
    }
}

uint32_t heap_total_bytes(void) { return HEAP_SIZE; }
uint32_t heap_used_bytes(void)  { return used_bytes; }
