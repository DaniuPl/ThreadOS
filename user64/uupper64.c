/* uupper64 - przepisuje wejscie na wyjscie wielkimi literami
 *
 * Nie zna zadnego pliku ani zadnego innego programu: czyta z numeru
 * zero i pisze na numer jeden. To, ze po drugiej stronie siedzi
 * drugi program, a nie klawiatura i ekran, jest sprawa tego, kto go
 * uruchomil - i program nie ma jak tego zauwazyc.
 *
 * Konczy sie, gdy odczyt odda zero, czyli gdy zniknie ostatni piszacy.
 */

#include "usys64.h"

static char buf[256];

void _start(void)
{
    int64_t n;

    while ((n = sys_read(FD_STDIN, buf, sizeof(buf))) > 0) {
        for (int64_t i = 0; i < n; i++)
            if (buf[i] >= 'a' && buf[i] <= 'z')
                buf[i] = (char)(buf[i] - 'a' + 'A');
        sys_write(FD_STDOUT, buf, (uint64_t)n);
    }
    sys_exit(0);
}
