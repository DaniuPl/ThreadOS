/* shmread - czytelnik pamieci wspoldzielonej
 *
 * Mapuje ten sam region shm (klucz 42) co shmtest i wypisuje wartosc,
 * ktora zapisal tam inny proces. To samo fizyczne miejsce w pamieci.
 */

#include "usys.h"

void _start(void)
{
    volatile uint32_t *p = (volatile uint32_t *)sys_shm(42, 4096);
    if (!p) {
        print("[shmread] brak pamieci wspoldzielonej\n");
        sys_exit(1);
    }
    print("[shmread] wartosc=");
    print_dec(p[0]);
    print("\n");
    sys_exit(0);
}
