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
 * Uzycie: run syscalls   - oba procesy na rdzeniu rozruchowym
 *         runw syscalls  - wolno im biec na kazdym rdzeniu
 */

#include "uapi.h"

#define CALLS  1000000u

static uint32_t loop(uint32_t *mask_out, uint32_t *matching_out)
{
    int32_t mine = sys_getpid();
    uint32_t mask = 0, matching = 0;

    for (uint32_t i = 0; i < CALLS; i++) {
        if (sys_getpid() == mine)
            matching++;
        /* O rdzen pytamy rzadko: to wywolanie siega do lokalnego APIC,
           czyli do rejestru sprzetowego, a tego nie chcemy w petli
           pomiarowej */
        if ((i & 0xFFFFu) == 0)
            mask |= 1u << (uint32_t)sys_core();
    }

    *mask_out = mask;
    *matching_out = matching;
    return (uint32_t)mine;
}

static void report(const char *who, uint32_t mask, uint32_t ticks,
                    uint32_t matching)
{
    print("[syscalls] ");
    print(who);
    print(": rdzenie ");
    print_dec(mask);
    print(", tyknac ");
    print_dec(ticks);
    print(", wywolan ");
    print_dec(CALLS);
    print(", zgodnych ");
    print_dec(matching);
    print("\n");
}

void _start(void)
{
    int32_t pid = sys_fork();
    if (pid < 0) {
        print("[syscalls] ZLE: fork sie nie udal\n");
        sys_exit(1);
    }

    uint32_t start = sys_getticks();
    uint32_t mask = 0, matching = 0;
    loop(&mask, &matching);
    uint32_t ticks = sys_getticks() - start;

    if (pid == 0) {
        report("dziecko", mask, ticks, matching);
        sys_exit(0);
    }

    int code = 0;
    sys_wait_child(&code);
    report("rodzic ", mask, ticks, matching);
    print("[syscalls] gotowe\n");
    sys_exit(0);
}
