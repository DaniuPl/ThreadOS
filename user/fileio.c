/* fileio - dowod, ze proces ring 3 obsluguje pliki przez deskryptory
 *
 * Otwiera plik do zapisu (tworzac go), pisze przez fd, zamyka. Potem
 * otwiera do odczytu i czyta przez fd na konsole. Plik zostaje na
 * dysku - widoczny przez 'cat ring3.txt' takze po restarcie.
 */

#include "usys.h"

static char content[] = "plik zapisany przez proces ring3\n";

void _start(void)
{
    print("[fileio] otwieram 'ring3.txt' do zapisu...\n");
    int fd = sys_open("ring3.txt", O_WRITE | O_CREATE | O_TRUNC);
    if (fd < 0) {
        print("[fileio] blad open do zapisu (brak dysku?)\n");
        sys_exit(1);
    }
    sys_write(fd, content, ustrlen(content));
    sys_close(fd);

    fd = sys_open("ring3.txt", O_READ);
    if (fd < 0) {
        print("[fileio] blad open do odczytu\n");
        sys_exit(1);
    }
    char buf[128];
    int n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n < 0) {
        print("[fileio] blad read\n");
        sys_exit(1);
    }
    buf[n] = '\0';

    print("[fileio] odczytano z dysku: ");
    print(buf);
    print("[fileio] OK\n");
    sys_exit(0);
}
