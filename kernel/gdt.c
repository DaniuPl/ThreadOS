/* GDT - Global Descriptor Table
 *
 * GRUB zostawia nam swoją GDT, ale nie mamy gwarancji, gdzie leży
 * i co zawiera - jądro musi załadować własną. Używamy płaskiego
 * modelu pamięci: wszystkie segmenty obejmują pełne 4 GiB.
 * Segmenty użytkownika (ring 3) już przygotowane pod przyszłe procesy.
 */

#include <stdint.h>
#include "include/gdt.h"
#include "include/smp.h"

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;   /* górne 4 bity limitu + flagi */
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* TSS - w trybie chronionym używany tylko po to, by CPU wiedział,
   na jaki stos jądra (ss0:esp0) przeskoczyć przy przerwaniu z ring 3 */
struct tss {
    uint32_t prev_tss;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t unused[23];
} __attribute__((packed));

/* Segment stanu zadania na KAZDY rdzen.
 *
 * Procesor czyta z niego stos jadra przy wejsciu z ring 3 - a kazdy
 * rdzen wchodzi tam z innego zadania i na inny stos. Jeden wspolny
 * segment znaczylby, ze dwa rdzenie wchodza do jadra na ten sam stos,
 * i to jest dokladnie ten rodzaj bledu, ktory nie objawia sie od razu.
 *
 * Deskryptorow tez musi byc tyle, ile rdzeni: instrukcja ltr oznacza
 * deskryptor jako ZAJETY, wiec drugi rdzen ladujacy ten sam selektor
 * dostalby wyjatek ochrony. Tablica zostaje jedna - rozne sa tylko
 * wpisy i selektory. */
static struct gdt_entry gdt[5 + SMP_RDZENI];
static struct gdt_ptr gdt_pointer;
static struct tss tss[SMP_RDZENI];

static void gdt_set_entry(int i, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t gran)
{
    gdt[i].base_low    = base & 0xFFFF;
    gdt[i].base_middle = (base >> 16) & 0xFF;
    gdt[i].base_high   = (base >> 24) & 0xFF;
    gdt[i].limit_low   = limit & 0xFFFF;
    gdt[i].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[i].access      = access;
}

void gdt_init(void)
{
    gdt_pointer.limit = sizeof(gdt) - 1;
    gdt_pointer.base  = (uint32_t)&gdt;

    /* access: P | DPL | S | typ; gran: G (strony 4K) | D (32-bit) */
    gdt_set_entry(0, 0, 0, 0, 0);                   /* null */
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xC0);       /* kod jądra, ring 0 */
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xC0);       /* dane jądra, ring 0 */
    gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0xC0);       /* kod użytkownika, ring 3 */
    gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xC0);       /* dane użytkownika, ring 3 */

    for (int i = 0; i < SMP_RDZENI; i++) {
        tss[i].ss0 = GDT_KERNEL_DATA;
        /* access 0x89 = obecny, 32-bit TSS; gran 0x00 = limit w bajtach */
        gdt_set_entry(5 + i, (uint32_t)&tss[i], sizeof(tss[i]) - 1,
                      0x89, 0x00);
    }

    gdt_zaladuj();

    /* Segment stanu zadania laduje TYLKO rdzen rozruchowy. Instrukcja
       ltr oznacza deskryptor jako zajety, wiec ta sama tablica nie da
       sie zaladowac dwa razy - a i tak nie ma po co: TSS jest jeden,
       bo sluzy wejsciu w ring 0 z ring 3, a programy uzytkownika
       chodza na razie tylko na rdzeniu rozruchowym. */
    gdt_zaladuj_tss();
}

/* Kazdy rdzen laduje SWOJ segment stanu zadania. Selektor liczy sie
   z jego numeru: piaty wpis tablicy plus tyle, ile wynosi numer. */
void gdt_zaladuj_tss(void)
{
    uint16_t sel = (uint16_t)(GDT_TSS + smp_rdzen_apic() * 8);
    __asm__ volatile("ltr %0" : : "r"(sel) : "memory");
}

/* Laduje juz zbudowana tablice na biezacym rdzeniu i przeladowuje
   rejestry segmentowe; CS wymaga dalekiego skoku. Rdzen wtorny wola to
   po przejsciu w tryb chroniony, zeby zejsc z tymczasowej tablicy
   z trampoliny na wspolna tablice jadra. */
void gdt_zaladuj(void)
{
    __asm__ volatile(
        "lgdt %0\n\t"
        "movw %1, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        "ljmpl %2, $1f\n\t"
        "1:\n\t"
        :
        : "m"(gdt_pointer), "i"(GDT_KERNEL_DATA), "i"(GDT_KERNEL_CODE)
        : "eax", "memory");
}

void tss_set_esp0(uint32_t esp0)
{
    tss[smp_rdzen()].esp0 = esp0;
}
