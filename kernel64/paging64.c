/* Stronicowanie czteropoziomowe
 *
 * Adres wirtualny to cztery indeksy po dziewiec bitow i przesuniecie
 * w stronie. Kazdy poziom to jedna ramka 4 KiB z 512 wpisami po 64 bity.
 *
 * Pamiec opisujemy stronami po 2 MiB - jeden wpis zamiast 512, wiec
 * caly stumegabajtowy obszar mieści sie w kilku tablicach zamiast
 * w setkach. Drobniejsze mapowania robi paging64_map(), i to wlasnie
 * one beda potrzebne procesom.
 *
 * Doszedl bit, ktorego stronicowanie 32-bitowe nie mialo: zakaz
 * wykonywania. Strona z danymi moze byc od teraz oznaczona tak, ze
 * procesor odmowi wykonania czegokolwiek, co w niej lezy - bufor
 * obrazu czy sterta nie sa juz potencjalnym miejscem na cudzy kod.
 */

#include <stdint.h>
#include "paging64.h"
#include "../kernel/include/pmm.h"
#include "../kernel/include/fbcon.h"
#include "../kernel/include/string.h"
#include "../kernel/include/printk.h"

#define PTE_PRESENT 0x001
#define PTE_WRITE   0x002
#define PTE_HUGE    0x080
#define ADDR_MASK   0x000FFFFFFFFFF000ULL

#define HUGE_SIZE   (2u * 1024 * 1024)

static uint64_t *pml4;
static uint64_t identity_top;
static int nx_on;

static uint64_t *table_at(uint64_t entry)
{
    return (uint64_t *)(uintptr_t)(entry & ADDR_MASK);
}

/* Zwraca tablice nizszego poziomu, tworzac ja, gdy jeszcze nie ma */
static uint64_t *ensure(uint64_t *table, int idx)
{
    if (!(table[idx] & PTE_PRESENT)) {
        uint32_t frame = pmm_alloc_frame();
        if (!frame)
            return (uint64_t *)0;
        memset((void *)(uintptr_t)frame, 0, PAGE_SIZE);
        /* Prawa nadaje sie na kazdym poziomie osobno; na posrednich
           dajemy najszersze, bo o dostepie rozstrzyga i tak najbardziej
           restrykcyjny wpis na calej sciezce */
        table[idx] = (uint64_t)frame | PTE_PRESENT | PTE_WRITE;
    }
    return table_at(table[idx]);
}

static void invlpg(uint64_t addr)
{
    __asm__ volatile("invlpg (%0)" : : "r"((void *)(uintptr_t)addr) : "memory");
}

int paging64_map(uint64_t virt, uint64_t phys, uint64_t flags)
{
    if (!pml4)
        return -1;

    uint64_t *pdpt = ensure(pml4, (int)((virt >> 39) & 0x1FF));
    if (!pdpt)
        return -1;
    uint64_t *pd = ensure(pdpt, (int)((virt >> 30) & 0x1FF));
    if (!pd)
        return -1;

    int i2 = (int)((virt >> 21) & 0x1FF);
    if (pd[i2] & PTE_HUGE)
        return -1;              /* obszar opisany juz strona 2 MiB */
    uint64_t *pt = ensure(pd, i2);
    if (!pt)
        return -1;

    pt[(virt >> 12) & 0x1FF] = (phys & ADDR_MASK) | PTE_PRESENT | flags;
    invlpg(virt);
    return 0;
}

/* Jeden wpis na dwa megabajty - tak opisujemy pamiec i bufor obrazu */
static int map_huge(uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t *pdpt = ensure(pml4, (int)((virt >> 39) & 0x1FF));
    if (!pdpt)
        return -1;
    uint64_t *pd = ensure(pdpt, (int)((virt >> 30) & 0x1FF));
    if (!pd)
        return -1;
    pd[(virt >> 21) & 0x1FF] = (phys & ~(uint64_t)(HUGE_SIZE - 1)) |
                               PTE_PRESENT | PTE_HUGE | flags;
    return 0;
}

uint64_t paging64_lookup(uint64_t virt)
{
    if (!pml4)
        return 0;
    uint64_t e = pml4[(virt >> 39) & 0x1FF];
    if (!(e & PTE_PRESENT))
        return 0;
    e = table_at(e)[(virt >> 30) & 0x1FF];
    if (!(e & PTE_PRESENT))
        return 0;
    e = table_at(e)[(virt >> 21) & 0x1FF];
    if (!(e & PTE_PRESENT))
        return 0;
    if (e & PTE_HUGE)
        return (e & ADDR_MASK) + (virt & (HUGE_SIZE - 1));
    e = table_at(e)[(virt >> 12) & 0x1FF];
    if (!(e & PTE_PRESENT))
        return 0;
    return (e & ADDR_MASK) + (virt & 0xFFF);
}

uint64_t paging64_identity_top(void)
{
    return identity_top;
}

int paging64_nx_enabled(void)
{
    return nx_on;
}

int paging64_init(void)
{
    /* Zakaz wykonywania trzeba najpierw wlaczyc w EFER - inaczej bit 63
       wpisu jest zarezerwowany i jego ustawienie konczy sie bledem */
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000080));
    low |= (1u << 11);                       /* NXE */
    __asm__ volatile("wrmsr" : : "a"(low), "d"(high), "c"(0xC0000080));
    nx_on = 1;

    uint32_t root = pmm_alloc_frame();
    if (!root)
        return -1;
    memset((void *)(uintptr_t)root, 0, PAGE_SIZE);
    pml4 = (uint64_t *)(uintptr_t)root;

    /* Cala pamiec, ktora zarzadza PMM - identycznosciowo. Jadro,
       jego stos i wszystkie tablice stron leza w tym obszarze, wiec
       po przelaczeniu CR3 nic nam nie znika spod nog. */
    uint64_t top = (pmm_managed_top() + HUGE_SIZE - 1) & ~(uint64_t)(HUGE_SIZE - 1);
    for (uint64_t a = 0; a < top; a += HUGE_SIZE)
        if (map_huge(a, a, PTE_WRITE | PG_NX) < 0)
            return -1;
    identity_top = top;

    /* Kod jadra musi zostac wykonywalny - pierwsze dwa megabajty nad
       megabajtem to obraz jadra, wiec temu jednemu wpisowi zdejmujemy
       zakaz wykonywania */
    if (map_huge(0, 0, PTE_WRITE) < 0 ||
        map_huge(HUGE_SIZE, HUGE_SIZE, PTE_WRITE) < 0)
        return -1;

    /* Bufor obrazu lezy wysoko, poza pamiecia - i na pewno nie ma
       w nim kodu do wykonania */
    if (fbcon_have()) {
        uint64_t fb = fbcon_addr();
        uint64_t end = fb + fbcon_bytes();
        for (uint64_t a = fb & ~(uint64_t)(HUGE_SIZE - 1); a < end;
             a += HUGE_SIZE)
            if (map_huge(a, a, PTE_WRITE | PG_NX) < 0)
                return -1;
    }

    __asm__ volatile("movq %0, %%cr3" : : "r"((uint64_t)root) : "memory");
    return 0;
}
