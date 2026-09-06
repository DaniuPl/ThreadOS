/* nslookup - a DNS query from ring 3 (UDP, port 53)
 *
 * The whole conversation happens in a user process: the kernel only
 * provides a socket. The DNS server we take from the configuration
 * granted by DHCP.
 *
 * Usage: run nslookup example.com
 */

#include "uapi.h"
#include "udns.h"

void _start(int argc, char **argv)
{
    if (argc < 2) {
        print("usage: nslookup <name>\n");
        sys_exit(1);
    }

    struct netinfo ni;
    if (netinfo(&ni) < 0) {
        print("[nslookup] the system has no IP address yet\n");
        sys_exit(1);
    }
    if (!ni.dns) {
        print("[nslookup] DHCP gave no DNS server\n");
        sys_exit(1);
    }

    print("[nslookup] asking ");
    print_ip(ni.dns);
    print(" o ");
    print(argv[1]);
    print("\n");

    int rcode = 0;
    uint32_t ip = dns_lookup(argv[1], ni.dns, &rcode);

    if (ip) {
        print(argv[1]);
        print(" has the address ");
        print_ip(ip);
        print("\n");
        sys_exit(0);
    }
    if (rcode == 3) {
        print("[nslookup] there is no such name\n");
        sys_exit(2);
    }
    if (rcode) {
        print("[nslookup] the server answered with an error ");
        print_dec((uint32_t)rcode);
        print("\n");
        sys_exit(2);
    }
    print("[nslookup] no answer (5 s timeout)\n");
    sys_exit(1);
}
