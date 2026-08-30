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

#define WINDOW_W 170
#define WINDOW_H 80
#define BAR_W 160

static void num(int w, int x, int y, uint64_t v, int color)
{
    char s[24];
    utoa(v, s);
    win_text(w, x, y, s, color);
}

void _start(void)
{
    int w = (int)win_create(WINDOW_W, WINDOW_H, "Monitor");
    if (w < 0) {
        print("gmon64: pulpit nie dal okna\n");
        sys_exit(1);
    }

    for (;;) {
        struct sysinfo64 si;
        if (sys_sysinfo(&si) < 0)
            sys_exit(1);

        win_rect(w, 0, 0, WINDOW_W, WINDOW_H, C_BG);
        win_text(w, 4, 4, "RAM uzyte (KiB):", C_GREEN);
        num(w, 136, 4, si.used_kib, C_WHITE);
        win_text(w, 4, 16, "RAM razem (KiB):", C_GREEN);
        num(w, 136, 16, si.ram_kib, C_WHITE);
        win_text(w, 4, 28, "uptime (s):", C_GREEN);
        num(w, 96, 28, si.uptime_s, C_WHITE);

        /* Pasek zajetosci: czerwone tlo, niebieska czesc uzyta */
        uint32_t ile = si.ram_kib
                           ? si.used_kib * BAR_W / si.ram_kib : 0;
        win_rect(w, 4, 48, BAR_W, 12, C_RED);
        win_rect(w, 4, 48, (int)ile, 12, C_BLUE);

        sys_sleep(500);
    }
}
