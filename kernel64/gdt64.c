/* Tablica deskryptorow trybu dlugiego
 *
 * W trybie dlugim baza i limit segmentu nie znacza juz nic - procesor
 * ich nie sprawdza. Zostaly trzy rzeczy: poziom uprawnien, bit L
 * (kod 64-bitowy) i to, czy deskryptor opisuje kod czy dane.
 *
 * Kolejnosc wpisow nie jest dowolna. Instrukcja sysret, ktora wraca
 * z jadra do programu, nie bierze selektorow z pamieci - sklada je
 * z jednej wartosci zapisanej w rejestrze STAR: dane to ta wartosc
 * plus osiem, kod plus szesnascie. Segment danych uzytkownika musi
 * wiec lezec przed jego segmentem kodu, choc intuicja podpowiada
 * odwrotnie.
 *
 * TSS w trybie dlugim nie sluzy juz do przelaczania zadan. Zostalo mu
 * jedno, ale niezbywalne zadanie: powiedziec procesorowi, na ktory stos
 * ma przejsc, gdy przerwanie zastanie go w ring 3.
 */

#include <stdint.h>
#include "gdt64.h"
#include "../kernel/include/string.h"
#include "../kernel/include/smp.h"

struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* Segment stanu zadania na KAZDY rdzen.
 *
 * Procesor czyta z niego stos jadra przy wejsciu z ring 3 - a kazdy
 * rdzen wchodzi tam z innego zadania i na inny stos. Deskryptorow tez
 * musi byc tyle, ile rdzeni: ltr oznacza deskryptor jako ZAJETY, wiec
 * drugi rdzen ladujacy ten sam selektor dostalby wyjatek ochrony.
 *
 * Piec zwyklych wpisow po osiem bajtow, a potem po DWA na kazdy TSS -
 * jego adres ma szescdziesiat cztery bity i w jeden wpis sie nie
 * miesci. */
static uint64_t gdt[5 + 2 * SMP_RDZENI];
static struct tss64 tss[SMP_RDZENI];
static struct gdt_ptr gdtp;

static uint64_t entry(uint8_t access, uint8_t flags)
{
    /* Baza zero, limit maksymalny - i tak nikt ich nie oglada */
    return ((uint64_t)access << 40) | ((uint64_t)flags << 52) | 0xFFFFULL;
}

void gdt64_set_kernel_stack(uint64_t rsp)
{
    tss[smp_rdzen()].rsp0 = rsp;
}

void gdt64_init(void)
{
    memset(tss, 0, sizeof(tss));
    for (int i = 0; i < SMP_RDZENI; i++)
        tss[i].iomap_base = sizeof(tss[i]);   /* brak mapy portow */

    gdt[0] = 0;
    gdt[1] = entry(0x9A, 0x2);           /* kod jadra, bit L */
    gdt[2] = entry(0x92, 0x0);           /* dane jadra */
    gdt[3] = entry(0xF2, 0x0);           /* dane uzytkownika (ring 3) */
    gdt[4] = entry(0xFA, 0x2);           /* kod uzytkownika, bit L */

    /* Deskryptor TSS zajmuje dwa wpisy - adres ma 64 bity */
    for (int i = 0; i < SMP_RDZENI; i++) {
        uint64_t base = (uint64_t)(uintptr_t)&tss[i];
        uint64_t limit = sizeof(tss[i]) - 1;
        gdt[5 + 2 * i] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) |
                         ((uint64_t)0x89 << 40) |
                         (((limit >> 16) & 0xF) << 48) |
                         (((base >> 24) & 0xFF) << 56);
        gdt[6 + 2 * i] = base >> 32;
    }

    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base = (uint64_t)(uintptr_t)gdt;

    gdt64_zaladuj();

    gdt64_zaladuj_tss();
}

/* Kazdy rdzen laduje SWOJ segment stanu zadania. Selektor liczy sie
   z jego numeru: piaty wpis tablicy plus po szesnascie bajtow na
   kazdy poprzedni rdzen. */
void gdt64_zaladuj_tss(void)
{
    uint16_t sel = (uint16_t)(GDT64_TSS + smp_rdzen() * 16);
    __asm__ volatile("ltr %0" : : "r"(sel) : "memory");
}

/* Laduje juz zbudowana tablice na biezacym rdzeniu. Rdzen wtorny wola
   to po dojsciu do trybu dlugiego, zeby zejsc z tymczasowej tablicy
   z trampoliny na wspolna tablice jadra. */
void gdt64_zaladuj(void)
{
    __asm__ volatile(
        "lgdt %0\n\t"
        /* Przeladowanie CS wymaga skoku dalekiego; w trybie dlugim
           robi sie go przez powrot z odlozonym selektorem */
        "pushq %1\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "movw %w2, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        :
        : "m"(gdtp), "i"((uint64_t)GDT64_KERNEL_CODE),
          "i"(GDT64_KERNEL_DATA)
        : "rax", "memory");
}
