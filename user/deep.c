/* deep - stos rosnacy na zadanie
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

#define FRAME   4096
#define DEEP 40

static uint32_t deepest;

static uint32_t descend(uint32_t level)
{
    volatile unsigned char frame[FRAME];
    for (uint32_t i = 0; i < FRAME; i += 512)
        frame[i] = (unsigned char)(level + i);

    deepest = level;
    uint32_t below = level < DEEP ? descend(level + 1) : 0;

    /* Po powrocie: czy ramka jest wciaz nasza i nietknieta? */
    for (uint32_t i = 0; i < FRAME; i += 512)
        if (frame[i] != (unsigned char)(level + i))
            return below + 1000000;
    return below;
}

void _start(void)
{
    uint32_t bad = descend(1);

    print("[deep] zeszlem na poziom ");
    print_dec(deepest);
    print(", ramek po ");
    print_dec(FRAME);
    print(" B\n");

    if (bad) {
        print("[deep] ZLE: ramki nie przetrwaly\n");
        sys_exit(1);
    }
    print("[deep] stos urosl pod palcami, wszystkie ramki caly\n");
    sys_exit(0);
}
