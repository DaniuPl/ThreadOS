/* Wejscie do jadra dla kodu wspolnego programow
 *
 * Kryptografia, TLS, HTTP, DNS i parser HTML sa te same po obu
 * stronach - licza na bajtach i o trybie procesora nie wiedza nic.
 * Do jadra mowia jednak kilkoma nazwami, a te w trybie dlugim maja
 * inny ksztalt. Ten plik wybiera wlasciwa strone, zeby zaden
 * z tamtych naglowkow nie musial o tym wiedziec.
 */

#ifndef PROTONOS_UAPI_H
#define PROTONOS_UAPI_H

#ifdef __x86_64__
#include "../user64/usys64.h"
#include "../user64/uzgodnosc64.h"
#else
#include "usys.h"
#endif

#endif
