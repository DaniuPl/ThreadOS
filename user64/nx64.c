/* nx64 - program, ktory probuje wykonac wlasne dane
 *
 * Bit zakazu wykonywania jest ustawiany od dawna, ale samo ustawienie
 * bitu niczego nie dowodzi. Dowodem jest dopiero to, ze procesor
 * naprawde na nim reaguje - a jedyny sposob, zeby to sprawdzic, to
 * sprobowac.
 *
 * Program wpisuje do swojej tablicy jedna instrukcje powrotu i skacze
 * pod jej adres. Tablica lezy w sekcji .bss, ktorej jadro nadalo
 * prawo zapisu i zakaz wykonywania - wiec zapis przechodzi, a skok
 * konczy sie wyjatkiem i smiercia programu.
 */

#include "usys64.h"

static volatile unsigned char data[16];

void _start(void)
{
    print("[nx64] wpisuje instrukcje do wlasnych danych\n");
    data[0] = 0xC3;                  /* ret */

    print("[nx64] i probuje ja wykonac - to powinno mnie zabic\n");
    void (*fn)(void);
    *(void **)&fn = (void *)(unsigned char *)data;
    fn();

    print("[nx64] BLAD: procesor wykonal dane!\n");
    sys_exit(1);
}
