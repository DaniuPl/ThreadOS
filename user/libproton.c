/* libproton - biblioteka wspoldzielona ProtonOS
 *
 * Nie ma tu ani jednej linijki wlasnego kodu i tak ma byc. Caly
 * material lezy tam, gdzie lezal od zawsze - w naglowkach z katalogu user -
 * a ten plik jest tylko miejscem, w ktorym raz sie go kompiluje.
 *
 * Naglowki wciagane sa w wersji 'lib' (scripts/rozdziel.py): te same
 * funkcje, ale z wiazaniem zewnetrznym, zeby dalo sie je w gotowej
 * bibliotece odnalezc po nazwie. Program dostaje te same naglowki
 * w wersji 'dekl' - same zapowiedzi, bez cial.
 *
 * Czego tu NIE MA i dlaczego:
 *
 *   usys.h - wywolania systemowe. To dwie instrukcje na sztuke,
 *   wpisane wprost w kod; przepuszczanie ich przez tablice skokow
 *   biblioteki kosztowaloby wiecej niz oszczedzalo.
 *
 *   certdata.h - dane testowe jednego programu, nie kod uzywany
 *   przez kilka.
 */

#include "uapi.h"
#include "ubignum.h"
#include "usha256.h"
#include "usha512.h"
#include "uchacha.h"
#include "urand.h"
#include "ux25519.h"
#include "ursa.h"
#include "uecdsa.h"
#include "ux509.h"
#include "ucert.h"
#include "utls.h"
#include "uhttp.h"
#include "uhtml.h"
#include "udns.h"
#include "uproton.h"
