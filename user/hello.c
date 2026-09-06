/* hello - grzeczny program uzytkownika: syscalle i czyste wyjscie */

#include "usys.h"

static char counter_msg[] = "[hello] counting: 0\n";

void _start(void)
{
    print("[hello] hello, I am an ELF process from the initrd!\n");

    for (int i = 1; i <= 3; i++) {
        counter_msg[15] = (char)('0' + i);
        print(counter_msg);
        sys_sleep(250);
    }

    print("[hello] ending politely\n");
    sys_exit(0);
}
