#include "uapi.h"

int main(void)
{
    struct netinfo info;
    print("Network configuration:\n");
    if (netinfo(&info) < 0) {
        print("No configured IPv4 connection.\n");
        return 0;
    }
    print("inet ");
    print_ip(info.ip);
    print("\nnetmask ");
    print_ip(info.mask);
    print("\ngateway ");
    print_ip(info.gw);
    print("\nDNS ");
    print_ip(info.dns);
    print("\n");
    return 0;
}
