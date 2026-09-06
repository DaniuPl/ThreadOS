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
 * The window itself is a LIST OF THINGS now (user/ugui.h): a label, a
 * slider, two buttons. What was arithmetic in the event loop - is this
 * click inside the first button or the second - belongs to that layer,
 * and the same rewrite takes the same measurements in the test, which
 * is the only way to tell a framework that works from one that merely
 * compiles.
 *
 * Nothing here depends on the processor mode: one source, both
 * kernels.
 */

#include "uapi.h"
#include "ugui.h"

#define W0 360
#define H0 128

#define ROW_Y0 4
static int rowy(int r) { return ROW_Y0 + r * UG_ROW; }

#define BAR_X  8
#define BAR_Y  52
#define BAR_H  16

#define BTN_W  84
#define BTN_H  16
#define BTN_Y  82

static struct snd_info snd;
static struct ug u;
static int id_state, id_steps, id_bar, id_mute, id_test, id_note;
static char state_line[64];
static char steps_line[64];
static char note[40];

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

static int bar_w(void)
{
    return u.w - 2 * BAR_X;
}

/* What the window says about itself, rebuilt whenever the numbers
   change. The layer holds the pointers; the text is ours. */
static void relabel(void)
{
    int at = add(state_line, 0, "Volume: ");

    at += num(state_line + at, snd.volume);
    at = add(state_line, at, "%");
    if (snd.muted)
        at = add(state_line, at, "  (muted)");
    ug_set_colour(&u, id_state, snd.muted ? UG_RED : UG_BLUE);

    at = add(steps_line, 0, "codec steps: ");
    at += num(steps_line + at, snd.steps);
    add(steps_line, at, ", each of them a decibel");

    ug_set_value(&u, id_bar, snd.muted ? 0 : (int)snd.volume);
    ug_set_text(&u, id_mute, snd.muted ? "Unmute" : "Mute");
    ug_set_colour(&u, id_mute, snd.muted ? UG_RED : UG_BLUE);
    ug_set_text(&u, id_note, note);
}

static void refresh(void)
{
    sys_snd_get(&snd);
    relabel();
}

static void build(int win)
{
    ug_start(&u, win, W0, H0);

    if (!snd.present) {
        ug_label(&u, 6, rowy(0), "Sound");
        ug_set_colour(&u, 0, UG_BLUE);
        ug_label(&u, 6, rowy(2), "This machine has no sound card");
        ug_label(&u, 6, rowy(3), "that I know how to drive.");
        ug_note(&u, 6, rowy(5), "'hardware' says more.");
        id_state = id_steps = id_bar = id_mute = id_test = id_note = -1;
        return;
    }

    id_state = ug_label(&u, 6, rowy(0), state_line);
    id_steps = ug_note(&u, 6, rowy(1), steps_line);
    ug_note(&u, 6, rowy(2), "- so half the slider is not half the");
    ug_note(&u, 6, rowy(3), "loudness. The mouse wheel works too.");

    id_bar = ug_slider(&u, BAR_X, BAR_Y, bar_w(), BAR_H,
                       (int)snd.volume, 100);
    id_mute = ug_button(&u, BAR_X, BTN_Y, BTN_W, BTN_H, "Mute");
    id_test = ug_button(&u, BAR_X + BTN_W + 10, BTN_Y, BTN_W, BTN_H,
                        "Test");
    id_note = ug_label(&u, BAR_X + 2 * BTN_W + 26, BTN_Y + 4, note);

    relabel();
}

void _start(void)
{
    int win = (int)win_create(W0, H0, "Volume");
    if (win < 0)
        sys_exit(1);

    note[0] = '\0';
    sys_snd_get(&snd);
    build(win);
    ug_draw(&u);

    for (;;) {
        struct win_event ev;
        int again = 0;

        while (win_poll(win, &ev) == 1) {
            int which = -1;
            int what = ug_event(&u, &ev, &which);

            again = 1;
            if (what == UG_CLOSED)
                sys_exit(0);
            if (what == UG_RESIZED) {
                /* The groove follows the window; everything else stays
                   where it was put. */
                u.it[id_bar >= 0 ? id_bar : 0].w = bar_w();
                continue;
            }
            if (!snd.present)
                continue;

            if (what == UG_CHANGED && which == id_bar) {
                sys_snd_set(ug_value(&u, id_bar), 0);
                note[0] = '\0';
                refresh();
                continue;
            }
            if (what == UG_PRESSED && which == id_mute) {
                sys_snd_set(-1, snd.muted ? 0 : 1);
                note[0] = '\0';
                refresh();
                continue;
            }
            if (what == UG_PRESSED && which == id_test) {
                /* A tone at the loudness just chosen - which is the
                   only way to know whether it is the right one. The
                   call plays and returns, so the window is still while
                   it sounds; that is honest, because the machine
                   really is busy playing. */
                ecopy(note, "playing...", sizeof(note));
                relabel();
                ug_draw(&u);
                sys_snd_beep(440, 200);
                note[0] = '\0';
                refresh();
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
            ug_draw(&u);
        sys_sleep(60);
    }
}
