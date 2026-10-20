#include "uapi.h"
#include "ulocale.h"

void _start(void)
{
    struct netinfo info;
    int polish = ulocale_language_polish();
    print(polish ? "Konfiguracja sieci:\n" : "Network configuration:\n");
    if (netinfo(&info) < 0) {
        print(polish ? "Brak skonfigurowanego połączenia IPv4.\n"
                     : "No configured IPv4 connection.\n");
        sys_exit(0);
    }
    print(polish ? "adres IP " : "inet ");
    print_ip(info.ip);
    print(polish ? "\nmaska " : "\nnetmask ");
    print_ip(info.mask);
    print(polish ? "\nbrama " : "\ngateway ");
    print_ip(info.gw);
    print("\nDNS ");
    print_ip(info.dns);
    print("\n");
    sys_exit(0);
}
