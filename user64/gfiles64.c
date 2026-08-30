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

#define WINDOW_W 180
#define WINDOW_H 120
#define ROW_Y0 16
#define ROW_H_PX  10
#define MAXP      6

static char names[MAXP][NAME_MAX64 + 1];
static int ile;
static char selected[NAME_MAX64 + 1];
static char preview[96];

static void copy_out(char *d, const char *s, int max)
{
    int i = 0;
    while (s[i] && i < max - 1) {
        d[i] = s[i];
        i++;
    }
    d[i] = '\0';
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Otwiera zaznaczony plik i wczytuje jego poczatek do podgladu */
static void load(const char *name)
{
    copy_out(selected, name, sizeof(selected));

    int fd = (int)sys_open(name, 0);
    if (fd < 0) {
        copy_out(preview, "(nie da sie otworzyc)", sizeof(preview));
        return;
    }
    int64_t n = sys_read(fd, preview, sizeof(preview) - 1);
    sys_close(fd);
    if (n < 0)
        n = 0;
    preview[n] = '\0';
    /* Podglad rysuje sie w jednej linii - koniec na pierwszym enterze */
    for (int i = 0; preview[i]; i++)
        if (preview[i] == '\n') {
            preview[i] = '\0';
            break;
        }
}

void _start(void)
{
    int w = (int)win_create(WINDOW_W, WINDOW_H, "Pliki");
    if (w < 0) {
        print("gfiles64: pulpit nie dal okna\n");
        sys_exit(1);
    }

    for (;;) {
        struct win_event ev;
        while (win_poll(w, &ev) == 1) {
            if (ev.type == WEV_CLICK) {
                int row_text = (ev.y - ROW_Y0) / ROW_H_PX;
                if (ev.y >= ROW_Y0 && row_text >= 0 && row_text < ile)
                    load(names[row_text]);
            }
        }

        win_rect(w, 0, 0, WINDOW_W, WINDOW_H, C_BG);
        win_text(w, 4, 2, "Pliki na dysku:", C_GREEN);

        ile = 0;
        struct dirent64 entry;
        for (int i = 0; i < 32 && ile < MAXP; i++) {
            /* sys_readdir oddaje ZERO przy trafieniu, a -1 na koncu
               katalogu - inaczej niz fs_readdir w jadrze, ktore oddaje
               jedynke. Pomylenie tych dwoch konwencji konczy sie
               pustym oknem, bo pierwszy wpis wyglada jak koniec. */
            if (sys_readdir(i, &entry) != 0)
                break;
            copy_out(names[ile], entry.name, sizeof(names[0]));
            int color = same(entry.name, selected) ? C_BLUE : C_WHITE;
            win_text(w, 8, ROW_Y0 + ile * ROW_H_PX, names[ile], color);
            ile++;
        }
        if (ile == 0)
            win_text(w, 8, ROW_Y0, "(pusto)", C_WHITE);

        win_rect(w, 0, 86, WINDOW_W, 1, C_WHITE);
        if (selected[0]) {
            win_text(w, 4, 90, "tresc:", C_GREEN);
            win_text(w, 4, 102, preview, C_WHITE);
        }

        sys_sleep(120);
    }
}
