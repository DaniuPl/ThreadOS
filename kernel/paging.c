/* Stronicowanie
 *
 * Pierwsze 64 MiB mapujemy identycznościowo (adres wirtualny = fizyczny)
 * stronami 4 KiB i włączamy bit PG w CR0. Jądro działa dalej pod tymi
 * samymi adresami, ale od teraz każdy dostęp poza mapowany obszar
 * wywoła page fault zamiast cichej korupcji pamięci.
 *
 * Katalog i tablice stron są statyczne - 64 MiB to 16 tablic po 4 KiB.
 */

#include <stdint.h>
#include "include/paging.h"

#define PTE_PRESENT 0x1
#define PTE_WRITE   0x2
#define PTE_USER    0x4

#define NUM_TABLES (PAGING_IDENTITY_LIMIT / (4096u * 1024))

static uint32_t page_directory[1024] __attribute__((aligned(4096)));
static uint32_t page_tables[NUM_TABLES][1024] __attribute__((aligned(4096)));

void paging_init(void)
{
    for (uint32_t t = 0; t < NUM_TABLES; t++) {
        for (uint32_t i = 0; i < 1024; i++)
            page_tables[t][i] = ((t * 1024 + i) * 4096) | PTE_PRESENT | PTE_WRITE;
        page_directory[t] = (uint32_t)page_tables[t] | PTE_PRESENT | PTE_WRITE;
    }

    __asm__ volatile(
        "movl %0, %%cr3\n\t"
        "movl %%cr0, %%eax\n\t"
        "orl $0x80000000, %%eax\n\t"
        "movl %%eax, %%cr0\n\t"
        :
        : "r"(page_directory)
        : "eax", "memory");
}

void paging_make_user(uint32_t addr, uint32_t size)
{
    uint32_t start = addr & ~0xFFFu;
    uint32_t end = addr + size;

    for (uint32_t a = start; a < end; a += 4096) {
        if (a >= PAGING_IDENTITY_LIMIT)
            break;
        uint32_t pde = a >> 22;
        uint32_t pte = (a >> 12) & 0x3FF;
        /* USER musi byc i w katalogu, i we wpisie strony - reszta
           stron tej tablicy zostaje supervisor, wiec nic nie tracimy */
        page_directory[pde] |= PTE_USER;
        page_tables[pde][pte] |= PTE_USER;
        __asm__ volatile("invlpg (%0)" : : "r"(a) : "memory");
    }
}
