/* gappear - how the desktop looks, picked by looking at it (ring 3)
 *
 * The theme, the background, how windows move and how large the desktop
 * is drawn are four settings,
 * and the settings window shows them as words: 'dark', '0', '1'. A
 * look is a thing to choose by seeing it, so here each choice is a
 * picture of itself: the two themes as a small desktop each, with a
 * window on it in that theme's colours, the backgrounds as the
 * backgrounds themselves - drawn by the kernel, the very pixels the
 * desktop would draw, only smaller - the three ways of moving, and the
 * scale: on a panel with many pixels to the inch the desktop is laid
 * out larger and drawn at the panel's own size, and Auto lets the panel
 * decide.
 *
 * A click chooses, and the desktop follows at once. Changing a setting
 * needs admin, so the first change asks for the password - once, and
 * the right is given back as soon as that change is written. One
 * program for both kernels (uapi.h).
 */

#include "udraw.h"

#define W0 400
#define H0 368
#define PAD 14

/* The two themes, side by side */
#define CARD_Y  52
#define CARD_W  178
#define CARD_H  96
#define CARD_GAP 16

/* The backgrounds: a row of swatches */
#define BG_N    7
#define SW_Y    192
#define SW_W    46
#define SW_H    32
#define SW_GAP  8

/* How windows move */
#define MO_Y    262
#define MO_W    74
#define MO_H    24

/* How large the desktop is drawn */
#define SC_N    5
#define SC_Y    314
#define SC_W    66
#define SC_H    24

#define NOTE_DY 12            /* the last line's baseline, from the bottom */

static const char *const bg_names[BG_N] = {
    "Aurora", "Navy", "Violet", "Green", "Brown", "Grey", "Sea"
};
static const char *const motions[3] = { "Still", "Quick", "Slow" };
static const char *const motion_values[3] = { "0", "1", "3" };
static const char *const scales[SC_N] = { "Auto", "100%", "125%", "150%",
                                          "200%" };
static const char *const scale_values[SC_N] = { "0", "100", "125", "150",
                                                "200" };

static uint32_t card_px[2][CARD_W * CARD_H];
static uint32_t sw_px[BG_N][SW_W * SW_H];
static int sw_dark = -1;          /* which theme the swatches were drawn in */

static int asking;                /* the password is being typed */
static char pass[32];
static int plen;
static const char *want_name, *want_value;   /* the change waiting for it */
/* 1 saved, 2 changed but there is no disk to keep it on, -1 not changed,
   0 nothing to say */
static int said;

static int card_x(int i) { return PAD + i * (CARD_W + CARD_GAP); }
static int sw_x(int i) { return PAD + i * (SW_W + SW_GAP); }
static int mo_x(int i) { return PAD + i * (MO_W + 8); }
static int sc_x(int i) { return PAD + i * (SC_W + 8); }

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static uint32_t to_num(const char *s)
{
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (uint32_t)(*s++ - '0');
    return v;
}

/* A setting's value, by name - "" when there is none */
static struct cfg_entry entry;
static const char *setting(const char *name)
{
    for (uint32_t i = 0; sys_cfg_get(i, &entry) == 0; i++)
        if (same(entry.name, name))
            return entry.value;
    return "";
}

/* A picture into the window, as many rows at a time as one call takes */
static void blit(int win, int x, int y, int w, int h, const uint32_t *px)
{
    int rows = 1024 / w;
    for (int j = 0; j < h; j += rows) {
        int n = h - j < rows ? h - j : rows;
        win_blit32(win, x, y + j, w, n, px + j * w);
    }
}

/* A picture with round corners: the corners drawn over in the
   window's background, then the edge softened with a ring */
static void picture(int win, int x, int y, int w, int h, const uint32_t *px,
                    const struct ud_look *l, int chosen)
{
    blit(win, x, y, w, h, px);
    for (int k = 0; k < 3; k++)
        ud_ring(win, x - k, y - k, w + 2 * k, h + 2 * k, 8 + k, l->bg);
    if (chosen) {
        ud_ring(win, x - 3, y - 3, w + 6, h + 6, 10, l->accent);
        ud_ring(win, x - 2, y - 2, w + 4, h + 4, 9, l->accent);
    } else {
        ud_ring(win, x, y, w, h, 8, l->line);
    }
}

/* A theme, shown: its background with one of its windows on it */
static void draw_card(int win, int i, const struct ud_look *l, int chosen)
{
    struct ud_look t;
    ud_look_of(i == 0 ? UD_DARK : UD_LIGHT, &t);
    int x = card_x(i), y = CARD_Y;
    picture(win, x, y, CARD_W, CARD_H, card_px[i], l, chosen);

    int wx = x + 28, wy = y + 18, ww = CARD_W - 56, wh = CARD_H - 30;
    ud_round(win, wx, wy, ww, wh, 7, t.bg);
    ud_round(win, wx, wy, ww, 14, 7, t.row);
    ud_fill(win, wx, wy + 8, ww, 6, t.row);
    ud_disc(win, wx + 6, wy + 4, 6, 0xFF5F57);
    ud_disc(win, wx + 15, wy + 4, 6, 0xFEBC2E);
    ud_disc(win, wx + 24, wy + 4, 6, 0x28C840);
    ud_round(win, wx + 8, wy + 22, ww - 40, 8, 4, t.text);
    ud_round(win, wx + 8, wy + 36, ww - 60, 8, 4, t.dim);
    ud_round(win, wx + ww - 42, wy + wh - 18, 34, 12, 6, t.accent);

    const char *name = i == 0 ? "Dark" : "Light";
    ud_text_mid(win, x, y + CARD_H + 6, CARD_W, 18, name,
                chosen ? l->text : l->dim, chosen ? UD_BOLD : UD_SANS, UD_PX);
}

static void previews(int dark)
{
    if (sw_dark == dark)
        return;
    sw_dark = dark;
    for (int i = 0; i < BG_N; i++)
        sys_bg_preview(i, dark, SW_W, SW_H, sw_px[i]);
}

static void draw(int win, int cw, int ch)
{
    struct ud_look l;
    ud_look(&l);
    previews((int)l.dark);
    ud_fill(win, 0, 0, cw, ch, l.bg);
    ud_text(win, PAD, 28, "Appearance", l.text, UD_BOLD, 15);

    const char *theme = setting("theme");
    int dark = !same(theme, "light");
    ud_text(win, PAD, CARD_Y - 8, "Theme", l.dim, UD_SANS, 11);
    for (int i = 0; i < 2; i++)
        draw_card(win, i, &l, (i == 0) == dark);

    uint32_t bg = to_num(setting("background"));
    ud_text(win, PAD, SW_Y - 8, "Background", l.dim, UD_SANS, 11);
    for (int i = 0; i < BG_N; i++) {
        picture(win, sw_x(i), SW_Y, SW_W, SW_H, sw_px[i], &l, (uint32_t)i == bg);
        ud_text_mid(win, sw_x(i) - 4, SW_Y + SW_H + 4, SW_W + 8, 14,
                    bg_names[i], (uint32_t)i == bg ? l.text : l.dim, UD_SANS,
                    10);
    }

    uint32_t m = to_num(setting("motion"));
    int mi = m == 0 ? 0 : m == 1 ? 1 : 2;
    ud_text(win, PAD, MO_Y - 8, "Windows move", l.dim, UD_SANS, 11);
    for (int i = 0; i < 3; i++) {
        int on = i == mi;
        ud_round(win, mo_x(i), MO_Y, MO_W, MO_H, MO_H / 2,
                 on ? l.accent : l.row);
        ud_text_mid(win, mo_x(i), MO_Y, MO_W, MO_H, motions[i],
                    on ? l.on_accent : l.text, on ? UD_BOLD : UD_SANS, UD_PX);
    }

    /* The scale: 1 to 4 are the old way of saying 100 to 400 */
    uint32_t sv = to_num(setting("scale"));
    if (sv >= 1 && sv <= 4)
        sv *= 100;
    ud_text(win, PAD, SC_Y - 8, "Scale", l.dim, UD_SANS, 11);
    for (int i = 0; i < SC_N; i++) {
        int on = to_num(scale_values[i]) == sv;
        ud_round(win, sc_x(i), SC_Y, SC_W, SC_H, SC_H / 2,
                 on ? l.accent : l.row);
        ud_text_mid(win, sc_x(i), SC_Y, SC_W, SC_H, scales[i],
                    on ? l.on_accent : l.text, on ? UD_BOLD : UD_SANS, UD_PX);
    }

    int nb = ch - NOTE_DY;
    if (asking) {
        int x = ud_text(win, PAD, nb, "Admin password:", l.dim, UD_SANS,
                        UD_PX) + 8;
        for (int i = 0; i < plen && x < cw - PAD; i++, x += 9)
            ud_disc(win, x, nb - 7, 6, l.text);
        ud_fill(win, x, nb - 10, 1, 12, l.accent);
    } else if (said == 1) {
        ud_text(win, PAD, nb, "Saved", l.good, UD_BOLD, UD_PX);
    } else if (said == 2) {
        ud_text(win, PAD, nb, "Changed - no disk to keep it on", l.good,
                UD_BOLD, UD_PX);
    } else if (said < 0) {
        ud_text(win, PAD, nb, "NOT changed (wrong password)", l.danger,
                UD_BOLD, UD_PX);
    } else {
        ud_text(win, PAD, nb, "A click chooses; the desktop follows at once",
                l.dim, UD_SANS, 11);
    }
}

/* What a write came to. A setting that cannot be written to the disk is
   still changed for as long as the machine runs - which is worth telling
   apart from not changed at all. */
static int outcome(int32_t rc, const char *name, const char *value)
{
    if (rc == 0)
        return 1;
    return same(setting(name), value) ? 2 : -1;
}

static void apply(const char *name, const char *value)
{
    int32_t rc = sys_cfg_set(name, value);
    if (rc == -2) {               /* allowed for admin only: ask */
        want_name = name;
        want_value = value;
        asking = 1;
        plen = 0;
        said = 0;
        return;
    }
    said = outcome(rc, name, value);
}

static void try_password(void)
{
    pass[plen] = 0;
    asking = 0;
    if (!plen || sys_auth(pass) != 0) {
        said = plen ? -1 : 0;
    } else {
        said = outcome(sys_cfg_set(want_name, want_value), want_name,
                       want_value);
        sys_drop();
    }
    for (int i = 0; i < plen; i++)
        pass[i] = 0;
    plen = 0;
}

static void click(int x, int y)
{
    static const char *const bg_values[BG_N] = { "0", "1", "2", "3", "4",
                                                 "5", "6" };
    for (int i = 0; i < 2; i++)
        if (x >= card_x(i) && x < card_x(i) + CARD_W && y >= CARD_Y &&
            y < CARD_Y + CARD_H + 24) {
            apply("theme", i == 0 ? "dark" : "light");
            return;
        }
    for (int i = 0; i < BG_N; i++)
        if (x >= sw_x(i) && x < sw_x(i) + SW_W && y >= SW_Y &&
            y < SW_Y + SW_H + 18) {
            apply("background", bg_values[i]);
            return;
        }
    for (int i = 0; i < 3; i++)
        if (x >= mo_x(i) && x < mo_x(i) + MO_W && y >= MO_Y &&
            y < MO_Y + MO_H) {
            apply("motion", motion_values[i]);
            return;
        }
    for (int i = 0; i < SC_N; i++)
        if (x >= sc_x(i) && x < sc_x(i) + SC_W && y >= SC_Y &&
            y < SC_Y + SC_H) {
            apply("scale", scale_values[i]);
            return;
        }
}

void _start(void)
{
    int win = (int)win_create(W0, H0, "Appearance");
    if (win < 0)
        sys_exit(1);
    int cw = W0, ch = H0;
    for (int i = 0; i < 2; i++)
        sys_bg_preview(0, i == 0, CARD_W, CARD_H, card_px[i]);
    draw(win, cw, ch);

    struct ud_look was;
    ud_look(&was);
    for (;;) {
        int again = 0;
        struct win_event ev;
        while (win_poll(win, &ev) == 1) {
            if (ev.type == WEV_CLOSE)
                sys_exit(0);
            if (ev.type == WEV_SIZE) {
                cw = ev.x;
                ch = ev.y;
            } else if (ev.type == WEV_CLICK && !asking) {
                click(ev.x, ev.y);
            } else if (ev.type == WEV_KEY && asking) {
                char c = (char)ev.val;
                if (c == '\n' || c == '\r')
                    try_password();
                else if (c == '\b') {
                    if (plen > 0)
                        pass[--plen] = 0;
                } else if (c == 27) {
                    asking = 0;
                    plen = 0;
                } else if (c >= 0x20 && c < 0x7F &&
                           plen < (int)sizeof(pass) - 1) {
                    pass[plen++] = c;
                }
            }
            again = 1;
        }
        /* The theme may change from elsewhere too - the settings, the
           shell - and the window follows it */
        struct ud_look now;
        ud_look(&now);
        if (now.dark != was.dark || now.bg != was.bg) {
            was = now;
            again = 1;
        }
        if (again)
            draw(win, cw, ch);
        sys_sleep(100);
    }
}
