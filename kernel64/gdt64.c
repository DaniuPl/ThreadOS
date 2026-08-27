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

/* Piec zwyklych wpisow po osiem bajtow plus TSS, ktory zajmuje dwa */
static uint64_t gdt[7];
static struct tss64 tss;
static struct gdt_ptr gdtp;

static uint64_t entry(uint8_t access, uint8_t flags)
{
    /* Baza zero, limit maksymalny - i tak nikt ich nie oglada */
    return ((uint64_t)access << 40) | ((uint64_t)flags << 52) | 0xFFFFULL;
}

void gdt64_set_kernel_stack(uint64_t rsp)
{
    tss.rsp0 = rsp;
}

void gdt64_init(void)
{
    memset(&tss, 0, sizeof(tss));
    tss.iomap_base = sizeof(tss);        /* brak mapy portow */

    gdt[0] = 0;
    gdt[1] = entry(0x9A, 0x2);           /* kod jadra, bit L */
    gdt[2] = entry(0x92, 0x0);           /* dane jadra */
    gdt[3] = entry(0xF2, 0x0);           /* dane uzytkownika (ring 3) */
    gdt[4] = entry(0xFA, 0x2);           /* kod uzytkownika, bit L */

    /* Deskryptor TSS zajmuje dwa wpisy - adres ma 64 bity */
    uint64_t base = (uint64_t)(uintptr_t)&tss;
    uint64_t limit = sizeof(tss) - 1;
    gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) |
             ((uint64_t)0x89 << 40) | (((limit >> 16) & 0xF) << 48) |
             (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;

    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base = (uint64_t)(uintptr_t)gdt;

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
        "movw %w3, %%ax\n\t"
        "ltr %%ax"
        :
        : "m"(gdtp), "i"((uint64_t)GDT64_KERNEL_CODE),
          "i"(GDT64_KERNEL_DATA), "i"(GDT64_TSS)
        : "rax", "memory");
}
