#ifndef PROTONOS_PROG_H
#define PROTONOS_PROG_H

#include <stdint.h>

/* Skad wziac obraz programu
 *
 * Do tej pory odpowiedz byla jedna: z initrd. Initrd jest jednak
 * obrazem tylko do odczytu, wgrywanym razem z jadrem - system, ktory
 * ma dysk i system plikow, powinien umiec uruchomic takze to, co na
 * tym dysku lezy.
 *
 * Kolejnosc jest taka: najpierw initrd, potem dysk. Initrd jest
 * zawsze i zawsze dziala, wiec program o tej samej nazwie wgrany
 * razem z jadrem nie da sie przykryc plikiem z dysku - a to znaczy,
 * ze zepsuty plik na dysku nie odbierze systemu.
 */

/* Znajduje obraz programu. Zwraca wskaznik do niego i rozmiar, albo
   NULL. Gdy obraz przyszedl z dysku, w *bufor wraca adres do oddania
   przez prog_zwolnij - obraz z initrd niczego nie wymaga i zostawia
   tam zero. */
const void *prog_obraz(const char *nazwa, uint32_t *rozmiar, void **bufor);
void prog_zwolnij(void *bufor);

/* Czy program o tej nazwie w ogole gdzies jest. Sprawdzenie tanie:
   nie wczytuje pliku, tylko pyta o niego initrd i system plikow. */
int prog_istnieje(const char *nazwa);

/* Skad przyszedl ostatnio znaleziony obraz - do komunikatow */
const char *prog_skad(void);

#endif
