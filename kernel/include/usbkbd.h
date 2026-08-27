#ifndef PROTONOS_USBKBD_H
#define PROTONOS_USBKBD_H

/* Klawiatura USB (klasa HID, protokol rozruchowy)
 *
 * Rozpoznaje urzadzenie na magistrali USB, przelacza je w protokol
 * rozruchowy - ten, ktorego uzywa BIOS, o stalym osmiobajtowym
 * raporcie - i podaje wcisniete klawisze tam, gdzie trafiaja te
 * z PS/2. Dla reszty systemu nie ma zadnej roznicy, ktora klawiatura
 * jest podpieta.
 */

/* Szuka klawiatury na magistrali i uruchamia watek odpytujacy.
   1 = znaleziona. */
int usbkbd_init(void);

#endif
