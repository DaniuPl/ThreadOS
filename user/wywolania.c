/* wywolania - ile wejsc do jadra da sie zrobic naraz
 *
 * Program rdzen mierzyl LICZENIE w pierscieniu 3: tam dwa procesy na
 * dwoch rdzeniach ida obok siebie, bo jadra nie dotykaja wcale. Ten
 * mierzy co innego - samo WCHODZENIE DO JADRA. Petla nie liczy prawie
 * niczego, tylko wola jadro tak czesto, jak sie da.
 *
 * Roznice robi wielki zamek jadra. Dopoki bral go kazdy syscall, dwa
 * procesy na dwoch rdzeniach czekaly na siebie po kolei i 'runw' nie
 * dawal nic - a nawet szkodzil, bo do czekania dochodzilo bicie sie
 * o te sama linie pamieci. Wywolanie, ktore siega tylko po stan
 * wolajacego zadania, zamka nie potrzebuje.
 *
 * Mierzymy sys_getpid, bo to najczystsze wejscie do jadra, jakie jest:
 * czyta jedno pole wlasnego zadania i wraca. Przy okazji jest wlasnym
 * sprawdzianem poprawnosci - kazde z miliona wywolan ma zwrocic numer
 * TEGO procesu. Gdyby dwa rdzenie mylily sie co do tego, ktore zadanie
 * prowadza, widac by to bylo tutaj i to od razu.
 *
 * Uzycie: run wywolania   - oba procesy na rdzeniu rozruchowym
 *         runw wywolania  - wolno im biec na kazdym rdzeniu
 */

#include "uapi.h"

#define WYWOLAN  1000000u

static uint32_t petla(uint32_t *maska_out, uint32_t *zgodnych_out)
{
    int32_t moj = sys_getpid();
    uint32_t maska = 0, zgodnych = 0;

    for (uint32_t i = 0; i < WYWOLAN; i++) {
        if (sys_getpid() == moj)
            zgodnych++;
        /* O rdzen pytamy rzadko: to wywolanie siega do lokalnego APIC,
           czyli do rejestru sprzetowego, a tego nie chcemy w petli
           pomiarowej */
        if ((i & 0xFFFFu) == 0)
            maska |= 1u << (uint32_t)sys_rdzen();
    }

    *maska_out = maska;
    *zgodnych_out = zgodnych;
    return (uint32_t)moj;
}

static void powiedz(const char *kto, uint32_t maska, uint32_t tyknac,
                    uint32_t zgodnych)
{
    print("[wywolania] ");
    print(kto);
    print(": rdzenie ");
    print_dec(maska);
    print(", tyknac ");
    print_dec(tyknac);
    print(", wywolan ");
    print_dec(WYWOLAN);
    print(", zgodnych ");
    print_dec(zgodnych);
    print("\n");
}

void _start(void)
{
    int32_t pid = sys_fork();
    if (pid < 0) {
        print("[wywolania] ZLE: fork sie nie udal\n");
        sys_exit(1);
    }

    uint32_t start = sys_getticks();
    uint32_t maska = 0, zgodnych = 0;
    petla(&maska, &zgodnych);
    uint32_t tyknac = sys_getticks() - start;

    if (pid == 0) {
        powiedz("dziecko", maska, tyknac, zgodnych);
        sys_exit(0);
    }

    int kod = 0;
    sys_wait_dziecka(&kod);
    powiedz("rodzic ", maska, tyknac, zgodnych);
    print("[wywolania] gotowe\n");
    sys_exit(0);
}
