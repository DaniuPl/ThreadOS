/* sigtest64 - program, ktory przechwytuje Ctrl+C
 *
 * Ustawia wlasna procedure obslugi i czeka. Po sygnale procedura
 * wypisuje, ze go dostala, i wraca - a program ma dzialac dalej
 * dokladnie tam, gdzie mu przerwano. To drugie jest wazniejsze od
 * pierwszego: procedura obslugi nie konczy programu, tylko wchodzi
 * mu w pol kroku i wychodzi.
 */

#include "usys64.h"

static volatile int dostalem;
static volatile uint64_t obroty;

static void na_sygnal(int sig)
{
    (void)sig;
    print("[sigtest64] procedura obslugi: dostalem sygnal\n");
    dostalem = 1;
}

void _start(long id)
{
    (void)id;

    sys_signal(SIGINT, na_sygnal);
    print("[sigtest64] czekam na Ctrl+C\n");

    /* Petla bez konca: gdyby sygnal nie dzialal, program zostalby
       tu na zawsze. Licznik pokazuje, ze petla naprawde biegla. */
    while (!dostalem) {
        obroty++;
        sys_sleep(10);
    }

    print("[sigtest64] wrocilem do przerwanego miejsca i koncze\n");
    sys_exit(0);
}
