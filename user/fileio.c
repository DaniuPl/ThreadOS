/* fileio - dowod, ze proces ring 3 moze pisac i czytac pliki na dysku
 *
 * Zapisuje plik syscallem fwrite, czyta go z powrotem syscallem fread
 * i wypisuje zawartosc. Plik zostaje na dysku - kolejne uruchomienia
 * (takze po restarcie) zobacza go przez 'cat ring3.txt' albo 'fls'.
 */

#include "usys.h"

static char content[] = "plik zapisany przez proces ring3\n";

void _start(void)
{
    print("[fileio] zapisuje 'ring3.txt'...\n");
    if (sys_fwrite("ring3.txt", content, ustrlen(content)) < 0) {
        print("[fileio] blad zapisu (brak dysku?)\n");
        sys_exit(1);
    }

    char buf[128];
    int n = sys_fread("ring3.txt", buf, sizeof(buf) - 1);
    if (n < 0) {
        print("[fileio] blad odczytu\n");
        sys_exit(1);
    }
    buf[n] = '\0';

    print("[fileio] odczytano z dysku: ");
    print(buf);
    print("[fileio] OK\n");
    sys_exit(0);
}
