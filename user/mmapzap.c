/* mmapzap - zapis przez plik odwzorowany w pamieci
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
 * Uzycie: run mmapzap <plik> [tryb]
 */

#include "usys.h"

#define ZMIENIONE 8192u          /* dokladnie dwie strony */
#define ZNAK_A    0xA5
#define ZNAK_B    0x5A
#define OFF_B     (4096u + 7)
#define OFF_DZIECKA 100u

static unsigned char stary(uint32_t i)  { return (unsigned char)(i * 7 + 13); }
static unsigned char nowy(uint32_t i)   { return (unsigned char)(i * 11 + 5); }

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
static int czytaj_bajt(const char *nazwa, uint32_t offset)
{
    int32_t fd = sys_open(nazwa, O_READ);
    if (fd < 0)
        return -1;
    uint32_t doszlo = 0;
    int wynik = -1;
    for (;;) {
        int32_t n = sys_read(fd, bufor, sizeof(bufor));
        if (n <= 0)
            break;
        if (offset >= doszlo && offset < doszlo + (uint32_t)n) {
            wynik = bufor[offset - doszlo];
            break;
        }
        doszlo += (uint32_t)n;
    }
    sys_close(fd);
    return wynik;
}

/* Sprawdza caly plik zwyklym odczytem wobec oczekiwanego wzorca */
static uint32_t sprawdz_czytajac(const char *nazwa)
{
    int32_t fd = sys_open(nazwa, O_READ);
    if (fd < 0)
        return 0xFFFFFFFFu;
    uint32_t doszlo = 0, zle = 0;
    for (;;) {
        int32_t n = sys_read(fd, bufor, sizeof(bufor));
        if (n <= 0)
            break;
        for (int32_t i = 0; i < n; i++) {
            uint32_t poz = doszlo + (uint32_t)i;
            unsigned char ma = poz < ZMIENIONE ? nowy(poz) : stary(poz);
            if (bufor[i] != ma)
                zle++;
        }
        doszlo += (uint32_t)n;
    }
    sys_close(fd);
    /* Pusty odczyt nie jest zgodnoscia: petla, ktora nie obejrzala ani
       jednego bajtu, zawsze naliczy zero bledow */
    if (doszlo < ZMIENIONE)
        return 0xFFFFFFFFu;
    return zle;
}

void _start(int argc, char **argv)
{
    const char *nazwa = argc > 1 ? argv[1] : "zapis.txt";
    const char *tryb = argc > 2 ? argv[2] : "pisz";

    if (rowne(tryb, "spr")) {
        uint32_t zle = sprawdz_czytajac(nazwa);
        if (zle) {
            print("[mmapzap] ZLE: zwykly odczyt widzi inne bajty: ");
            print_dec(zle);
            print("\n");
            sys_exit(1);
        }
        print("[mmapzap] zwykly odczyt widzi to, co wpisala pamiec\n");
        sys_exit(0);
    }

    if (rowne(tryb, "spr2")) {
        int a = czytaj_bajt(nazwa, 0);
        int b = czytaj_bajt(nazwa, OFF_B);
        if (a != ZNAK_A || b != ZNAK_B) {
            print("[mmapzap] ZLE: bajty bez msync nie doszly na dysk\n");
            sys_exit(1);
        }
        print("[mmapzap] bajty bez msync doszly na dysk przy koncu procesu\n");
        sys_exit(0);
    }

    uint32_t rozmiar = 0;
    unsigned char *p = sys_mmap_file_w(nazwa, &rozmiar);
    if (!p || !rozmiar) {
        print("[mmapzap] nie da sie odwzorowac pliku do zapisu\n");
        sys_exit(1);
    }

    if (rowne(tryb, "bez")) {
        p[0] = ZNAK_A;
        p[OFF_B] = ZNAK_B;
        print("[mmapzap] dwa bajty wpisane, koncze bez msync\n");
        sys_exit(0);
    }

    if (rowne(tryb, "widac")) {
        int32_t pid = sys_fork();
        if (pid == 0) {
            p[OFF_DZIECKA] = ZNAK_B;
            sys_exit(0);
        }
        if (pid < 0) {
            print("[mmapzap] ZLE: fork sie nie udal\n");
            sys_exit(1);
        }
        int kod = 0;
        sys_waitany(&kod);
        if (p[OFF_DZIECKA] != ZNAK_B) {
            print("[mmapzap] ZLE: rodzic nie widzi zapisu dziecka\n");
            sys_exit(1);
        }
        print("[mmapzap] rodzic widzi zapis dziecka - odwzorowanie wspolne\n");
        sys_exit(0);
    }

    /* Tryb 'pisz': czytamy caly plik, ale piszemy tylko po dwoch
       pierwszych stronach - i to ma byc widac w liczbie odeslanych */
    if (rozmiar <= ZMIENIONE) {
        print("[mmapzap] ZLE: plik za maly na ten test\n");
        sys_exit(1);
    }
    uint32_t zle = 0;
    for (uint32_t i = 0; i < rozmiar; i++)
        if (p[i] != stary(i))
            zle++;
    if (zle) {
        print("[mmapzap] ZLE: stary wzorzec sie nie zgadza: ");
        print_dec(zle);
        print("\n");
        sys_exit(1);
    }
    print("[mmapzap] stary wzorzec przeczytany, bajtow ");
    print_dec(rozmiar);
    print("\n");

    for (uint32_t i = 0; i < ZMIENIONE; i++)
        p[i] = nowy(i);

    int odeslane = sys_msync();
    print("[mmapzap] odeslanych stron: ");
    print_dec((uint32_t)odeslane);
    print("\n");

    int drugie = sys_msync();
    print("[mmapzap] drugie msync odeslalo: ");
    print_dec((uint32_t)drugie);
    print("\n");

    /* Pamiec ma dalej pokazywac to, co wpisalismy - odeslanie na dysk
       niczego w niej nie rusza */
    if (p[0] != nowy(0) || p[ZMIENIONE - 1] != nowy(ZMIENIONE - 1)) {
        print("[mmapzap] ZLE: pamiec zgubila zapisane bajty\n");
        sys_exit(1);
    }
    print("[mmapzap] zapis przez pamiec gotowy\n");
    sys_exit(0);
}
