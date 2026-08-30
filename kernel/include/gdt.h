#ifndef PROTONOS_GDT_H
#define PROTONOS_GDT_H

#include <stdint.h>

/* Selektory segmentów (indeks w GDT * 8) */
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x18
#define GDT_USER_DATA   0x20
#define GDT_TSS         0x28

void gdt_init(void);

/* Laduje juz zbudowana tablice na biezacym rdzeniu (rdzenie wtorne) */
void gdt_load(void);

/* Laduje segment stanu zadania NALEZACY DO TEGO RDZENIA. Kazdy ma swoj:
   procesor czyta z niego stos jadra przy wejsciu z ring 3, a kazdy
   rdzen wchodzi tam z innego zadania. */
void gdt_load_tss(void);

/* Ustawia stos jądra, na który CPU przeskoczy przy przerwaniu z ring 3 */
void tss_set_esp0(uint32_t esp0);

#endif
