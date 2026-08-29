/* shmpisz64 - pisarz pamieci wspoldzielonej
 *
 * Mapuje region o kluczu 42, zapisuje w nim znana wartosc i wlasny
 * numer procesu, po czym uruchamia 'shmczytaj64' - osobny proces
 * z wlasna przestrzenia adresowa. Ten mapuje ten sam klucz i czyta to,
 * co tu zostalo zapisane.
 *
 * Dowod jest w tym, ze procesy sa dwa: kazdy widzi region pod swoim
 * adresem, a bajty sa te same, bo ramki fizyczne sa te same.
 */

#include "usys64.h"

#define KLUCZ  42
#define WZORZEC 12345

static char out[96];

static void powiedz(const char *a, uint64_t v)
{
    int n = 0;
    while (*a)
        out[n++] = *a++;
    char tmp[24];
    utoa(v, tmp);
    for (int i = 0; tmp[i]; i++)
        out[n++] = tmp[i];
    out[n++] = '\n';
    out[n] = '\0';
    print(out);
}

void _start(void)
{
    /* Najpierw region na boku, o innym kluczu. Nie jest do niczego
       potrzebny poza jednym: przesuwa nasz obszar mapowan, wiec
       WSPOLNY region dostaniemy pod innym adresem niz czytelnik.
       O tym, gdzie cos widac, decyduje przestrzen adresowa, a nie
       pamiec - i dopiero rozne adresy pokazuja, ze to naprawde
       wspoldzielenie, a nie przypadkowa zgodnosc ukladu. */
    sys_shm(7, 4096);

    volatile uint32_t *p = sys_shm(KLUCZ, 4096);
    if (!p) {
        print("[shmpisz64] brak pamieci wspoldzielonej\n");
        sys_exit(1);
    }

    p[0] = WZORZEC;
    p[2] = 0;                        /* miejsce na odpowiedz czytelnika */
    powiedz("[shmpisz64] region pod adresem ", (uint64_t)(uintptr_t)p);
    powiedz("[shmpisz64] zapisalem ", WZORZEC);

    /* Dziecko z wlasna przestrzenia adresowa - fork i exec, tak jak
       robi to shell */
    int64_t pid = sys_fork();
    if (pid == 0) {
        char *argv[2] = {"shmczytaj64", 0};
        sys_exec("shmczytaj64", argv);
        print("[shmpisz64] nie ma 'shmczytaj64'\n");
        sys_exit(1);
    }
    if (pid > 0) {
        int kod = 0;
        sys_wait(&kod);
    }

    /* Czytelnik odbil wartosc przez to samo miejsce - jesli doszla,
       widac ja teraz tutaj */
    if (p[2] != WZORZEC + 1) {
        powiedz("[shmpisz64] ZLE: czytelnik nie odpisal, jest ", p[2]);
        sys_exit(1);
    }
    print("[shmpisz64] czytelnik odpisal przez te sama pamiec\n");
    sys_exit(0);
}
