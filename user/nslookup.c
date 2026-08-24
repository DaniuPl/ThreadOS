/* nslookup - zapytanie DNS z ring 3 (UDP, port 53)
 *
 * Cala rozmowa dzieje sie w procesie uzytkownika: jadro daje tylko
 * gniazdo. Serwer DNS bierzemy z konfiguracji przyznanej przez DHCP.
 *
 * Uzycie: run nslookup example.com
 */

#include "usys.h"
#include "udns.h"

void _start(int argc, char **argv)
{
    if (argc < 2) {
        print("uzycie: nslookup <nazwa>\n");
        sys_exit(1);
    }

    struct netinfo ni;
    if (netinfo(&ni) < 0) {
        print("[nslookup] system nie ma jeszcze adresu IP\n");
        sys_exit(1);
    }
    if (!ni.dns) {
        print("[nslookup] DHCP nie podal serwera DNS\n");
        sys_exit(1);
    }

    print("[nslookup] pytam ");
    print_ip(ni.dns);
    print(" o ");
    print(argv[1]);
    print("\n");

    int rcode = 0;
    uint32_t ip = dns_lookup(argv[1], ni.dns, &rcode);

    if (ip) {
        print(argv[1]);
        print(" ma adres ");
        print_ip(ip);
        print("\n");
        sys_exit(0);
    }
    if (rcode == 3) {
        print("[nslookup] nazwa nie istnieje\n");
        sys_exit(2);
    }
    if (rcode) {
        print("[nslookup] serwer zwrocil blad ");
        print_dec((uint32_t)rcode);
        print("\n");
        sys_exit(2);
    }
    print("[nslookup] brak odpowiedzi (timeout 5 s)\n");
    sys_exit(1);
}
