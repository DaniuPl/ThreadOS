/* wild64 - program, ktory siega po pamiec jadra
 *
 * Dwa razy, bo sa na to dwie drogi. Najpierw prosi jadro, zeby
 * wypisalo mu kawalek pamieci spod adresu obrazu jadra - to jest
 * proba oszukania jadra jego wlasnymi rekami, i jadro musi odmowic,
 * bo sprawdza, czy wskaznik w ogole nalezy do programu.
 *
 * Potem siega tam sam. Tej proby nie zatrzymuje juz zadne
 * sprawdzenie w kodzie, tylko sam procesor: strona jadra nie ma bitu
 * dostepu dla pierscienia 3.
 */

#include "usys64.h"

#define KERNEL_ADDR 0x100000ULL          /* obraz jadra pod megabajtem */

void _start(void)
{
    print("[wild64] prosze jadro o wypisanie jego wlasnej pamieci\n");
    int64_t ret = syscall64(SYS64_WRITE, (int64_t)KERNEL_ADDR, 16);
    if (ret < 0)
        print("[wild64] jadro odmowilo - dobrze\n");
    else
        print("[wild64] BLAD: jadro wypisalo swoja pamiec!\n");

    print("[wild64] to siegne tam sam - to powinno mnie zabic\n");
    volatile unsigned char *p = (volatile unsigned char *)(uintptr_t)KERNEL_ADDR;
    unsigned char v = *p;
    (void)v;

    print("[wild64] BLAD: przeczytalem pamiec jadra!\n");
    sys_exit(1);
}
