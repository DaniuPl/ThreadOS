#include "pciinfo.h"

static void put_hex(char *out, uint32_t value, int digits)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--) {
        out[i] = hex[value & 0xF];
        value >>= 4;
    }
}

static void show_device(const struct pci_info *info)
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
        line[n++] = 'n';
        line[n++] = 'e';
        line[n++] = 't';
        line[n++] = 'w';
        line[n++] = 'o';
        line[n++] = 'r';
        line[n++] = 'k';
    }
    line[n++] = '\n';
    line[n] = '\0';
    print(line);
}

int main(void)
{
    int count = 0;
    struct pci_info info;
    for (int index = 0;; index++) {
        int result = sys_pci_info(index, &info);
        if (result < 0) {
            print("lspci: unable to read PCI device information\n");
            return 1;
        }
        if (result == 0)
            break;
        show_device(&info);
        count++;
    }
    if (!count)
        print("No PCI devices found.\n");
    print("Network devices use class 02; vendor:device IDs identify the model.\n");
    return 0;
}
