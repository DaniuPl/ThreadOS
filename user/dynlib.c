/* dynlib - dowod na to, ze biblioteka wspoldzielona naprawde jest wspolna
 *
 * Program nie niesie w sobie kryptografii; niesie tylko wywolania.
 * Sprawdzamy to trzema rzeczami, ktorych nie da sie sfalszowac:
 *
 *   1. Kod, ktory wykonujemy, lezy POZA obrazem programu. Adres musi
 *      podac sama biblioteka - program nieprzemieszczalny, ktory
 *      bierze adres jej funkcji, dostaje adres wlasnej zaslepki skoku.
 *   2. Wynik jest poprawny: SHA-256('abc') to znany wektor z FIPS
 *      180-4. Skok przez niewypelniona pozycje albo w zly kod nie ma
 *      jak dac wlasciwej odpowiedzi.
 *   3. Dane biblioteki sa PRYWATNE. Licznik generatora losowego zaczyna
 *      w kazdym procesie od zera; gdyby dane byly wspolne, drugi bieg
 *      zastalby licznik z pierwszego.
 *
 * Uzycie: run dynlib
 */

#include "uapi.h"
#include "usha256.h"
#include "urand.h"
#include "uproton.h"

static void report(const char *co, uint32_t v)
{
    print(co);
    print_dec(v);
    print("\n");
}

/* Punkt odniesienia: wlasna funkcja programu, zeby bylo z czym
   porownac adres kodu biblioteki */
static void our_fn(void)
{
    print("");
}

void _start(int argc, char **argv)
{
    our_fn();

    uint32_t code = (uint32_t)proton_code_addr();
    uint32_t data = (uint32_t)proton_data_addr();
    uint32_t mine = (uint32_t)(uintptr_t)our_fn;

    report("[dynlib] kod programu pod ", mine);
    report("[dynlib] kod biblioteki pod ", code);
    report("[dynlib] dane biblioteki pod ", data);

    if (code <= mine) {
        print("[dynlib] ZLE: kod biblioteki lezy w obrazie programu\n");
        sys_exit(1);
    }

    /* Znany wektor: SHA-256('abc') zaczyna sie od ba 78 16 bf */
    static const uint8_t want[8] = { 0xba, 0x78, 0x16, 0xbf,
                                     0x8f, 0x01, 0xcf, 0xea };
    uint8_t digest[32];
    sha256("abc", 3, digest);
    for (int i = 0; i < 8; i++) {
        if (digest[i] != want[i]) {
            print("[dynlib] ZLE: SHA-256 z biblioteki daje nie to, co trzeba\n");
            sys_exit(1);
        }
    }
    print("[dynlib] SHA-256('abc') z biblioteki zgadza sie z FIPS 180-4\n");

    /* Kod biblioteki jest WSPOLNY - te same ramki widzi kazdy proces.
       Gdyby dalo sie w nich pisac, jeden program podmienilby funkcje
       wszystkim naraz. Ma sie wiec nie dac, i to ma go zabic. */
    if (argc > 1 && argv[1][0] == 'p') {
        print("[dynlib] probuje pisac po kodzie biblioteki\n");
        *(volatile uint8_t *)(uintptr_t)code = 0x90;
        print("[dynlib] ZLE: zapis do kodu biblioteki sie udal\n");
        sys_exit(1);
    }

    /* Licznik generatora to stan biblioteki. W swiezym procesie ma byc
       zerowy - bez tego nie wiadomo by bylo, czy dane sa prywatne. */
    if (proton_random_counter() != 0) {
        print("[dynlib] ZLE: licznik biblioteki nie zaczyna od zera\n");
        sys_exit(1);
    }
    uint8_t random[64];
    urand_init();
    urand_bytes(random, sizeof(random));
    report("[dynlib] licznik po pobraniu bajtow: ",
            proton_random_counter());

    print("[dynlib] gotowe\n");
    sys_exit(0);
}
