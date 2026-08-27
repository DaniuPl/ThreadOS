/* sigtest - sygnaly: wlasna procedura obslugi, ignorowanie, reakcje
 * domyslne i Ctrl+C
 *
 * Sygnal doreczany jest zawsze przy powrocie do ring 3. Gdy proces ma
 * wlasna procedure obslugi, jadro odklada na jego stos ramke
 * przerwanego stanu i skacze w handler; ten konczy sie zwyklym 'ret'
 * do trampoliny, ktora prosi jadro o odtworzenie tego stanu.
 *
 * Dlatego najostrzejszym sprawdzianem jest tu wynik przerwanego
 * syscalla: sygnal przychodzi dokladnie w chwili, gdy jadro odlozylo
 * juz w eax zero jako wynik kill(). Jesli ramka wroci uszkodzona,
 * sys_kill() odda smieci zamiast zera.
 */

#include "usys.h"

static volatile int32_t hits;
static volatile int32_t last_sig;
static volatile int32_t chld;

static void on_signal(int32_t sig)
{
    hits++;
    last_sig = sig;
}

static void on_chld(int32_t sig)
{
    (void)sig;
    chld++;
}

static void fail(const char *msg)
{
    print("[sigtest] ZLE: ");
    print(msg);
    print("\n");
    sys_exit(1);
}

void _start(void)
{
    /* 1. Wlasna procedura obslugi i powrot w to samo miejsce */
    if (sys_signal(SIGINT, on_signal) < 0)
        fail("nie da sie ustawic procedury obslugi");
    int32_t rc = sys_kill(sys_getpid(), SIGINT);
    if (rc != 0)
        fail("przerwany syscall nie oddal swojego wyniku");
    if (hits != 1 || last_sig != SIGINT)
        fail("procedura obslugi nie dostala SIGINT");
    print("[sigtest] wlasna procedura obslugi dostala SIGINT i wrocila\n");

    /* 2. Ignorowanie */
    sys_signal(SIGINT, (sighandler_t)SIG_IGN);
    sys_kill(sys_getpid(), SIGINT);
    sys_sleep(50);
    if (hits != 1)
        fail("zignorowany sygnal jednak wszedl w procedure obslugi");
    print("[sigtest] SIG_IGN: sygnal przepadl, tak jak mial\n");

    /* 3. Reakcja domyslna: dziecko ginie od SIGTERM */
    sys_signal(SIGCHLD, on_chld);
    int32_t kid = sys_fork();
    if (kid == 0) {
        sys_signal(SIGTERM, (sighandler_t)SIG_DFL);
        for (;;)
            sys_sleep(100);          /* czeka na swoj koniec */
    }
    sys_sleep(200);
    sys_kill(kid, SIGTERM);
    int32_t code = 0;
    if (sys_waitany(&code) != kid || code != -SIGTERM)
        fail("SIGTERM nie zakonczyl dziecka domyslna reakcja");
    print("[sigtest] SIGTERM domyslnie konczy proces (kod -15)\n");
    if (chld < 1)
        fail("rodzic nie dostal SIGCHLD po smierci dziecka");
    print("[sigtest] rodzic dostal SIGCHLD\n");

    /* 4. SIGKILL-a nie da sie przechwycic */
    int32_t tough = sys_fork();
    if (tough == 0) {
        sys_signal(SIGKILL, on_signal);   /* powinno odmowic */
        for (;;)
            sys_sleep(100);
    }
    sys_sleep(200);
    sys_kill(tough, SIGKILL);
    if (sys_waitany(&code) != tough || code != -9)
        fail("SIGKILL dal sie przechwycic albo nie zabil");
    print("[sigtest] SIGKILL nie do przechwycenia (kod -9)\n");

    /* 5. Ctrl+C z klawiatury: sterownik zamienia go na SIGINT dla
       procesu trzymajacego konsole, a czekanie na klawisz przerywa */
    hits = 0;
    sys_signal(SIGINT, on_signal);
    print("[sigtest] czekam na Ctrl+C\n");
    char c;
    while (hits == 0) {
        if (sys_read(STDIN, &c, 1) < 0)
            continue;                /* odczyt przerwany sygnalem */
    }
    if (last_sig != SIGINT)
        fail("Ctrl+C przyszedl jako cos innego niz SIGINT");
    print("[sigtest] Ctrl+C dotarl jako SIGINT\n");
    sys_exit(0);
}
