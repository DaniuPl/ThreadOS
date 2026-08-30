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
 * Uzycie: run cores   - oba procesy na rdzeniu rozruchowym
 *         runw cores  - wolno im biec na kazdym rdzeniu
 */

#include "uapi.h"

#define STEPS  60000000u

static uint32_t compute(uint32_t *mask_out)
{
    uint32_t stan = 12345u, sum = 0, mask = 0;
    for (uint32_t i = 0; i < STEPS; i++) {
        stan = stan * 1103515245u + 12345u;
        sum += stan >> 16;
        /* Pytanie o rdzen to wywolanie systemowe, a te chodza pod
           wielkim zamkiem jadra - pytamy wiec rzadko, zeby mierzyc
           liczenie, a nie czekanie na zamek */
        if ((i & 0x1FFFFF) == 0)
            mask |= 1u << (uint32_t)sys_core();
    }
    *mask_out = mask;
    return sum;
}

static void report(const char *who, uint32_t mask, uint32_t ticks,
                    uint32_t sum)
{
    print("[cores] ");
    print(who);
    print(": rdzenie ");
    print_dec(mask);
    print(", tyknac ");
    print_dec(ticks);
    print(", suma ");
    print_dec(sum);
    print("\n");
}

void _start(void)
{
    int32_t pid = sys_fork();
    if (pid < 0) {
        print("[cores] ZLE: fork sie nie udal\n");
        sys_exit(1);
    }

    uint32_t start = sys_getticks();
    uint32_t mask = 0;
    uint32_t sum = compute(&mask);
    uint32_t ticks = sys_getticks() - start;

    if (pid == 0) {
        report("dziecko", mask, ticks, sum);
        sys_exit(0);
    }

    int code = 0;
    sys_wait_child(&code);
    report("rodzic ", mask, ticks, sum);
    print("[cores] gotowe\n");
    sys_exit(0);
}
