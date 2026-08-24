/* evil2 - niegrzeczny inaczej: nie siega po pamiec jadra sam (od tego
 * jest 'evil' i page fault), tylko probuje zalatwic to cudzymi rekami.
 *
 * Podaje w syscallach wskazniki na pamiec jadra: gdyby jadro ufalo im
 * naiwnie, zapisywaloby i czytalo pod te adresy w ring 0, z pelnymi
 * prawami - ochrona stronicowania nic by nie dala. Kazda taka proba
 * musi skonczyc sie bledem -1, a proces ma to przezyc.
 */

#include "usys.h"

#define KERNEL_ADDR 0x100000u    /* poczatek obrazu jadra */
#define VGA_ADDR    0xB8000u     /* bufor trybu tekstowego VGA */

/* Surowy syscall - potrzebny, bo chcemy widziec eax nawet tam, gdzie
   biblioteczka usys.h zwraca void (sys_time, sys_getcwd). */
static inline int32_t raw(int32_t nr, uint32_t a, uint32_t b, uint32_t c)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "b"(a), "c"(b), "d"(c)
                     : "memory");
    return ret;
}

static int failures;

static void expect_refused(const char *what, int32_t ret)
{
    print("[evil2] ");
    print(what);
    if (ret == -1) {
        print(": odrzucone (-1) OK\n");
    } else {
        print(": PRZESZLO! zwrocono ");
        print_dec((uint32_t)ret);
        print("\n");
        failures++;
    }
}

void _start(void)
{
    print("[evil2] probuje oszukac jadro wskaznikami na jego wlasna pamiec\n");

    /* Wypis pamieci jadra na konsole cudzymi rekami */
    expect_refused("write(1, jadro, 32)",
                   raw(SYS_WRITE, 1, KERNEL_ADDR, 32));

    /* Nadpisanie pamieci jadra data z RTC */
    expect_refused("time(-> jadro)", raw(SYS_TIME, KERNEL_ADDR, 0, 0));

    /* Nadpisanie bufora VGA statystykami pamieci */
    expect_refused("sysinfo(-> VGA)", raw(SYS_SYSINFO, VGA_ADDR, 0, 0));

    /* Zapis sciezki katalogu pod adres zerowy */
    expect_refused("getcwd(NULL)", raw(SYS_GETCWD, 0, 0, 0));

    /* Sciezka pliku wskazujaca w pamiec jadra */
    expect_refused("open(jadro)", raw(SYS_OPEN, KERNEL_ADDR, 0, 0));

    /* Punkt wejscia watku w kodzie jadra */
    expect_refused("thread(-> jadro)", raw(SYS_THREAD, KERNEL_ADDR, 0, 0));

    /* Kontrola pozytywna: zwykly, uczciwy wskaznik ma dzialac dalej */
    struct utime t;
    if (raw(SYS_TIME, (uint32_t)&t, 0, 0) != 0) {
        print("[evil2] BLAD: uczciwy wskaznik tez zostal odrzucony!\n");
        failures++;
    }

    if (failures) {
        print("[evil2] DZIURA: jadro dalo sie oszukac\n");
        sys_exit(1);
    }
    print("[evil2] jadro odrzucilo wszystkie proby - granica trzyma\n");
    sys_exit(0);
}
