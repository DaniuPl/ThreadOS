/* date - wypisuje date i godzine z RTC w formacie YYYY-MM-DD HH:MM:SS */

#include "usys.h"

/* Dwucyfrowa liczba z wiodacym zerem */
static void pad2(uint8_t v)
{
    char b[2] = { (char)('0' + v / 10), (char)('0' + v % 10) };
    sys_write(STDOUT, b, 2);
}

void _start(void)
{
    struct utime t;
    sys_time(&t);

    print_dec(t.year);
    print("-");
    pad2(t.month);
    print("-");
    pad2(t.day);
    print(" ");
    pad2(t.hour);
    print(":");
    pad2(t.min);
    print(":");
    pad2(t.sec);
    print("\n");
    sys_exit(0);
}
