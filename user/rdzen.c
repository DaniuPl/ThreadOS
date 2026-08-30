/* rdzen - na ktorym rdzeniu procesora naprawde biegne
 *
 * Program rozwidla sie i liczy w dwoch procesach naraz. Kazdy z nich
 * sam sprawdza, gdzie go prowadza - numeru rdzenia nie ma jak
 * przeczytac z pierscienia 3, bo lokalny APIC lezy poza przestrzenia
 * adresowa programu, wiec pyta o niego jadro.
 *
 * Rozwidlenie nie jest ozdoba. Zadanie, ktore biegnie samo, zostaje
 * tam, gdzie ruszylo: scheduler nie ma powodu go przenosic, skoro nikt
 * inny nie czeka. Dopiero DWA liczace naraz musza trafic na dwa
 * rdzenie - i wtedy oba konczy sie w czasie jednego, a nie dwoch.
 *
 * Kazdy proces wypisuje trzy rzeczy i kazda jest po cos: maske rdzeni,
 * ktore go prowadzily, liczbe tykniec (czyli czy naprawde szlo
 * rownolegle) i sume kontrolna (czyli czy policzyl to, co mial).
 *
 * Uzycie: run rdzen   - oba procesy na rdzeniu rozruchowym
 *         runw rdzen  - wolno im biec na kazdym rdzeniu
 */

#include "uapi.h"

#define KROKOW  60000000u

static uint32_t policz(uint32_t *maska_out)
{
    uint32_t stan = 12345u, suma = 0, maska = 0;
    for (uint32_t i = 0; i < KROKOW; i++) {
        stan = stan * 1103515245u + 12345u;
        suma += stan >> 16;
        /* Pytanie o rdzen to wywolanie systemowe, a te chodza pod
           wielkim zamkiem jadra - pytamy wiec rzadko, zeby mierzyc
           liczenie, a nie czekanie na zamek */
        if ((i & 0x1FFFFF) == 0)
            maska |= 1u << (uint32_t)sys_rdzen();
    }
    *maska_out = maska;
    return suma;
}

static void powiedz(const char *kto, uint32_t maska, uint32_t tyknac,
                    uint32_t suma)
{
    print("[rdzen] ");
    print(kto);
    print(": rdzenie ");
    print_dec(maska);
    print(", tyknac ");
    print_dec(tyknac);
    print(", suma ");
    print_dec(suma);
    print("\n");
}

void _start(void)
{
    int32_t pid = sys_fork();
    if (pid < 0) {
        print("[rdzen] ZLE: fork sie nie udal\n");
        sys_exit(1);
    }

    uint32_t start = sys_getticks();
    uint32_t maska = 0;
    uint32_t suma = policz(&maska);
    uint32_t tyknac = sys_getticks() - start;

    if (pid == 0) {
        powiedz("dziecko", maska, tyknac, suma);
        sys_exit(0);
    }

    int kod = 0;
    sys_wait_dziecka(&kod);
    powiedz("rodzic ", maska, tyknac, suma);
    print("[rdzen] gotowe\n");
    sys_exit(0);
}
