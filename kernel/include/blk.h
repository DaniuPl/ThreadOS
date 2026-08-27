#ifndef PROTONOS_BLK_H
#define PROTONOS_BLK_H

#include <stdint.h>

/* Warstwa urzadzenia blokowego
 *
 * System plikow i wymiana stron nie musza wiedziec, czy dysk siedzi za
 * kontrolerem AHCI, czy za starym ATA w trybie PIO. Wybor zapada raz,
 * przy starcie: AHCI ma pierwszenstwo (jest szybszy i to jedyne, co
 * widac na nowszych plytach), ATA zostaje jako droga zapasowa.
 */

/* Wykrywa dysk i wybiera sterownik. 1 = jest dysk. */
int blk_init(void);
int blk_present(void);

/* Nazwa wybranego sterownika (do komunikatu startowego) */
const char *blk_name(void);

/* Pojemnosc w sektorach 512 B (0 = nieznana) */
uint32_t blk_sectors(void);

/* Odczyt/zapis 'count' kolejnych sektorow. Bufor musi lezec w pamieci
   ciaglej fizycznie - przy AHCI kontroler siega po niego sam.
   0 = OK, -1 = blad. */
int blk_read(uint32_t lba, uint32_t count, void *buf);
int blk_write(uint32_t lba, uint32_t count, const void *buf);

#endif
