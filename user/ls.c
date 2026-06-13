/* ls - wypisuje pliki na dysku (nazwa + rozmiar) przez sys_readdir.
 * Dziala w ring 3 - enumeracje katalogu udostepnia jadro syscallem.
 */

#include "usys.h"

#define MAX_SLOTS 64

void _start(void)
{
    int found = 0;
    for (int slot = 0; slot < MAX_SLOTS; slot++) {
        char name[FS_NAME_MAX + 1];
        int32_t size = sys_readdir(slot, name);
        if (size < 0)
            continue;
        print(name);
        print("  ");
        print_dec((uint32_t)size);
        print(" B\n");
        found++;
    }
    if (!found)
        print("(brak plikow)\n");
    sys_exit(0);
}
