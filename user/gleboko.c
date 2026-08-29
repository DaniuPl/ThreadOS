/* gleboko - stos rosnacy na zadanie
 *
 * Program dostaje na start osiem stron stosu. Ta rekurencja siega
 * grubo glebiej: kazde wywolanie zajmuje cztery kilobajty ramki
 * i zapisuje w niej wzorzec, a po powrocie sprawdza, czy wzorzec
 * przetrwal. Jesli jadro dokladalo strony pod palcami, wszystko sie
 * zgadza; jesli nie, program ginie po pierwszym przekroczeniu granicy.
 *
 * Sprawdzanie wzorca PO powrocie jest tu istotne: dokladanie stron
 * pod nadpisywanie cudzych ramek wygladaloby w polowie tak samo.
 */

#include "usys.h"

#define RAMKA   4096
#define GLEBOKO 40

static uint32_t najglebiej;

static uint32_t schodz(uint32_t poziom)
{
    volatile unsigned char ramka[RAMKA];
    for (uint32_t i = 0; i < RAMKA; i += 512)
        ramka[i] = (unsigned char)(poziom + i);

    najglebiej = poziom;
    uint32_t nizej = poziom < GLEBOKO ? schodz(poziom + 1) : 0;

    /* Po powrocie: czy ramka jest wciaz nasza i nietknieta? */
    for (uint32_t i = 0; i < RAMKA; i += 512)
        if (ramka[i] != (unsigned char)(poziom + i))
            return nizej + 1000000;
    return nizej;
}

void _start(void)
{
    uint32_t zle = schodz(1);

    print("[gleboko] zeszlem na poziom ");
    print_dec(najglebiej);
    print(", ramek po ");
    print_dec(RAMKA);
    print(" B\n");

    if (zle) {
        print("[gleboko] ZLE: ramki nie przetrwaly\n");
        sys_exit(1);
    }
    print("[gleboko] stos urosl pod palcami, wszystkie ramki caly\n");
    sys_exit(0);
}
