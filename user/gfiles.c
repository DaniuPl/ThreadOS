/* gfiles - graficzny menedzer plikow (ring 3)
 *
 * Listuje pliki z dysku (PFS) w swoim oknie: co chwile odpytuje jadro
 * syscallem readdir i rysuje nazwy. Spina system plikow z GUI -
 * uruchamiany z menu pulpitu.
 */

#include "usys.h"

#define C_BG    8
#define C_WHITE 4
#define C_GREEN 2

void _start(void)
{
    int w = win_create(180, 120, "Pliki");
    if (w < 0)
        sys_exit(1);

    for (;;) {
        win_rect(w, 0, 0, 180, 120, C_BG);
        win_text(w, 4, 2, "Pliki na dysku:", C_GREEN);

        int n = 0;
        char name[FS_NAME_MAX + 1];
        for (int i = 0; i < 32; i++) {
            int32_t r = sys_readdir(i, name);
            if (r == -1)
                break;                  /* koniec wpisow */
            win_text(w, 8, 16 + n * 10, name, C_WHITE);
            n++;
        }
        if (n == 0)
            win_text(w, 8, 16, "(pusto)", C_WHITE);

        sys_sleep(300);
    }
}
