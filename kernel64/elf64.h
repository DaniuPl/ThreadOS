#ifndef PROTONOS_ELF64_H
#define PROTONOS_ELF64_H

#include <stdint.h>

/* Ladowanie programow ELF64 do przestrzeni uzytkownika
 *
 * Jadro 32-bitowe robi to samo w exec.c, ale tam kazdy proces dostaje
 * wlasna przestrzen adresowa. Tutaj przestrzen jest na razie jedna
 * i wspolna, wiec ladowanie sprowadza sie do trzech rzeczy:
 * sprawdzic naglowek, przydzielic ramki pod segmenty i zmapowac je
 * z prawami, ktore ELF sam podaje.
 *
 * Prawa biore sie wprost z pliku: segment bez bitu "wykonywalny"
 * dostaje zakaz wykonywania (bit NX), segment bez bitu "zapisywalny"
 * zostaje tylko do odczytu. Kod programu nie jest wiec zapisywalny,
 * a jego dane nie sa wykonywalne - i pilnuje tego procesor.
 */

/* Przestrzen uzytkownika: od 1 GiB w gore. Stos siedzi na jej szczycie
   i rosnie w dol, wiec segmenty programu musza sie skonczyc pod nim -
   inaczej pierwsze glebsze wywolanie zadeptaloby wlasny kod.

   Szczyt stoi w polowie gigabajta przydzielonego programowi (wpis
   PAGING64_USER_PDPT), a nie tuz nad obrazem: pod nim zostaje wtedy
   pol gigabajta na sterte. Osiem megabajtow, ktore bylo tu wczesniej,
   wystarczalo, dopoki nie bylo wymiany stron - a wymiana ma sens
   dopiero wtedy, gdy program moze poprosic o wiecej pamieci, niz
   maszyna ma RAM-u. */
#define USER64_BASE        0x0000000040000000ULL
#define USER64_STACK_TOP   0x0000000060000000ULL
#define USER64_STACK_PAGES 4

/* Miedzy sterta a stosem lezy obszar na mapowania: pamiec wspoldzielona
   i wszystko, co po niej przyjdzie. Sterta konczy sie u jego progu,
   wiec jedno nie ma jak wejsc w drugie. */
#define USER64_MMAP_BASE   0x0000000050000000ULL
#define USER64_MMAP_MAX    (USER64_STACK_TOP - USER64_STACK_PAGES * 4096ULL)
#define USER64_TOP         USER64_MMAP_BASE

/* Laduje obraz ELF64. Zwraca 0 i ustawia *entry oraz poczatek sterty
   (pierwsza wolna strona za obrazem), albo -1. */
int elf64_load(const void *image, uint32_t size, uint64_t *entry,
               uint64_t *brk_base);

/* Przydziela i mapuje stos programu. Zwraca jego wierzcholek albo 0. */
uint64_t elf64_make_stack(void);

#endif
