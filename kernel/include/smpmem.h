#ifndef PROTONOS_SMPMEM_H
#define PROTONOS_SMPMEM_H

/* Proba zamkow: wspolne struktury jadra pod obstrzalem wszystkich rdzeni
 *
 * Rdzenie wtorne umialy dotad tylko liczyc na tym, co dostaly - caly
 * dzielony miedzy nimi stan to byla jedna liczba i jeden zamek. Zeby
 * mogly kiedykolwiek prowadzic zadania, musza najpierw umiec bezpiecznie
 * wolac jadro; a jadro musi to wytrzymac.
 *
 * Ta proba bierze dwie struktury, po ktore siega wszystko inne - mape
 * ramek fizycznych i sterte jadra - i kaze kazdemu rdzeniowi przydzielac
 * z nich i oddawac tysiace razy naraz. Sprawdzanie nie polega na tym,
 * czy sie nie wysypalo: kazdy przydzial dostaje wzorzec zalezny od tego,
 * kto go wzial, i zaraz potem jest czytany. Ramka wydana dwom rdzeniom
 * naraz ma dwoch piszacych i wzorzec sie nie zgadza.
 */

/* Puszcza probe i wypisuje wynik. Wolane z shella obu jader. */
void smpmem_test(void);

#endif
