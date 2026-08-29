/* mmapfile - plik widziany jako pamiec
 *
 * Odwzorowuje plik z dysku pod adres w swojej przestrzeni i czyta go
 * jak zwykla tablice - bez ani jednego wywolania read. Sprawdza trzy
 * rzeczy, kazda innym sposobem:
 *
 *   1. bajty zgadzaja sie z tym, co w pliku naprawde lezy,
 *   2. przy podanej liczbie bajtow dotyka tylko tylu - wtedy widac,
 *      ile stron jadro naprawde wczytalo z dysku,
 *   3. zapis do odwzorowania jest bledem: to odwzorowanie tylko do
 *      odczytu, wiec proba zapisu ma zabic proces, a nie po cichu
 *      przepasc. Probuje tego DZIECKO, zeby bylo komu opowiedziec,
 *      jak poszlo.
 *
 * Uzycie: run mmapfile <plik> [ile bajtow sprawdzic]
 */

#include "usys.h"

void _start(int argc, char **argv)
{
    const char *nazwa = argc > 1 ? argv[1] : "mapa.txt";

    uint32_t rozmiar = 0;
    const unsigned char *p = sys_mmap_file(nazwa, &rozmiar);
    if (!p || !rozmiar) {
        print("[mmapfile] nie da sie odwzorowac pliku\n");
        sys_exit(1);
    }
    print("[mmapfile] plik pod adresem ");
    print_dec((uint32_t)p);
    print("\n[mmapfile] rozmiar ");
    print_dec(rozmiar);
    print(" B\n");

    uint32_t ile = rozmiar;
    if (argc > 2) {
        uint32_t v = 0;
        for (const char *q = argv[2]; *q >= '0' && *q <= '9'; q++)
            v = v * 10 + (uint32_t)(*q - '0');
        if (v && v < rozmiar)
            ile = v;
    }

    uint32_t zle = 0;
    for (uint32_t i = 0; i < ile; i++)
        if (p[i] != (unsigned char)(i * 7 + 13))
            zle++;
    if (zle) {
        print("[mmapfile] ZLE: bajtow nie tak, jak trzeba: ");
        print_dec(zle);
        print("\n");
        sys_exit(1);
    }

    print("[mmapfile] sprawdzone bajty zgadzaja sie, bylo ich ");
    print_dec(ile);
    print("\n");
    if (ile != rozmiar) {
        print("[mmapfile] reszty pliku nie tknalem\n");
        sys_exit(0);
    }

    /* Zapis ma sie nie udac. Probuje dziecko - rodzic przezyje
       i opowie, co z tego wyszlo. */
    int32_t pid = sys_fork();
    if (pid == 0) {
        unsigned char *psuj = (unsigned char *)p;
        psuj[0] = 0xFF;                  /* tu proces ma zginac */
        print("[mmapfile] ZLE: zapis do odwzorowania sie udal\n");
        sys_exit(1);
    }
    if (pid > 0) {
        int kod = 0;
        sys_waitany(&kod);
        if (p[0] != (unsigned char)13) {
            print("[mmapfile] ZLE: tresc pliku zostala zmieniona\n");
            sys_exit(1);
        }
        print("[mmapfile] zapis do odwzorowania odrzucony\n");
    }

    print("[mmapfile] plik czytany jak pamiec, bez ani jednego read\n");
    sys_exit(0);
}
