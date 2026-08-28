/* ucat64 - wypisuje plik podany w argumencie
 *
 * Shell jadra ma wbudowane 'cat', bo do niedawna nie bylo innego
 * wyjscia. Ten robi to samo, ale jest zwyklym programem: dostaje
 * nazwe pliku jako argument, otwiera go deskryptorem i czyta.
 * Jadro nie wie o nim nic ponad to, ze ma go uruchomic.
 */

#include "usys64.h"

static char bufor[256];

void _start(int argc, char **argv)
{
    if (argc < 2) {
        print("ucat64: podaj nazwe pliku\n");
        sys_exit(1);
    }

    int64_t fd = sys_open(argv[1], 0);
    if (fd < 0) {
        print("ucat64: nie moge otworzyc '");
        print(argv[1]);
        print("'\n");
        sys_exit(1);
    }

    /* Czytamy porcjami az do konca - deskryptor sam pamieta, gdzie
       stanal, wiec kolejny odczyt zaczyna tam, gdzie skonczyl poprzedni */
    int64_t n;
    while ((n = sys_read((int)fd, bufor, sizeof(bufor))) > 0)
        sys_write(FD_STDOUT, bufor, (uint64_t)n);

    sys_close((int)fd);
    sys_exit(0);
}
