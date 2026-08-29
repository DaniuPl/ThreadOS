/* gmon64 - wskaznik zasobow w oknie (ring 3, jadro 64-bitowe)
 *
 * Co pol sekundy pyta jadro o stan maszyny i rysuje pamiec oraz czas
 * pracy na zywo, z paskiem zajetosci. Nic tu nie jest tajemnica:
 * liczby opisuja maszyne, a nie cudze procesy - dlatego program
 * z pierscienia 3 moze je dostac, nie prosac o zadne uprawnienia.
 */

#include "usys64.h"

#define C_RED   1
#define C_GREEN 2
#define C_BLUE  3
#define C_WHITE 4
#define C_BG    8

#define OKNO_W 170
#define OKNO_H 80
#define PASEK_W 160

static void liczba(int w, int x, int y, uint64_t v, int kolor)
{
    char s[24];
    utoa(v, s);
    win_text(w, x, y, s, kolor);
}

void _start(void)
{
    int w = (int)win_create(OKNO_W, OKNO_H, "Monitor");
    if (w < 0) {
        print("gmon64: pulpit nie dal okna\n");
        sys_exit(1);
    }

    for (;;) {
        struct sysinfo64 si;
        if (sys_sysinfo(&si) < 0)
            sys_exit(1);

        win_rect(w, 0, 0, OKNO_W, OKNO_H, C_BG);
        win_text(w, 4, 4, "RAM uzyte (KiB):", C_GREEN);
        liczba(w, 136, 4, si.uzyte_kib, C_WHITE);
        win_text(w, 4, 16, "RAM razem (KiB):", C_GREEN);
        liczba(w, 136, 16, si.ram_kib, C_WHITE);
        win_text(w, 4, 28, "uptime (s):", C_GREEN);
        liczba(w, 96, 28, si.uptime_s, C_WHITE);

        /* Pasek zajetosci: czerwone tlo, niebieska czesc uzyta */
        uint32_t ile = si.ram_kib
                           ? si.uzyte_kib * PASEK_W / si.ram_kib : 0;
        win_rect(w, 4, 48, PASEK_W, 12, C_RED);
        win_rect(w, 4, 48, (int)ile, 12, C_BLUE);

        sys_sleep(500);
    }
}
