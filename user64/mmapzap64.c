/* mmapzap64 - zapis przez plik odwzorowany w pamieci (tryb dlugi)
 *
 * Odwzorowanie z prawem zapisu: program zmienia bajty w pamieci, a one
 * maja trafic do pliku na dysku. Nie po kazdym bajcie - o tym, kiedy,
 * decyduje sam program przez sys_msync (albo jadro, gdy proces sie
 * konczy). Ktore strony odeslac, wie procesor: brudna jest ta, do
 * ktorej naprawde ktos pisal.
 *
 * Tryby (drugi argument, domyslnie 'pisz'):
 *
 *   pisz   - sprawdza stary wzorzec, wpisuje nowy w pierwsze dwie
 *            strony i odsyla je jawnie. Odeslac ma DWIE strony, nie
 *            wszystkie: reszte pliku tylko czytalismy, a czytanie
 *            brudnym nie robi. Drugie sys_msync ma dac zero - po
 *            odeslaniu strona przestaje byc brudna.
 *   spr    - otwiera plik ZWYKLYM sys_read i sprawdza, ze widzi tam
 *            nowy wzorzec w pierwszych dwoch stronach, a stary dalej.
 *   bez    - pisze dwa bajty i konczy sie BEZ sys_msync. Odeslac ma
 *            je jadro przy koncu procesu.
 *   spr2   - sprawdza zwyklym odczytem, ze te dwa bajty tam sa.
 *   widac  - rozwidla sie; dziecko pisze przez odwzorowanie, a rodzic
 *            ma to zobaczyc u siebie. Zapisywalne odwzorowanie jest
 *            WSPOLNE, a nie kopiowane przy zapisie - inaczej zapisy
 *            dziecka nigdy nie trafilyby do pliku.
 *
 * Uzycie: run mmapzap64 <plik> [tryb]
 */

#include "usys64.h"

#define ZMIENIONE 8192u          /* dokladnie dwie strony */
#define ZNAK_A    0xA5
#define ZNAK_B    0x5A
#define OFF_B     (4096u + 7)
#define OFF_DZIECKA 100u

static char out[128];

static void powiedz(const char *a, uint64_t v, const char *b)
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

static unsigned char stary(uint64_t i)  { return (unsigned char)(i * 7 + 13); }
static unsigned char nowy(uint64_t i)   { return (unsigned char)(i * 11 + 5); }

static int rowne(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static unsigned char bufor[512];

/* Czyta plik zwyklym sys_read i oddaje bajt spod danego miejsca.
   Zwraca -1, gdy pliku nie ma albo jest za krotki. */
static int czytaj_bajt(const char *nazwa, uint64_t offset)
{
    int64_t fd = sys_open(nazwa, 0);
    if (fd < 0)
        return -1;
    uint64_t doszlo = 0;
    int wynik = -1;
    for (;;) {
        int64_t n = sys_read((int)fd, bufor, sizeof(bufor));
        if (n <= 0)
            break;
        if (offset >= doszlo && offset < doszlo + (uint64_t)n) {
            wynik = bufor[offset - doszlo];
            break;
        }
        doszlo += (uint64_t)n;
    }
    sys_close((int)fd);
    return wynik;
}

/* Sprawdza caly plik zwyklym odczytem wobec oczekiwanego wzorca */
static uint64_t sprawdz_czytajac(const char *nazwa)
{
    int64_t fd = sys_open(nazwa, 0);
    if (fd < 0)
        return (uint64_t)-1;
    uint64_t doszlo = 0, zle = 0;
    for (;;) {
        int64_t n = sys_read((int)fd, bufor, sizeof(bufor));
        if (n <= 0)
            break;
        for (int64_t i = 0; i < n; i++) {
            uint64_t poz = doszlo + (uint64_t)i;
            unsigned char ma = poz < ZMIENIONE ? nowy(poz) : stary(poz);
            if (bufor[i] != ma)
                zle++;
        }
        doszlo += (uint64_t)n;
    }
    sys_close((int)fd);
    /* Pusty odczyt nie jest zgodnoscia: petla, ktora nie obejrzala ani
       jednego bajtu, zawsze naliczy zero bledow */
    if (doszlo < ZMIENIONE)
        return (uint64_t)-1;
    return zle;
}

void _start(int argc, char **argv)
{
    const char *nazwa = argc > 1 ? argv[1] : "zapis.txt";
    const char *tryb = argc > 2 ? argv[2] : "pisz";

    if (rowne(tryb, "spr")) {
        uint64_t zle = sprawdz_czytajac(nazwa);
        if (zle) {
            powiedz("[mmapzap64] ZLE: zwykly odczyt widzi inne bajty: ",
                    zle, "");
            sys_exit(1);
        }
        print("[mmapzap64] zwykly odczyt widzi to, co wpisala pamiec\n");
        sys_exit(0);
    }

    if (rowne(tryb, "spr2")) {
        int a = czytaj_bajt(nazwa, 0);
        int b = czytaj_bajt(nazwa, OFF_B);
        if (a != ZNAK_A || b != ZNAK_B) {
            print("[mmapzap64] ZLE: bajty bez msync nie doszly na dysk\n");
            sys_exit(1);
        }
        print("[mmapzap64] bajty bez msync doszly na dysk"
              " przy koncu procesu\n");
        sys_exit(0);
    }

    uint64_t rozmiar = 0;
    unsigned char *p = sys_mmap_file_w(nazwa, &rozmiar);
    if (!p || !rozmiar) {
        print("[mmapzap64] nie da sie odwzorowac pliku do zapisu\n");
        sys_exit(1);
    }

    if (rowne(tryb, "bez")) {
        p[0] = ZNAK_A;
        p[OFF_B] = ZNAK_B;
        print("[mmapzap64] dwa bajty wpisane, koncze bez msync\n");
        sys_exit(0);
    }

    if (rowne(tryb, "widac")) {
        int64_t pid = sys_fork();
        if (pid == 0) {
            p[OFF_DZIECKA] = ZNAK_B;
            sys_exit(0);
        }
        if (pid < 0) {
            print("[mmapzap64] ZLE: fork sie nie udal\n");
            sys_exit(1);
        }
        int kod = 0;
        sys_wait(&kod);
        if (p[OFF_DZIECKA] != ZNAK_B) {
            print("[mmapzap64] ZLE: rodzic nie widzi zapisu dziecka\n");
            sys_exit(1);
        }
        print("[mmapzap64] rodzic widzi zapis dziecka"
              " - odwzorowanie wspolne\n");
        sys_exit(0);
    }

    /* Tryb 'pisz': czytamy caly plik, ale piszemy tylko po dwoch
       pierwszych stronach - i to ma byc widac w liczbie odeslanych */
    if (rozmiar <= ZMIENIONE) {
        print("[mmapzap64] ZLE: plik za maly na ten test\n");
        sys_exit(1);
    }
    uint64_t zle = 0;
    for (uint64_t i = 0; i < rozmiar; i++)
        if (p[i] != stary(i))
            zle++;
    if (zle) {
        powiedz("[mmapzap64] ZLE: stary wzorzec sie nie zgadza: ", zle, "");
        sys_exit(1);
    }
    powiedz("[mmapzap64] stary wzorzec przeczytany, bajtow ", rozmiar, "");

    for (uint64_t i = 0; i < ZMIENIONE; i++)
        p[i] = nowy(i);

    int64_t odeslane = sys_msync();
    powiedz("[mmapzap64] odeslanych stron: ", (uint64_t)odeslane, "");

    int64_t drugie = sys_msync();
    powiedz("[mmapzap64] drugie msync odeslalo: ", (uint64_t)drugie, "");

    /* Pamiec ma dalej pokazywac to, co wpisalismy - odeslanie na dysk
       niczego w niej nie rusza */
    if (p[0] != nowy(0) || p[ZMIENIONE - 1] != nowy(ZMIENIONE - 1)) {
        print("[mmapzap64] ZLE: pamiec zgubila zapisane bajty\n");
        sys_exit(1);
    }
    print("[mmapzap64] zapis przez pamiec gotowy\n");
    sys_exit(0);
}
