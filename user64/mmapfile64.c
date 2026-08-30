/* mmapfile64 - plik widziany jako pamiec
 *
 * Odwzorowuje plik z dysku pod adres w swojej przestrzeni i czyta go
 * jak zwykla tablice - bez ani jednego wywolania read. Sprawdza trzy
 * rzeczy, kazda innym sposobem:
 *
 *   1. bajty zgadzaja sie z tym, co w pliku naprawde lezy,
 *   2. czytanie dalekiego konca pliku dziala tak samo, jak poczatku
 *      (strony przychodza pod palcami, a nie wszystkie na raz),
 *   3. zapis do odwzorowania jest bledem - to jest odwzorowanie tylko
 *      do odczytu, wiec proba zapisu ma zabic proces, a nie po cichu
 *      przepasc. Probuje tego DZIECKO, zeby bylo komu opowiedziec,
 *      jak poszlo.
 *
 * Uzycie: run mmapfile64 <plik> [ile bajtow sprawdzic]
 *
 * Drugi argument sluzy do pokazania, ze strony naprawde przychodza
 * pod palcami: przy "1" program dotyka jednego bajtu i konczy, a jadro
 * melduje, ile stron musialo wczytac z dysku. Dla stustronicowego
 * pliku ma to byc jedna.
 */

#include "usys64.h"

static char out[128];

static void report(const char *a, uint64_t v, const char *b)
{
    int n = 0;
    while (*a)
        out[n++] = *a++;
    char tmp[24];
    utoa(v, tmp);
    for (int i = 0; tmp[i]; i++)
        out[n++] = tmp[i];
    while (*b)
        out[n++] = *b++;
    out[n++] = '\n';
    out[n] = '\0';
    print(out);
}

void _start(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "mapa.txt";

    uint64_t size = 0;
    const unsigned char *p = sys_mmap_file(name, &size);
    if (!p) {
        print("[mmapfile64] nie da sie odwzorowac pliku\n");
        sys_exit(1);
    }
    report("[mmapfile64] plik pod adresem ", (uint64_t)(uintptr_t)p, "");
    report("[mmapfile64] rozmiar ", size, " B");

    /* Ile bajtow sprawdzic. Domyslnie caly plik - ale przy jawnie
       podanej liczbie dotykamy tylko tylu, i wtedy widac, ile stron
       jadro naprawde wczytalo. */
    uint64_t ile = size;
    if (argc > 2) {
        int64_t v = uatoi(argv[2]);
        if (v > 0 && (uint64_t)v < size)
            ile = (uint64_t)v;
    }

    /* Wzorzec pliku: bajt o numerze i ma wartosc (i * 7 + 13) & 0xFF.
       Zapisal go mkmap64, zanim plik trafil na dysk. */
    uint64_t bad = 0;
    for (uint64_t i = 0; i < ile; i++)
        if (p[i] != (unsigned char)(i * 7 + 13))
            bad++;

    if (bad) {
        report("[mmapfile64] ZLE: bajtow nie tak, jak trzeba: ", bad, "");
        sys_exit(1);
    }
    report("[mmapfile64] sprawdzone bajty zgadzaja sie, bylo ich ",
            ile, "");
    if (ile != size) {
        print("[mmapfile64] reszty pliku nie tknalem\n");
        sys_exit(0);
    }

    /* Zapis: ma sie nie udac. Probuje dziecko - rodzic przezyje
       i opowie, co z tego wyszlo. */
    int64_t pid = sys_fork();
    if (pid == 0) {
        unsigned char *corrupt = (unsigned char *)p;
        corrupt[0] = 0xFF;                  /* tu proces ma zginac */
        print("[mmapfile64] ZLE: zapis do odwzorowania sie udal\n");
        sys_exit(1);
    }
    if (pid > 0) {
        int code = 0;
        sys_wait(&code);
        if (p[0] != (unsigned char)13) {
            print("[mmapfile64] ZLE: tresc pliku zostala zmieniona\n");
            sys_exit(1);
        }
        print("[mmapfile64] zapis do odwzorowania odrzucony\n");
    }

    print("[mmapfile64] plik czytany jak pamiec, bez ani jednego read\n");
    sys_exit(0);
}
