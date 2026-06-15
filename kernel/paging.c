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
#include "include/pmm.h"
#include "include/string.h"
#include "include/shm.h"

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

uint32_t paging_kernel_cr3(void)
{
    return (uint32_t)page_directory;
}

int paging_map_phys(uint32_t phys, uint32_t size)
{
    uint32_t start = phys & ~0xFFFu;
    uint32_t end = (phys + size + 0xFFFu) & ~0xFFFu;

    for (uint32_t a = start; a < end; a += 4096) {
        uint32_t pde = a >> 22;
        uint32_t pte = (a >> 12) & 0x3FF;
        if (!(page_directory[pde] & PTE_PRESENT)) {
            uint32_t pt = pmm_alloc_frame();   /* ramka <64 MiB, identycznie */
            if (!pt)
                return -1;
            memset((void *)pt, 0, 4096);
            page_directory[pde] = pt | PTE_PRESENT | PTE_WRITE;
        }
        uint32_t *pt = (uint32_t *)(page_directory[pde] & ~0xFFFu);
        pt[pte] = a | PTE_PRESENT | PTE_WRITE;
        __asm__ volatile("invlpg (%0)" : : "r"(a) : "memory");
    }
    return 0;
}

/* Wszystkie ramki z PMM leżą <64 MiB, więc jądro widzi je przez
   mapowanie identycznościowe - katalogi i tablice stron procesów
   modyfikujemy wprost po adresie fizycznym. */

uint32_t paging_new_address_space(void)
{
    uint32_t dir_phys = pmm_alloc_frame();
    if (!dir_phys)
        return 0;
    uint32_t *dir = (uint32_t *)dir_phys;
    /* Mapowania jądra współdzielone (te same tablice stron, supervisor),
       reszta wpisów pusta - tam zamieszka program użytkownika */
    for (int i = 0; i < 1024; i++)
        dir[i] = page_directory[i];
    return dir_phys;
}

int paging_map_user_in(uint32_t dir_phys, uint32_t vaddr, uint32_t frame)
{
    /* Nie pozwól zamapować strony użytkownika na obszarze jądra */
    if (vaddr < PAGING_IDENTITY_LIMIT)
        return 0;

    uint32_t *dir = (uint32_t *)dir_phys;
    uint32_t pde = vaddr >> 22;
    uint32_t pte = (vaddr >> 12) & 0x3FF;
    uint32_t pt_phys;

    if (!(dir[pde] & PTE_PRESENT)) {
        pt_phys = pmm_alloc_frame();
        if (!pt_phys)
            return 0;
        memset((void *)pt_phys, 0, 4096);
        dir[pde] = pt_phys | PTE_PRESENT | PTE_WRITE | PTE_USER;
    } else {
        if (dir[pde] == page_directory[pde])  /* tablica jądra - nie ruszaj */
            return 0;
        pt_phys = dir[pde] & ~0xFFFu;
    }

    ((uint32_t *)pt_phys)[pte] = frame | PTE_PRESENT | PTE_WRITE | PTE_USER;
    /* Jeśli mapujemy do aktywnej przestrzeni (sbrk), odśwież TLB */
    uint32_t active;
    __asm__ volatile("movl %%cr3, %0" : "=r"(active));
    if (active == dir_phys)
        __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
    return 1;
}

uint32_t paging_get_mapping(uint32_t dir_phys, uint32_t vaddr)
{
    uint32_t *dir = (uint32_t *)dir_phys;
    uint32_t pde = vaddr >> 22;
    if (!(dir[pde] & PTE_PRESENT) || dir[pde] == page_directory[pde])
        return 0;
    uint32_t pte = ((uint32_t *)(dir[pde] & ~0xFFFu))[(vaddr >> 12) & 0x3FF];
    return pte & PTE_PRESENT ? pte & ~0xFFFu : 0;
}

void paging_destroy_address_space(uint32_t dir_phys)
{
    uint32_t *dir = (uint32_t *)dir_phys;
    for (int i = 0; i < 1024; i++) {
        /* Zwalniaj tylko tablice procesu - jądrowe są współdzielone */
        if (!(dir[i] & PTE_PRESENT) || dir[i] == page_directory[i])
            continue;
        uint32_t *pt = (uint32_t *)(dir[i] & ~0xFFFu);
        for (int j = 0; j < 1024; j++) {
            if (!(pt[j] & PTE_PRESENT))
                continue;
            uint32_t frame = pt[j] & ~0xFFFu;
            /* Ramek pamieci wspoldzielonej nie zwalniamy - moga ich
               uzywac inne procesy */
            if (!shm_is_shared(frame))
                pmm_free_frame(frame);
        }
        pmm_free_frame((uint32_t)pt);
    }
    pmm_free_frame(dir_phys);
}
