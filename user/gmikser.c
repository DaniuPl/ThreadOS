/* gmikser - how loud, in a window (ring 3)
 *
 * A knob and a switch, and that is the whole of it. What makes it
 * worth writing rather than leaving to a command is that loudness is
 * the one setting a person changes WHILE listening: typing a number,
 * pressing Enter and hearing the result a second later is a different
 * thing from dragging a slider and hearing it move.
 *
 * The program holds no sound. It asks the kernel how loud it is, tells
 * the kernel how loud to be, and asks for a tone when somebody wants
 * to hear what they just chose. Deciding what the percentage is a
 * percentage OF is the driver's business - a codec's amplifier has as
 * many steps as it has, and this window would be lying if it invented
 * a scale of its own.
 *
 * The steps are decibels, so the slider is not linear in loudness and
 * cannot be. Half way along is much quieter than half as loud, and
 * that is how every mixer feels, because that is how hearing works.
 *
 * Nothing here depends on the processor mode: one source, both
 * kernels.
 */

#include "uapi.h"

/* Dark text on a light surface, light text on a dark one. */
#define C_BLACK 0
#define C_RED   1
#define C_BLUE  3
#define C_WHITE 4
#define C_BG    8
#define C_DIM  15

#define C_TEXT  C_BLACK
#define C_HEAD  C_BLUE

#define W0 360
#define H0 128

#define ROW_H  12
#define ROW_Y0 4
static int rowy(int r) { return ROW_Y0 + r * ROW_H; }

/* The slider: a groove with a block on it. Wide enough that a
   percentage is a pixel, so the number under the pointer is the
   number that will be set. */
#define BAR_X  8
#define BAR_Y  52
#define BAR_H  16
static int bar_w(void);

#define BTN_W  84
#define BTN_H  16
#define BTN_Y  82

static struct snd_info snd;
static int cw = W0, ch = H0;
static char note[40];

static int bar_w(void) { return cw - 2 * BAR_X; }

static void ecopy(char *d, const char *s, int max)
{
    int i = 0;
    while (s[i] && i < max - 1) {
        d[i] = s[i];
        i++;
    }
    d[i] = '\0';
}

static int num(char *d, uint32_t v)
{
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 11);
    for (int i = 0; i < n; i++)
        d[i] = tmp[n - 1 - i];
    d[n] = '\0';
    return n;
}

static int add(char *d, int at, const char *s)
{
    int i = 0;
    while (s[i])
        d[at++] = s[i++];
    d[at] = '\0';
    return at;
}

static void refresh(void)
{
    sys_snd_get(&snd);
}

static void draw(int win)
{
    char line[64];
    int at;

    win_rect(win, 0, 0, cw, ch, C_BG);

    if (!snd.present) {
        win_text(win, 6, rowy(0), "Dzwiek", C_HEAD);
        win_text(win, 6, rowy(2), "Ta maszyna nie ma karty dzwiekowej,",
                 C_TEXT);
        win_text(win, 6, rowy(3), "ktora umiem obsluzyc.", C_TEXT);
        win_text(win, 6, rowy(5), "Wiecej powie 'sprzet'.", C_DIM);
        return;
    }

    at = add(line, 0, "Glosnosc: ");
    at += num(line + at, snd.volume);
    at = add(line, at, "%");
    if (snd.muted)
        at = add(line, at, "  (cisza)");
    win_text(win, 6, rowy(0), line, snd.muted ? C_RED : C_HEAD);

    at = add(line, 0, "kodek ma ");
    at += num(line + at, snd.steps);
    at = add(line, at, " stopni, a stopien to decybele");
    win_text(win, 6, rowy(1), line, C_DIM);
    win_text(win, 6, rowy(2), "- wiec polowa suwaka to nie polowa",
             C_DIM);
    win_text(win, 6, rowy(3), "glosnosci. Kolko myszy tez dziala.", C_DIM);

    /* The groove, and how much of it is filled */
    int w = bar_w();
    int fill = (int)snd.volume * w / 100;
    win_rect(win, BAR_X, BAR_Y, w, BAR_H, C_BLACK);
    if (!snd.muted && fill > 0)
        win_rect(win, BAR_X, BAR_Y, fill, BAR_H, C_HEAD);
    /* And a line where it ends, so the edge is visible even at the
       ends of its travel */
    if (fill > 0 && fill < w)
        win_rect(win, BAR_X + fill - 1, BAR_Y, 1, BAR_H, C_WHITE);

    win_rect(win, BAR_X, BTN_Y, BTN_W, BTN_H, snd.muted ? C_RED : C_BLUE);
    win_text(win, BAR_X + 8, BTN_Y + 4,
             snd.muted ? "Wlacz" : "Cisza", C_WHITE);
    win_rect(win, BAR_X + BTN_W + 10, BTN_Y, BTN_W, BTN_H, C_BLUE);
    win_text(win, BAR_X + BTN_W + 10 + 12, BTN_Y + 4, "Sprawdz", C_WHITE);

    if (note[0])
        win_text(win, BAR_X + 2 * BTN_W + 26, BTN_Y + 4, note, C_TEXT);
}

void _start(void)
{
    int win = (int)win_create(W0, H0, "Glosnosc");
    if (win < 0)
        sys_exit(1);

    refresh();
    draw(win);

    for (;;) {
        struct win_event ev;
        int again = 0;

        while (win_poll(win, &ev) == 1) {
            again = 1;
            if (ev.type == WEV_CLOSE)
                sys_exit(0);
            if (ev.type == WEV_SIZE) {
                cw = ev.x;
                ch = ev.y;
                continue;
            }
            if (ev.type == WEV_SCROLL) {
                /* The wheel over the window, which is what a person
                   reaches for first - two fingers on a touchpad. */
                int v = (int)snd.volume + ev.val * 5;
                if (v < 0)
                    v = 0;
                if (v > 100)
                    v = 100;
                sys_snd_set(v, 0);
                refresh();
                note[0] = '\0';
                continue;
            }
            if (ev.type == WEV_CLICK && snd.present) {
                if (ev.y >= BAR_Y && ev.y < BAR_Y + BAR_H &&
                    ev.x >= BAR_X && ev.x < BAR_X + bar_w()) {
                    /* Divided by one less than the width, so that the
                       last pixel of the groove really is a hundred
                       per cent: a slider one cannot drag to the end
                       is a slider that feels broken. */
                    int v = (ev.x - BAR_X) * 100 / (bar_w() - 1);
                    if (v > 100)
                        v = 100;
                    sys_snd_set(v, 0);
                    refresh();
                    note[0] = '\0';
                } else if (ev.y >= BTN_Y && ev.y < BTN_Y + BTN_H) {
                    if (ev.x >= BAR_X && ev.x < BAR_X + BTN_W) {
                        sys_snd_set(-1, snd.muted ? 0 : 1);
                        refresh();
                        note[0] = '\0';
                    } else if (ev.x >= BAR_X + BTN_W + 10 &&
                               ev.x < BAR_X + 2 * BTN_W + 10) {
                        /* A tone at the loudness just chosen - which
                           is the only way to know whether it is the
                           right one. The call plays and returns, so
                           the window is still while it sounds; that
                           is honest, because the machine really is
                           busy playing. */
                        ecopy(note, "gra...", sizeof(note));
                        draw(win);
                        sys_snd_beep(440, 200);
                        ecopy(note, "", sizeof(note));
                        refresh();
                    }
                }
                continue;
            }
            if (ev.type == WEV_KEY) {
                char c = (char)ev.val;
                int v = (int)snd.volume;
                if (c == '+' || c == '=')
                    v += 5;
                else if (c == '-')
                    v -= 5;
                else if (c == 'm')
                    sys_snd_set(-1, snd.muted ? 0 : 1);
                else
                    continue;
                if (v < 0)
                    v = 0;
                if (v > 100)
                    v = 100;
                if (c != 'm')
                    sys_snd_set(v, 0);
                refresh();
            }
        }

        if (again)
            draw(win);
        sys_sleep(60);
    }
}
