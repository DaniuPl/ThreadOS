/* gterm64 - terminal w oknie graficznym (ring 3, jadro 64-bitowe)
 *
 * Przyjmuje klawisze (gdy okno ma fokus) do wiersza polecen. Po
 * Enterze uruchamia program z wyjsciem przekierowanym do pliku,
 * czeka na jego koniec, czyta plik i rysuje wynik w oknie.
 *
 * Nie ma tu ani jednej rzeczy, ktorej jadro nie umialoby wczesniej -
 * jest za to wszystko naraz: klawiatura przez zdarzenia okna, fork
 * i exec, przekierowanie przez dup2, czekanie na dziecko, system
 * plikow i rysowanie. Terminal jest zwyklym programem w pierscieniu 3
 * i nie wie o kompozytorze nic poza czterema wywolaniami okien.
 */

#include "usys64.h"

#define C_GREEN 2
#define C_WHITE 4
#define C_BG    8

#define OKNO_W 240
#define OKNO_H 150

#define WIERSZ_MAX 60
#define PLIK "termout"

static char wiersz[WIERSZ_MAX];
static int dlug;
static char wynik[1024];

/* Uruchamia wpisane polecenie z wyjsciem do pliku i wczytuje wynik */
static void uruchom(void)
{
    wiersz[dlug] = '\0';
    dlug = 0;
    if (!wiersz[0])
        return;

    /* Plik od nowa: inaczej krotszy wynik zostawilby ogon
       poprzedniego. Kasujemy i zakladamy jeszcze raz - to samo, co
       zrobilby 'rm' przed przekierowaniem. */
    sys_unlink(PLIK);
    int fd = (int)sys_open(PLIK, 1);
    if (fd < 0)
        return;

    int64_t pid = sys_fork();
    if (pid == 0) {
        /* Od tej chwili wypisy dziecka ida do pliku, a nie na konsole.
           Dziecko o tym nie wie i wiedziec nie musi - dla niego to
           dalej jest deskryptor numer jeden. */
        sys_dup2(fd, FD_STDOUT);
        sys_close(fd);
        char *argv[2] = {wiersz, 0};
        sys_exec(wiersz, argv);
        sys_exit(127);               /* exec wraca tylko przy bledzie */
    }
    sys_close(fd);
    if (pid > 0) {
        int kod = 0;
        sys_wait(&kod);
    }

    int rd = (int)sys_open(PLIK, 0);
    if (rd < 0) {
        wynik[0] = '\0';
        return;
    }
    int64_t n = sys_read(rd, wynik, sizeof(wynik) - 1);
    sys_close(rd);
    wynik[n < 0 ? 0 : n] = '\0';
}

/* Rysuje bufor wyjscia rozbity na wiersze po znaku nowej linii */
static void rysuj_wynik(int w)
{
    char buf[40];
    int kol = 0, rzad = 0, y = 4;

    for (int i = 0; wynik[i] && rzad < 12; i++) {
        if (wynik[i] == '\n' || kol >= (int)sizeof(buf) - 1) {
            buf[kol] = '\0';
            win_text(w, 4, y, buf, C_WHITE);
            y += 10;
            rzad++;
            kol = 0;
            if (wynik[i] != '\n')
                buf[kol++] = wynik[i];
        } else if (wynik[i] != '\r') {
            buf[kol++] = wynik[i];
        }
    }
    if (kol) {
        buf[kol] = '\0';
        win_text(w, 4, y, buf, C_WHITE);
    }
}

void _start(void)
{
    int w = (int)win_create(OKNO_W, OKNO_H, "Terminal");
    if (w < 0) {
        print("gterm64: pulpit nie dal okna\n");
        sys_exit(1);
    }

    for (;;) {
        struct win_event ev;
        while (win_poll(w, &ev) == 1) {
            if (ev.type != WEV_KEY)
                continue;
            char c = (char)ev.val;
            if (c == '\n')
                uruchom();
            else if (c == '\b') {
                if (dlug > 0)
                    dlug--;
            } else if (c >= 0x20 && c < 0x7F && dlug < WIERSZ_MAX - 1) {
                wiersz[dlug++] = c;
            }
        }

        win_rect(w, 0, 0, OKNO_W, OKNO_H, C_BG);
        rysuj_wynik(w);
        wiersz[dlug] = '\0';
        win_text(w, 4, OKNO_H - 12, ">", C_GREEN);
        win_text(w, 16, OKNO_H - 12, wiersz, C_WHITE);

        sys_sleep(60);
    }
}
