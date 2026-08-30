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

#define CHANGED 8192u          /* dokladnie dwie strony */
#define CH_A    0xA5
#define CH_B    0x5A
#define OFF_B     (4096u + 7)
#define OFF_CHILD 100u

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

static unsigned char old(uint64_t i)  { return (unsigned char)(i * 7 + 13); }
static unsigned char fresh_one(uint64_t i)   { return (unsigned char)(i * 11 + 5); }

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
static int read_byte(const char *name, uint64_t offset)
{
    int64_t fd = sys_open(name, 0);
    if (fd < 0)
        return -1;
    uint64_t doszlo = 0;
    int result = -1;
    for (;;) {
        int64_t n = sys_read((int)fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        if (offset >= doszlo && offset < doszlo + (uint64_t)n) {
            result = buf[offset - doszlo];
            break;
        }
        doszlo += (uint64_t)n;
    }
    sys_close((int)fd);
    return result;
}

/* Sprawdza caly plik zwyklym odczytem wobec oczekiwanego wzorca */
static uint64_t check_by_reading(const char *name)
{
    int64_t fd = sys_open(name, 0);
    if (fd < 0)
        return (uint64_t)-1;
    uint64_t doszlo = 0, bad = 0;
    for (;;) {
        int64_t n = sys_read((int)fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        for (int64_t i = 0; i < n; i++) {
            uint64_t poz = doszlo + (uint64_t)i;
            unsigned char ma = poz < CHANGED ? fresh_one(poz) : old(poz);
            if (buf[i] != ma)
                bad++;
        }
        doszlo += (uint64_t)n;
    }
    sys_close((int)fd);
    /* Pusty odczyt nie jest zgodnoscia: petla, ktora nie obejrzala ani
       jednego bajtu, zawsze naliczy zero bledow */
    if (doszlo < CHANGED)
        return (uint64_t)-1;
    return bad;
}

void _start(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "zapis.txt";
    const char *mode = argc > 2 ? argv[2] : "pisz";

    if (equal(mode, "spr")) {
        uint64_t bad = check_by_reading(name);
        if (bad) {
            report("[mmapzap64] ZLE: zwykly odczyt widzi inne bajty: ",
                    bad, "");
            sys_exit(1);
        }
        print("[mmapzap64] zwykly odczyt widzi to, co wpisala pamiec\n");
        sys_exit(0);
    }

    if (equal(mode, "spr2")) {
        int a = read_byte(name, 0);
        int b = read_byte(name, OFF_B);
        if (a != CH_A || b != CH_B) {
            print("[mmapzap64] ZLE: bajty bez msync nie doszly na dysk\n");
            sys_exit(1);
        }
        print("[mmapzap64] bajty bez msync doszly na dysk"
              " przy koncu procesu\n");
        sys_exit(0);
    }

    uint64_t size = 0;
    unsigned char *p = sys_mmap_file_w(name, &size);
    if (!p || !size) {
        print("[mmapzap64] nie da sie odwzorowac pliku do zapisu\n");
        sys_exit(1);
    }

    if (equal(mode, "bez")) {
        p[0] = CH_A;
        p[OFF_B] = CH_B;
        print("[mmapzap64] dwa bajty wpisane, koncze bez msync\n");
        sys_exit(0);
    }

    if (equal(mode, "widac")) {
        int64_t pid = sys_fork();
        if (pid == 0) {
            p[OFF_CHILD] = CH_B;
            sys_exit(0);
        }
        if (pid < 0) {
            print("[mmapzap64] ZLE: fork sie nie udal\n");
            sys_exit(1);
        }
        int code = 0;
        sys_wait(&code);
        if (p[OFF_CHILD] != CH_B) {
            print("[mmapzap64] ZLE: rodzic nie widzi zapisu dziecka\n");
            sys_exit(1);
        }
        print("[mmapzap64] rodzic widzi zapis dziecka"
              " - odwzorowanie wspolne\n");
        sys_exit(0);
    }

    /* Tryb 'pisz': czytamy caly plik, ale piszemy tylko po dwoch
       pierwszych stronach - i to ma byc widac w liczbie odeslanych */
    if (size <= CHANGED) {
        print("[mmapzap64] ZLE: plik za maly na ten test\n");
        sys_exit(1);
    }
    uint64_t bad = 0;
    for (uint64_t i = 0; i < size; i++)
        if (p[i] != old(i))
            bad++;
    if (bad) {
        report("[mmapzap64] ZLE: stary wzorzec sie nie zgadza: ", bad, "");
        sys_exit(1);
    }
    report("[mmapzap64] stary wzorzec przeczytany, bajtow ", size, "");

    for (uint64_t i = 0; i < CHANGED; i++)
        p[i] = fresh_one(i);

    int64_t sent_back = sys_msync();
    report("[mmapzap64] odeslanych stron: ", (uint64_t)sent_back, "");

    int64_t drugie = sys_msync();
    report("[mmapzap64] drugie msync odeslalo: ", (uint64_t)drugie, "");

    /* Pamiec ma dalej pokazywac to, co wpisalismy - odeslanie na dysk
       niczego w niej nie rusza */
    if (p[0] != fresh_one(0) || p[CHANGED - 1] != fresh_one(CHANGED - 1)) {
        print("[mmapzap64] ZLE: pamiec zgubila zapisane bajty\n");
        sys_exit(1);
    }
    print("[mmapzap64] zapis przez pamiec gotowy\n");
    sys_exit(0);
}
