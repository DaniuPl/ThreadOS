#ifndef PROTONOS_KBDMAP_H
#define PROTONOS_KBDMAP_H

#include <stdint.h>
#include "keyboard.h"

/* Tlumaczenie scancode'ow PS/2 (zestaw 1) na znaki
 *
 * Sam automat: bierze bajt po bajcie i mowi, co z niego wyszlo. Nie wie
 * nic ani o konsolach, ani o zadaniach, ani o pulpicie - i dlatego
 * uzywaja go oba jadra, 32- i 64-bitowe. Decyzja, DOKAD trafi znak,
 * nalezy do wolajacego.
 *
 * Stan (wcisniety Shift, Ctrl, Alt, prefiks klawisza rozszerzonego)
 * siedzi w srodku, bo rozklada sie na kilka kolejnych bajtow.
 */

#define KBD_NOTHING  0      /* prefiks, modyfikator albo puszczenie klawisza */
#define KBD_CHAR     1      /* znak ASCII albo kod sterujacy KEY_* */
#define KBD_FKEY     2      /* Alt + klawisz funkcyjny (numer w *fkey) */

/* Podaje jeden bajt ze sterownika. Zwraca ktoras ze stalych wyzej. */
int kbdmap_feed(uint8_t scancode, char *out_char, int *fkey);

#endif
