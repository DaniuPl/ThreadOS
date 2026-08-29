/* gfiles64 - menedzer plikow w oknie (ring 3, jadro 64-bitowe)
 *
 * Listuje pliki z dysku w oknie; klikniecie nazwy otwiera plik
 * i pokazuje jego poczatek w panelu podgladu. Spina cztery rzeczy
 * naraz: system plikow, zdarzenia myszy, wywolania okien i wlasna
 * przestrzen adresowa procesu - a wszystko z pierscienia 3.
 */

#include "usys64.h"

#define C_BG    8
#define C_BLUE  3
#define C_GREEN 2
#define C_WHITE 4

#define OKNO_W 180
#define OKNO_H 120
#define WIERSZ_Y0 16
#define WIERSZ_H  10
#define MAXP      6

static char nazwy[MAXP][NAME_MAX64 + 1];
static int ile;
static char zaznaczony[NAME_MAX64 + 1];
static char podglad[96];

static void skopiuj(char *d, const char *s, int max)
{
    int i = 0;
    while (s[i] && i < max - 1) {
        d[i] = s[i];
        i++;
    }
    d[i] = '\0';
}

static int takie_same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Otwiera zaznaczony plik i wczytuje jego poczatek do podgladu */
static void wczytaj(const char *nazwa)
{
    skopiuj(zaznaczony, nazwa, sizeof(zaznaczony));

    int fd = (int)sys_open(nazwa, 0);
    if (fd < 0) {
        skopiuj(podglad, "(nie da sie otworzyc)", sizeof(podglad));
        return;
    }
    int64_t n = sys_read(fd, podglad, sizeof(podglad) - 1);
    sys_close(fd);
    if (n < 0)
        n = 0;
    podglad[n] = '\0';
    /* Podglad rysuje sie w jednej linii - koniec na pierwszym enterze */
    for (int i = 0; podglad[i]; i++)
        if (podglad[i] == '\n') {
            podglad[i] = '\0';
            break;
        }
}

void _start(void)
{
    int w = (int)win_create(OKNO_W, OKNO_H, "Pliki");
    if (w < 0) {
        print("gfiles64: pulpit nie dal okna\n");
        sys_exit(1);
    }

    for (;;) {
        struct win_event ev;
        while (win_poll(w, &ev) == 1) {
            if (ev.type == WEV_CLICK) {
                int wiersz = (ev.y - WIERSZ_Y0) / WIERSZ_H;
                if (ev.y >= WIERSZ_Y0 && wiersz >= 0 && wiersz < ile)
                    wczytaj(nazwy[wiersz]);
            }
        }

        win_rect(w, 0, 0, OKNO_W, OKNO_H, C_BG);
        win_text(w, 4, 2, "Pliki na dysku:", C_GREEN);

        ile = 0;
        struct dirent64 wpis;
        for (int i = 0; i < 32 && ile < MAXP; i++) {
            /* sys_readdir oddaje ZERO przy trafieniu, a -1 na koncu
               katalogu - inaczej niz fs_readdir w jadrze, ktore oddaje
               jedynke. Pomylenie tych dwoch konwencji konczy sie
               pustym oknem, bo pierwszy wpis wyglada jak koniec. */
            if (sys_readdir(i, &wpis) != 0)
                break;
            skopiuj(nazwy[ile], wpis.name, sizeof(nazwy[0]));
            int kolor = takie_same(wpis.name, zaznaczony) ? C_BLUE : C_WHITE;
            win_text(w, 8, WIERSZ_Y0 + ile * WIERSZ_H, nazwy[ile], kolor);
            ile++;
        }
        if (ile == 0)
            win_text(w, 8, WIERSZ_Y0, "(pusto)", C_WHITE);

        win_rect(w, 0, 86, OKNO_W, 1, C_WHITE);
        if (zaznaczony[0]) {
            win_text(w, 4, 90, "tresc:", C_GREEN);
            win_text(w, 4, 102, podglad, C_WHITE);
        }

        sys_sleep(120);
    }
}
