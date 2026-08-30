/* fileio - proof that a ring 3 process handles files through
 * descriptors
 *
 * It opens a file for writing (creating it), writes through the fd and
 * closes it. Then it opens it for reading and reads through the fd to
 * the console. The file stays on the disk - visible through
 * 'cat ring3.txt' after a restart too.
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
