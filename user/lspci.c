#include "pciinfo.h"
#include "ulocale.h"

static void put_hex(char *out, uint32_t value, int digits)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--) {
        out[i] = hex[value & 0xF];
        value >>= 4;
    }
}

static void show_device(const struct pci_info *info, int polish)
{
    char line[64];
    int n = 0;

    put_hex(line + n, info->bus, 2);
    n += 2;
    line[n++] = ':';
    put_hex(line + n, info->slot, 2);
    n += 2;
    line[n++] = '.';
    put_hex(line + n, info->function, 1);
    n++;
    line[n++] = ' ';
    put_hex(line + n, info->vendor, 4);
    n += 4;
    line[n++] = ':';
    put_hex(line + n, info->device, 4);
    n += 4;
    line[n++] = ' ';
    line[n++] = 'c';
    line[n++] = 'l';
    line[n++] = 'a';
    line[n++] = 's';
    line[n++] = 's';
    line[n++] = ' ';
    put_hex(line + n, info->class_code >> 24, 2);
    n += 2;
    line[n++] = ':';
    put_hex(line + n, info->class_code >> 16, 2);
    n += 2;
    line[n++] = ':';
    put_hex(line + n, info->class_code >> 8, 2);
    n += 2;
    if ((info->class_code >> 24) == 0x02) {
        line[n++] = ' ';
        const char *network = polish ? "sieć" : "network";
        while (*network)
            line[n++] = *network++;
    }
    line[n++] = '\n';
    line[n] = '\0';
    print(line);
}

void _start(void)
{
    int count = 0;
    int polish = ulocale_language_polish();
    struct pci_info info;
    for (int index = 0;; index++) {
        int result = sys_pci_info(index, &info);
        if (result < 0) {
            print(polish
                      ? "lspci: nie można odczytać informacji o urządzeniach PCI\n"
                      : "lspci: unable to read PCI device information\n");
            sys_exit(1);
        }
        if (result == 0)
            break;
        show_device(&info, polish);
        count++;
    }
    if (!count)
        print(polish ? "Nie znaleziono urządzeń PCI.\n"
                     : "No PCI devices found.\n");
    print(polish
              ? "Urządzenia sieciowe mają klasę 02; identyfikatory wskazują model.\n"
              : "Network devices use class 02; vendor:device IDs identify the model.\n");
    sys_exit(0);
}
