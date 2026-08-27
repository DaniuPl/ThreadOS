#ifndef PROTONOS_AHCI_H
#define PROTONOS_AHCI_H

#include <stdint.h>

/* Sterownik AHCI (SATA)
 *
 * Nastepca ATA PIO. Roznica jest zasadnicza: przy PIO to procesor
 * przepisuje kazde slowo przez port I/O, przy AHCI kontroler sam
 * siega do pamieci (DMA) po tablicy opisow, ktora mu podajemy.
 * Nowoczesne plyty nie maja juz trybu zgodnosci - bez tego sterownika
 * dysk w takiej maszynie jest po prostu niewidoczny.
 */

/* Szuka kontrolera AHCI po PCI i przygotowuje pierwszy port z dyskiem.
   1 = jest dysk gotowy do pracy. */
int ahci_init(void);

/* Pojemnosc dysku w sektorach 512 B (0 = nieznana) */
uint32_t ahci_sectors(void);

/* Odczyt/zapis 'count' kolejnych sektorow. Bufor musi lezec w pamieci
   ciaglej fizycznie - kontroler czyta go sam, bez udzialu stronicowania.
   0 = OK, -1 = blad. */
int ahci_read(uint32_t lba, uint32_t count, void *buf);
int ahci_write(uint32_t lba, uint32_t count, const void *buf);

#endif
