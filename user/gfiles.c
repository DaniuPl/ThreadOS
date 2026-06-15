/* gfiles - graficzny menedzer plikow (ring 3)
 *
 * Listuje pliki z dysku (PFS) w oknie. Klikniecie nazwy otwiera plik
 * (sys_open/read) i pokazuje jego tresc w panelu podgladu. Spina FS,
 * zdarzenia myszy i GUI: FS -> klik -> odczyt -> rysowanie w oknie.
 */

#include "usys.h"

#define C_BG    8
#define C_WHITE 4
#define C_GREEN 2
#define C_BLUE  3

#define ROW_Y0 16
#define ROW_H  10
#define MAXF   8

static char names[MAXF][FS_NAME_MAX + 1];
static int nfiles;
static char selected[FS_NAME_MAX + 1];
static char preview[128];

static int ustrcpy(char *d, const char *s, int max)
{
    int i = 0;
    while (s[i] && i < max - 1) {
        d[i] = s[i];
        i++;
    }
    d[i] = '\0';
    return i;
}

/* Otwiera zaznaczony plik i wczytuje jego tresc do podgladu */
static void load_preview(const char *name)
{
    ustrcpy(selected, name, sizeof(selected));
    int fd = sys_open(name, O_READ);
    if (fd < 0) {
        ustrcpy(preview, "(blad odczytu)", sizeof(preview));
        return;
    }
    int n = sys_read(fd, preview, sizeof(preview) - 1);
    sys_close(fd);
    if (n < 0)
        n = 0;
    preview[n] = '\0';
}

void _start(void)
{
    int w = win_create(180, 120, "Pliki");
    if (w < 0)
        sys_exit(1);

    for (;;) {
        /* Zdarzenia: klik w wiersz pliku -> wczytaj podglad */
        struct win_event ev;
        while (win_poll(w, &ev) == 1) {
            if (ev.type == WEV_CLICK) {
                int row = (ev.y - ROW_Y0) / ROW_H;
                if (ev.y >= ROW_Y0 && row >= 0 && row < nfiles)
                    load_preview(names[row]);
            }
        }

        /* Odswiez liste plikow z dysku */
        win_rect(w, 0, 0, 180, 120, C_BG);
        win_text(w, 4, 2, "Pliki na dysku:", C_GREEN);
        nfiles = 0;
        char name[FS_NAME_MAX + 1];
        for (int i = 0; i < 32 && nfiles < MAXF; i++) {
            int32_t r = sys_readdir(i, name);
            if (r == -1)
                break;
            ustrcpy(names[nfiles], name, sizeof(names[0]));
            int col = ustreq(name, selected) ? C_BLUE : C_WHITE;
            win_text(w, 8, ROW_Y0 + nfiles * ROW_H, name, col);
            nfiles++;
        }
        if (nfiles == 0)
            win_text(w, 8, ROW_Y0, "(pusto)", C_WHITE);

        /* Panel podgladu zaznaczonego pliku */
        win_rect(w, 0, 86, 180, 1, C_WHITE);
        if (selected[0]) {
            win_text(w, 4, 90, "tresc:", C_GREEN);
            win_text(w, 4, 102, preview, C_WHITE);
        }

        sys_sleep(120);
    }
}
