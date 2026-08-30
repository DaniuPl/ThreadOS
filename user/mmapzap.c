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

#define CHANGED 8192u          /* dokladnie dwie strony */
#define CH_A    0xA5
#define CH_B    0x5A
#define OFF_B     (4096u + 7)
#define OFF_CHILD 100u

static unsigned char old(uint32_t i)  { return (unsigned char)(i * 7 + 13); }
static unsigned char fresh_one(uint32_t i)   { return (unsigned char)(i * 11 + 5); }

static int equal(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static unsigned char buf[512];

/* Czyta plik zwyklym sys_read i oddaje bajt spod danego miejsca.
   Zwraca -1, gdy pliku nie ma albo jest za krotki. */
static int read_byte(const char *name, uint32_t offset)
{
    int32_t fd = sys_open(name, O_READ);
    if (fd < 0)
        return -1;
    uint32_t doszlo = 0;
    int result = -1;
    for (;;) {
        int32_t n = sys_read(fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        if (offset >= doszlo && offset < doszlo + (uint32_t)n) {
            result = buf[offset - doszlo];
            break;
        }
        doszlo += (uint32_t)n;
    }
    sys_close(fd);
    return result;
}

/* Sprawdza caly plik zwyklym odczytem wobec oczekiwanego wzorca */
static uint32_t check_by_reading(const char *name)
{
    int32_t fd = sys_open(name, O_READ);
    if (fd < 0)
        return 0xFFFFFFFFu;
    uint32_t doszlo = 0, bad = 0;
    for (;;) {
        int32_t n = sys_read(fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        for (int32_t i = 0; i < n; i++) {
            uint32_t poz = doszlo + (uint32_t)i;
            unsigned char ma = poz < CHANGED ? fresh_one(poz) : old(poz);
            if (buf[i] != ma)
                bad++;
        }
        doszlo += (uint32_t)n;
    }
    sys_close(fd);
    /* Pusty odczyt nie jest zgodnoscia: petla, ktora nie obejrzala ani
       jednego bajtu, zawsze naliczy zero bledow */
    if (doszlo < CHANGED)
        return 0xFFFFFFFFu;
    return bad;
}

void _start(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "zapis.txt";
    const char *mode = argc > 2 ? argv[2] : "pisz";

    if (equal(mode, "spr")) {
        uint32_t bad = check_by_reading(name);
        if (bad) {
            print("[mmapzap] ZLE: zwykly odczyt widzi inne bajty: ");
            print_dec(bad);
            print("\n");
            sys_exit(1);
        }
        print("[mmapzap] zwykly odczyt widzi to, co wpisala pamiec\n");
        sys_exit(0);
    }

    if (equal(mode, "spr2")) {
        int a = read_byte(name, 0);
        int b = read_byte(name, OFF_B);
        if (a != CH_A || b != CH_B) {
            print("[mmapzap] ZLE: bajty bez msync nie doszly na dysk\n");
            sys_exit(1);
        }
        print("[mmapzap] bajty bez msync doszly na dysk przy koncu procesu\n");
        sys_exit(0);
    }

    uint32_t size = 0;
    unsigned char *p = sys_mmap_file_w(name, &size);
    if (!p || !size) {
        print("[mmapzap] nie da sie odwzorowac pliku do zapisu\n");
        sys_exit(1);
    }

    if (equal(mode, "bez")) {
        p[0] = CH_A;
        p[OFF_B] = CH_B;
        print("[mmapzap] dwa bajty wpisane, koncze bez msync\n");
        sys_exit(0);
    }

    if (equal(mode, "widac")) {
        int32_t pid = sys_fork();
        if (pid == 0) {
            p[OFF_CHILD] = CH_B;
            sys_exit(0);
        }
        if (pid < 0) {
            print("[mmapzap] ZLE: fork sie nie udal\n");
            sys_exit(1);
        }
        int code = 0;
        sys_waitany(&code);
        if (p[OFF_CHILD] != CH_B) {
            print("[mmapzap] ZLE: rodzic nie widzi zapisu dziecka\n");
            sys_exit(1);
        }
        print("[mmapzap] rodzic widzi zapis dziecka - odwzorowanie wspolne\n");
        sys_exit(0);
    }

    /* Tryb 'pisz': czytamy caly plik, ale piszemy tylko po dwoch
       pierwszych stronach - i to ma byc widac w liczbie odeslanych */
    if (size <= CHANGED) {
        print("[mmapzap] ZLE: plik za maly na ten test\n");
        sys_exit(1);
    }
    uint32_t bad = 0;
    for (uint32_t i = 0; i < size; i++)
        if (p[i] != old(i))
            bad++;
    if (bad) {
        print("[mmapzap] ZLE: stary wzorzec sie nie zgadza: ");
        print_dec(bad);
        print("\n");
        sys_exit(1);
    }
    print("[mmapzap] stary wzorzec przeczytany, bajtow ");
    print_dec(size);
    print("\n");

    for (uint32_t i = 0; i < CHANGED; i++)
        p[i] = fresh_one(i);

    int sent_back = sys_msync();
    print("[mmapzap] odeslanych stron: ");
    print_dec((uint32_t)sent_back);
    print("\n");

    int drugie = sys_msync();
    print("[mmapzap] drugie msync odeslalo: ");
    print_dec((uint32_t)drugie);
    print("\n");

    /* Pamiec ma dalej pokazywac to, co wpisalismy - odeslanie na dysk
       niczego w niej nie rusza */
    if (p[0] != fresh_one(0) || p[CHANGED - 1] != fresh_one(CHANGED - 1)) {
        print("[mmapzap] ZLE: pamiec zgubila zapisane bajty\n");
        sys_exit(1);
    }
    print("[mmapzap] zapis przez pamiec gotowy\n");
    sys_exit(0);
}
