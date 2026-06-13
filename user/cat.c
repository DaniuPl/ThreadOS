/* cat - wypisuje plik z dysku na konsole (czysty model fd: read pliku
 * -> write na fd 1). Uruchamiane z ush: 'cat <plik>'.
 */

#include "usys.h"

void _start(int argc, char **argv)
{
    if (argc < 2) {
        print("uzycie: cat <plik>\n");
        sys_exit(1);
    }

    int fd = sys_open(argv[1], O_READ);
    if (fd < 0) {
        print("cat: nie moge otworzyc '");
        print(argv[1]);
        print("'\n");
        sys_exit(1);
    }

    char buf[256];
    int n;
    while ((n = sys_read(fd, buf, sizeof(buf))) > 0)
        sys_write(STDOUT, buf, (uint32_t)n);

    sys_close(fd);
    sys_exit(0);
}
