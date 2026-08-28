/* Stan jednostki zmiennoprzecinkowej i SSE
 *
 * Rejestry XMM sa takim samym stanem zadania, jak RAX czy RSP - z ta
 * roznica, ze jest ich duzo i procesor zapisuje je jedna instrukcja
 * do obszaru 512 bajtow. Bez tego zapisu dwa programy liczace
 * rownoczesnie nadpisywalyby sobie wyniki, nie wiedzac o tym.
 *
 * Wyrownanie do szesnastu bajtow nie jest tu grzecznoscia: fxsave
 * i fxrstor po prostu odmawiaja pracy na zle wyrownanym adresie.
 */

#include "fpu64.h"
#include "../kernel/include/heap.h"
#include "../kernel/include/string.h"

#define FPU_AREA 512

/* Stan swiezo zainicjowanej jednostki - kazde nowe zadanie dostaje
   jego kopie, zamiast zaczynac od przypadkowych bitow */
static uint8_t czysty[FPU_AREA] __attribute__((aligned(16)));
static int wlaczone;

int fpu64_init(void)
{
    uint64_t cr0, cr4;

    __asm__ volatile("movq %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(uint64_t)(1 << 2);      /* EM: koniec z emulacja koprocesora */
    cr0 |= (1 << 1);                 /* MP: monitorowanie koprocesora */
    cr0 |= (1 << 5);                 /* NE: bledy przez wyjatek, nie IRQ */
    __asm__ volatile("movq %0, %%cr0" : : "r"(cr0));

    __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 9);                 /* OSFXSR: umiemy fxsave/fxrstor */
    cr4 |= (1 << 10);                /* OSXMMEXCPT: i bledy SIMD */
    __asm__ volatile("movq %0, %%cr4" : : "r"(cr4));

    __asm__ volatile("fninit");
    __asm__ volatile("fxsave (%0)" : : "r"(czysty) : "memory");

    wlaczone = 1;
    return 1;
}

int fpu64_enabled(void)
{
    return wlaczone;
}

void *fpu64_alloc(void)
{
    /* Zapasu jest 32 bajty, nie 16, i to nie z ostroznosci. Adres
       surowy chowamy tuz PRZED wyrownanym obszarem, bo inaczej nie
       byloby go jak zwolnic - a gdyby sterta oddala adres juz
       wyrowany, obszar zaczynalby sie dokladnie na nim i ten zapis
       trafilby poza przydzielona pamiec. Przesuniecie o 16 sprawia,
       ze miejsce na wskaznik jest zawsze wlasne. */
    uint8_t *raw = kmalloc(FPU_AREA + 32);
    if (!raw)
        return (void *)0;
    uint8_t *area = (uint8_t *)(((uintptr_t)raw + 16 + 15) & ~(uintptr_t)15);
    memcpy(area, czysty, FPU_AREA);

    ((uint8_t **)area)[-1] = raw;
    return area;
}

void fpu64_free(void *area)
{
    if (area)
        kfree(((uint8_t **)area)[-1]);
}

void fpu64_save(void *area)
{
    if (wlaczone && area)
        __asm__ volatile("fxsave (%0)" : : "r"(area) : "memory");
}

void fpu64_load(const void *area)
{
    if (wlaczone && area)
        __asm__ volatile("fxrstor (%0)" : : "r"(area) : "memory");
}
