/* gsettings - the system's settings, a section at a time (ring 3)
 *
 * What was here before was the settings file drawn as a table: a row
 * per name, its value as text, a click to step a number and Enter to
 * write. Every setting was there and none of them was easy to find -
 * 'background 3' says nothing to a person who wants the green one.
 *
 * So the window is split the way settings are split everywhere else:
 * sections down the left, and on the right the one that is open, with
 * each setting shown as what it is. A choice out of a few is a row of
 * buttons with the current one lit; a colour is a swatch; something
 * on or off is a box to tick; text is a field. The figures the system
 * keeps about itself (the network, the memory, the version) are shown
 * in the same window, under About and Network, because that is where
 * people look for them.
 *
 * The program holds none of the values. It asks the kernel for them
 * (sys_cfg_get) every time it draws and writes a change the moment it
 * is made (sys_cfg_set), so the desktop follows at once and what is
 * shown is what the kernel has. Changing a setting needs admin: the
 * first change asks for the password, in the window, and the right is
 * given back as soon as that one change is written.
 *
 * One program for both kernels (uapi.h).
 */

#include "udraw.h"

/* Small enough to open in the cascade's third place on a 640x480
   screen, between the bar and the dock */
#define W0 500
#define H0 340

#define SIDE_W 150            /* the sections, down the left */
#define SIDE_Y0 48            /* the first section's top */
#define SIDE_H 30
#define PANE_X 170            /* the open section, right of them */
#define HEAD_BASE 32          /* its name, in bold */
#define PANE_Y0 52            /* the first setting's top */
#define ROW_H 54              /* a setting: its name, then its control */
#define CTRL_DY 22            /* the control, below the name */
#define CHIP_W 58             /* a button in a row of choices */
#define CHIP_H 24
#define CHIP_GAP 6
#define FIELD_W 200
#define SAVE_W 70
#define SW_W 38               /* a background, as a swatch */
#define SW_H 26
#define SW_GAP 7
#define DOT 16                /* a colour of the prompt */
#define DOT_GAP 4
#define SLIDER_W 220
#define INFO_H 24             /* a line of figures: a name and a value */
#define INFO_X 120            /* the value, right of the name */
#define NOTE_DY 14            /* the last line's baseline, from the bottom */
#define PASS_W 300            /* the password panel */
#define PASS_H 96

/* The clipboard's keys, Ctrl+Insert and Shift+Insert, as the kernel
   delivers them (kernel/include/keyboard.h) */
#define KEY_COPY  0x1A
#define KEY_PASTE 0x1B

#define SEC_GENERAL 0
#define SEC_LOOK    1
#define SEC_DISPLAY 2
#define SEC_SOUND   3
#define SEC_NET     4
#define SEC_FILES   5
#define SEC_ABOUT   6
#define SEC_N       7

static const char *const sections[SEC_N] = {
    "General", "Appearance", "Display", "Sound", "Network", "Files", "About"
};

static int win = -1;
static int cw = W0, ch = H0;
static int section;
static struct ud_look l;

/* --- the settings, as the kernel has them ---------------------------- */

#define MAXCFG 16
static struct cfg_entry cfg[MAXCFG];
static int ncfg;

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void copy(char *d, const char *s, int max)
{
    int i = 0;
    while (s[i] && i < max - 1) {
        d[i] = s[i];
        i++;
    }
    d[i] = 0;
}

static int len(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static void load(void)
{
    ncfg = 0;
    while (ncfg < MAXCFG && sys_cfg_get((uint32_t)ncfg, &cfg[ncfg]) == 0)
        ncfg++;
}

static const char *setting(const char *name)
{
    for (int i = 0; i < ncfg; i++)
        if (same(cfg[i].name, name))
            return cfg[i].value;
    return "";
}

/* A number into text, appended where 'at' says; returns the new end */
static int put_num(char *out, int at, uint32_t v)
{
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        out[at++] = tmp[--n];
    out[at] = 0;
    return at;
}

static int put_str(char *out, int at, const char *s)
{
    while (*s)
        out[at++] = *s++;
    out[at] = 0;
    return at;
}

static int put_ip(char *out, int at, uint32_t ip)
{
    for (int i = 3; i >= 0; i--) {
        at = put_num(out, at, (ip >> (8 * i)) & 0xFF);
        if (i)
            out[at++] = '.';
    }
    out[at] = 0;
    return at;
}

/* --- changing one ----------------------------------------------------- */

/* The password is asked for in the window; the change it is for waits
   here, copied, because the value may be a field that is still being
   typed into. */
static int asking;
static char pass[32];
static int plen;
static char want_name[CFG_NAME_MAX], want_value[CFG_VAL_MAX];
/* 1 saved, 2 changed but there is no disk to keep it on, -1 refused
   (a wrong password), -2 refused (not a value it takes), 0 nothing */
static int said;

static int outcome(int32_t rc, const char *name, const char *value)
{
    if (rc == 0)
        return 1;
    load();
    if (same(setting(name), value))
        return 2;
    return -2;
}

static void apply(const char *name, const char *value)
{
    int32_t rc = sys_cfg_set(name, value);
    if (rc == -2) {               /* allowed for admin only: ask */
        copy(want_name, name, sizeof(want_name));
        copy(want_value, value, sizeof(want_value));
        asking = 1;
        plen = 0;
        said = 0;
        return;
    }
    said = outcome(rc, name, value);
    load();
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
    load();
}

/* --- the fields -------------------------------------------------------- */

#define F_NAME 0
#define F_RES  1
#define F_DNS  2
#define F_N    3
static const char *const field_setting[F_N] = { "name", "resolution", "dns" };
static char fields[F_N][CFG_VAL_MAX];
static int focus = -1;            /* the field that has the keys */

/* A field shows the setting until it is typed into */
static void fields_from_settings(void)
{
    for (int i = 0; i < F_N; i++)
        if (i != focus)
            copy(fields[i], setting(field_setting[i]), CFG_VAL_MAX);
}

/* --- where a click lands --------------------------------------------- */

#define A_SECTION 1
#define A_SET     2               /* name = value */
#define A_FOCUS   3               /* a field */
#define A_SAVE    4               /* a field's Save */
#define A_VOLUME  5
#define A_MUTE    6
#define A_BEEP    7
#define A_TOGGLE  8               /* name flips between 0 and 1 */

struct hot {
    int x, y, w, h;
    int act, arg;
    const char *name, *value;
};

#define MAXHOT 64
static struct hot hots[MAXHOT];
static int nhot;

static void hot(int x, int y, int w, int h, int act, int arg,
                const char *name, const char *value)
{
    if (nhot >= MAXHOT)
        return;
    struct hot *t = &hots[nhot++];
    t->x = x;
    t->y = y;
    t->w = w;
    t->h = h;
    t->act = act;
    t->arg = arg;
    t->name = name;
    t->value = value;
}

/* --- drawing pieces ---------------------------------------------------- */

/* A setting's name, at the top of its row */
static void row_name(int y, const char *text)
{
    ud_text(win, PANE_X, ud_baseline(y, 18, UD_PX), text, l.text, UD_SANS,
            UD_PX);
}

/* One choice out of a few: lit when it is the one in force */
static void chip(int x, int y, const char *text, int on, const char *name,
                 const char *value)
{
    ud_round(win, x, y, CHIP_W, CHIP_H, 8, on ? l.accent : l.row);
    ud_text_mid(win, x, y, CHIP_W, CHIP_H, text, on ? l.on_accent : l.text,
                UD_SANS, UD_PX);
    hot(x, y, CHIP_W, CHIP_H, A_SET, 0, name, value);
}

static void chips(int y, int n, const char *const *texts,
                  const char *const *values, const char *name)
{
    const char *now = setting(name);
    for (int i = 0; i < n; i++)
        chip(PANE_X + i * (CHIP_W + CHIP_GAP), y, texts[i],
             same(now, values[i]), name, values[i]);
}

static void button(int x, int y, int w, const char *text, int act, int arg)
{
    ud_round(win, x, y, w, CHIP_H, 8, l.accent);
    ud_text_mid(win, x, y, w, CHIP_H, text, l.on_accent, UD_SANS, UD_PX);
    hot(x, y, w, CHIP_H, act, arg, (const char *)0, (const char *)0);
}

static void field(int y, int f)
{
    int x = PANE_X;
    ud_round(win, x, y, FIELD_W, CHIP_H, 6, l.field);
    ud_ring(win, x, y, FIELD_W, CHIP_H, 6, focus == f ? l.accent : l.line);
    int end = ud_text(win, x + 8, ud_baseline(y, CHIP_H, UD_PX), fields[f],
                      l.text, UD_SANS, UD_PX);
    if (focus == f)
        ud_fill(win, end + 1, y + 5, 1, CHIP_H - 10, l.text);
    hot(x, y, FIELD_W, CHIP_H, A_FOCUS, f, (const char *)0, (const char *)0);
    button(x + FIELD_W + 8, y, SAVE_W, "Save", A_SAVE, f);
}

static void check_box(int y, const char *text, int on, const char *name)
{
    int b = 16, by = y + (CHIP_H - b) / 2;
    if (on) {
        ud_round(win, PANE_X, by, b, b, 4, l.accent);
        ud_line(win, PANE_X + 4, by + 8, PANE_X + 7, by + 11, 28, l.on_accent);
        ud_line(win, PANE_X + 7, by + 11, PANE_X + 12, by + 5, 28,
                l.on_accent);
    } else {
        ud_round(win, PANE_X, by, b, b, 4, l.field);
        ud_ring(win, PANE_X, by, b, b, 4, l.line);
    }
    int end = ud_text(win, PANE_X + b + 8, ud_baseline(y, CHIP_H, UD_PX), text,
                      l.text, UD_SANS, UD_PX);
    hot(PANE_X, y, end - PANE_X, CHIP_H, A_TOGGLE, 0, name, (const char *)0);
}

static void note(int y, const char *text)
{
    ud_text(win, PANE_X, ud_baseline(y, 16, 11), text, l.dim, UD_SANS, 11);
}

/* A line of figures: what it is, and its value */
static void info(int i, const char *name, const char *value)
{
    int y = PANE_Y0 + i * INFO_H;
    int base = ud_baseline(y, INFO_H, UD_PX);
    ud_text(win, PANE_X, base, name, l.dim, UD_SANS, UD_PX);
    ud_text(win, PANE_X + INFO_X, base, value, l.text, UD_SANS, UD_PX);
}

/* --- the sections ------------------------------------------------------ */

/* The prompt's sixteen colours, the VGA text palette the shell uses */
static const uint32_t vga[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500,
    0xAAAAAA, 0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF,
    0xFFFF55, 0xFFFFFF
};
static const char *const numbers[16] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12",
    "13", "14", "15"
};

static void draw_general(void)
{
    static const char *const motions[3] = { "Still", "Quick", "Slow" };
    static const char *const motion_values[3] = { "0", "1", "3" };

    int y = PANE_Y0;
    row_name(y, "Computer name");
    field(y + CTRL_DY, F_NAME);

    y += ROW_H;
    row_name(y, "Windows opening and closing");
    chips(y + CTRL_DY, 3, motions, motion_values, "motion");

    y += ROW_H;
    row_name(y, "Colour of the shell prompt");
    const char *now = setting("color");
    for (int i = 0; i < 16; i++) {
        int x = PANE_X + i * (DOT + DOT_GAP), dy = y + CTRL_DY + 3;
        if (same(now, numbers[i]))
            ud_round(win, x - 3, dy - 3, DOT + 6, DOT + 6, 7, l.accent);
        ud_round(win, x, dy, DOT, DOT, 5, vga[i]);
        ud_ring(win, x, dy, DOT, DOT, 5, l.line);
        hot(x, dy, DOT, DOT, A_SET, 0, "color", numbers[i]);
    }
}

/* The backgrounds as the desktop draws them - the kernel's own pixels,
   only smaller. Drawn again when the theme changes, since the aurora
   is not the same in the two. */
#define BG_N 7
static uint32_t sw_px[BG_N][SW_W * SW_H];
static int sw_dark = -1;

static void blit(int x, int y, int w, int h, const uint32_t *px)
{
    int rows = 1024 / w;
    for (int j = 0; j < h; j += rows) {
        int n = h - j < rows ? h - j : rows;
        win_blit32(win, x, y + j, w, n, px + j * w);
    }
}

static void draw_look(void)
{
    static const char *const themes[2] = { "Dark", "Light" };
    static const char *const theme_values[2] = { "dark", "light" };
    static const char *const docks[3] = { "Small", "Medium", "Large" };
    static const char *const dock_values[3] = { "0", "1", "2" };

    int y = PANE_Y0;
    row_name(y, "Theme");
    chips(y + CTRL_DY, 2, themes, theme_values, "theme");

    y += ROW_H;
    row_name(y, "Background");
    if (sw_dark != (int)l.dark) {
        for (int i = 0; i < BG_N; i++)
            sys_bg_preview((uint32_t)i, l.dark, SW_W, SW_H, sw_px[i]);
        sw_dark = (int)l.dark;
    }
    const char *now = setting("background");
    for (int i = 0; i < BG_N; i++) {
        int x = PANE_X + i * (SW_W + SW_GAP), sy = y + CTRL_DY;
        blit(x, sy, SW_W, SW_H, sw_px[i]);
        for (int k = 0; k < 3; k++)
            ud_ring(win, x - k, sy - k, SW_W + 2 * k, SW_H + 2 * k, 6 + k,
                    l.bg);
        if (same(now, numbers[i]))
            ud_ring(win, x - 2, sy - 2, SW_W + 4, SW_H + 4, 8, l.accent);
        hot(x, sy, SW_W, SW_H, A_SET, 0, "background", numbers[i]);
    }

    y += ROW_H;
    row_name(y, "Size of the dock");
    chips(y + CTRL_DY, 3, docks, dock_values, "dock");

    note(y + ROW_H, "The Appearance program shows the themes and the");
    note(y + ROW_H + 14, "wallpaper at full size.");
}

static void draw_display(void)
{
    static const char *const scales[5] = { "Auto", "100%", "125%", "150%",
                                           "200%" };
    static const char *const scale_values[5] = { "0", "100", "125", "150",
                                                 "200" };
    int y = PANE_Y0;
    row_name(y, "Scale");
    chips(y + CTRL_DY, 5, scales, scale_values, "scale");

    y += ROW_H;
    row_name(y, "Resolution");
    field(y + CTRL_DY, F_RES);

    y += ROW_H;
    note(y, "Auto lets the panel decide from its pixels per inch.");
    note(y + 14, "'native' is the panel's own mode; or one like 1280x800.");
}

static struct snd_info snd;

static void draw_sound(void)
{
    sys_snd_get(&snd);
    int y = PANE_Y0;
    if (!snd.present) {
        row_name(y, "No sound card was found in this machine.");
        note(y + 22, "The volume is kept for when there is one.");
        y += ROW_H;
    }
    row_name(y, "Volume");
    int sy = y + CTRL_DY, knob = 18;
    int fill = (int)snd.volume * (SLIDER_W - knob) / 100;
    ud_round(win, PANE_X, sy + CHIP_H / 2 - 3, SLIDER_W, 6, 3, l.row);
    if (fill > 0)
        ud_round(win, PANE_X, sy + CHIP_H / 2 - 3, fill + knob / 2, 6, 3,
                 snd.muted ? l.dim : l.accent);
    ud_disc(win, PANE_X + fill, sy + (CHIP_H - knob) / 2, knob,
            l.dark ? 0xEDEDF0 : 0xFFFFFF);
    ud_op(win, UD_OP_RING, PANE_X + fill, sy + (CHIP_H - knob) / 2, knob,
          knob, knob / 2, l.line, 0, 0, 0, (const char *)0);
    hot(PANE_X, sy, SLIDER_W, CHIP_H, A_VOLUME, 0, (const char *)0,
        (const char *)0);
    char pc[8];
    put_str(pc, put_num(pc, 0, snd.volume), "%");
    ud_text(win, PANE_X + SLIDER_W + 12, ud_baseline(sy, CHIP_H, UD_PX), pc,
            l.text, UD_SANS, UD_PX);

    y += ROW_H;
    int b = 16, by = y + (CHIP_H - b) / 2;
    if (snd.muted) {
        ud_round(win, PANE_X, by, b, b, 4, l.accent);
        ud_line(win, PANE_X + 4, by + 8, PANE_X + 7, by + 11, 28, l.on_accent);
        ud_line(win, PANE_X + 7, by + 11, PANE_X + 12, by + 5, 28,
                l.on_accent);
    } else {
        ud_round(win, PANE_X, by, b, b, 4, l.field);
        ud_ring(win, PANE_X, by, b, b, 4, l.line);
    }
    int end = ud_text(win, PANE_X + b + 8, ud_baseline(y, CHIP_H, UD_PX),
                      "Mute", l.text, UD_SANS, UD_PX);
    hot(PANE_X, y, end - PANE_X, CHIP_H, A_MUTE, 0, (const char *)0,
        (const char *)0);

    y += ROW_H - 16;
    button(PANE_X, y, 110, "Play a tone", A_BEEP, 0);
}

static void draw_network(void)
{
    struct netinfo ni;
    char t[24];
    if (netinfo(&ni) != 0 || !ni.ip) {
        info(0, "Address", "none yet (DHCP has not answered)");
    } else {
        put_ip(t, 0, ni.ip);
        info(0, "Address", t);
        put_ip(t, 0, ni.mask);
        info(1, "Mask", t);
        put_ip(t, 0, ni.gw);
        info(2, "Gateway", t);
        put_ip(t, 0, ni.dns);
        info(3, "Name server", t);
    }
    int y = PANE_Y0 + 4 * INFO_H + 14;
    row_name(y, "Name server to use instead");
    field(y + CTRL_DY, F_DNS);
    note(y + CTRL_DY + CHIP_H + 4, "0.0.0.0 uses the one DHCP gives");
}

static void draw_files(void)
{
    int y = PANE_Y0;
    row_name(y, "In the file manager");
    check_box(y + CTRL_DY, "Show system files (swap.sys, system.cfg, ...)",
              same(setting("hidden"), "1"), "hidden");
    note(y + CTRL_DY + CHIP_H + 4,
         "and names starting with a dot. They are hidden so that");
    note(y + CTRL_DY + CHIP_H + 18, "nobody throws them away by accident.");
}

static void draw_about(void)
{
    char t[48];
    int i = 0;
#ifdef __x86_64__
    struct sysinfo64 si;
#else
    struct sysinfo si;
#endif
    sys_sysinfo(&si);

    info(i++, "System", "ThreadOS");
#ifdef __x86_64__
    info(i++, "Kernel", "64-bit, long mode");
    /* The release number travels in the initrd (the update program
       reads it from there too); the 32-bit system is not released */
    char v[16];
    int64_t n = sys_initrd_read("version", v, sizeof(v) - 1);
    if (n > 0) {
        v[n] = 0;
        for (int k = 0; v[k]; k++)
            if (v[k] == '\n')
                v[k] = 0;
        info(i++, "Version", v);
    }
#else
    info(i++, "Kernel", "32-bit, protected mode");
#endif
    put_str(t, put_num(t, 0, si.ram_kib), " KiB");
    info(i++, "Memory", t);
    put_str(t, put_num(t, 0, si.used_kib), " KiB");
    info(i++, "In use", t);
    uint32_t s = si.uptime_s;
    int at = 0;
    if (s >= 3600)
        at = put_str(t, put_num(t, 0, s / 3600), " h ");
    put_str(t, put_num(t, at, (s / 60) % 60), " min");
    info(i++, "Running for", t);
    /* Level 0 is admin (kernel/include/user.h) */
    info(i++, "Signed in as", sys_level() == 0 ? "admin" : "user");
}

/* --- the window -------------------------------------------------------- */

static void draw(void)
{
    ud_look(&l);
    load();
    fields_from_settings();
    nhot = 0;

    /* One background for both halves, a line between them: the window
       stays the theme's window colour, which is how other programs and
       the desktop's tests tell a window's body from its title bar */
    ud_fill(win, 0, 0, cw, ch, l.bg);
    ud_fill(win, SIDE_W, 12, 1, ch - 24, l.line);
    ud_text(win, 16, HEAD_BASE, "Settings", l.text, UD_BOLD, 16);
    for (int i = 0; i < SEC_N; i++) {
        int y = SIDE_Y0 + i * SIDE_H;
        if (i == section)
            ud_round(win, 8, y + 2, SIDE_W - 16, SIDE_H - 4, 8, l.accent);
        ud_text(win, 20, ud_baseline(y, SIDE_H, UD_PX), sections[i],
                i == section ? l.on_accent : l.text, UD_SANS, UD_PX);
        hot(0, y, SIDE_W, SIDE_H, A_SECTION, i, (const char *)0,
            (const char *)0);
    }

    ud_text(win, PANE_X, HEAD_BASE, sections[section], l.text, UD_BOLD, 16);
    switch (section) {
    case SEC_GENERAL: draw_general(); break;
    case SEC_LOOK:    draw_look(); break;
    case SEC_DISPLAY: draw_display(); break;
    case SEC_SOUND:   draw_sound(); break;
    case SEC_NET:     draw_network(); break;
    case SEC_FILES:   draw_files(); break;
    default:          draw_about(); break;
    }

    /* What became of the last change - always in the same place, so it
       is not looked for */
    int nb = ch - NOTE_DY;
    if (said == 1)
        ud_text(win, PANE_X, nb, "Saved", l.good, UD_BOLD, UD_PX);
    else if (said == 2)
        ud_text(win, PANE_X, nb, "Changed until the machine stops (no disk)",
                l.dim, UD_BOLD, UD_PX);
    else if (said == -1)
        ud_text(win, PANE_X, nb, "NOT changed (wrong password)", l.danger,
                UD_BOLD, UD_PX);
    else if (said == -2)
        ud_text(win, PANE_X, nb, "NOT changed (not a value it takes)",
                l.danger, UD_BOLD, UD_PX);

    if (asking) {
        /* The password: dots, not what was typed - the person needs to
           see that the keys arrive, and nothing more */
        int px = PANE_X + (cw - PANE_X - PASS_W) / 2 - 10;
        int py = (ch - PASS_H) / 2;
        ud_fill_a(win, 0, 0, cw, ch, 0x000000, l.dark ? 110 : 70);
        ud_round(win, px, py, PASS_W, PASS_H, 10, l.field);
        ud_ring(win, px, py, PASS_W, PASS_H, 10, l.line);
        ud_text(win, px + 14, py + 24, "Changing settings needs admin",
                l.text, UD_BOLD, UD_PX);
        ud_round(win, px + 14, py + 36, PASS_W - 28, CHIP_H, 6, l.bg);
        ud_ring(win, px + 14, py + 36, PASS_W - 28, CHIP_H, 6, l.accent);
        int x = ud_text(win, px + 22, ud_baseline(py + 36, CHIP_H, UD_PX),
                        "Password:", l.dim, UD_SANS, UD_PX) + 8;
        for (int i = 0; i < plen && x < px + PASS_W - 24; i++, x += 9)
            ud_disc(win, x, py + 45, 6, l.text);
        ud_text(win, px + 14, py + PASS_H - 14,
                "Enter confirms, Escape gives up", l.dim, UD_SANS, 11);
    }
}

static void click(int x, int y)
{
    for (int i = nhot - 1; i >= 0; i--) {
        struct hot *t = &hots[i];
        if (x < t->x || x >= t->x + t->w || y < t->y || y >= t->y + t->h)
            continue;
        if (t->act != A_FOCUS)
            focus = -1;
        switch (t->act) {
        case A_SECTION:
            if (section != t->arg)
                said = 0;
            section = t->arg;
            break;
        case A_SET:
            apply(t->name, t->value);
            break;
        case A_TOGGLE:
            apply(t->name, same(setting(t->name), "1") ? "0" : "1");
            break;
        case A_FOCUS:
            focus = t->arg;
            break;
        case A_SAVE:
            apply(field_setting[t->arg], fields[t->arg]);
            break;
        case A_VOLUME: {
            int v = (x - t->x - 9) * 100 / (SLIDER_W - 18);
            sys_snd_set(v < 0 ? 0 : v > 100 ? 100 : v, 0);
            break;
        }
        case A_MUTE:
            sys_snd_set(-1, snd.muted ? 0 : 1);
            break;
        case A_BEEP:
            sys_snd_beep(440, 200);
            break;
        }
        return;
    }
    focus = -1;
}

static void key(char c)
{
    if (asking) {
        /* Everything typed now is the password */
        if (c == '\n' || c == '\r') {
            try_password();
        } else if (c == KEY_PASTE) {       /* Escape, the same code */
            asking = 0;
            plen = 0;
        } else if (c == '\b') {
            if (plen > 0)
                pass[--plen] = 0;
        } else if (c >= 0x20 && c < 0x7F && plen < (int)sizeof(pass) - 1) {
            pass[plen++] = c;
        }
        return;
    }
    if (focus >= 0) {
        char *f = fields[focus];
        int n = len(f);
        if (c == '\n' || c == '\r') {
            int was = focus;
            focus = -1;
            apply(field_setting[was], fields[was]);
        } else if (c == KEY_COPY) {
            if (n)
                sys_clip_set(f, (uint32_t)n);
        } else if (c == KEY_PASTE) {
            char got[CLIP_MAX + 1];
            int k = (int)sys_clip_get(got, sizeof(got));
            for (int i = 0; i < k && n < CFG_VAL_MAX - 1; i++)
                if (got[i] >= 0x20 && got[i] < 0x7F)
                    f[n++] = got[i];
            f[n] = 0;
        } else if (c == '\b') {
            if (n > 0)
                f[n - 1] = 0;
        } else if (c >= 0x20 && c < 0x7F && n < CFG_VAL_MAX - 1) {
            f[n] = c;
            f[n + 1] = 0;
        }
        return;
    }
    /* No field: the arrows walk the sections */
    if (c == (char)KEY_DOWN && section < SEC_N - 1) {
        section++;
        said = 0;
    } else if (c == (char)KEY_UP && section > 0) {
        section--;
        said = 0;
    }
}

void _start(void)
{
    win = (int)win_create(W0, H0, "Settings");
    if (win < 0)
        sys_exit(1);
    load();
    draw();

    int ticks = 0;
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
            } else if (ev.type == WEV_CLICK) {
                if (!asking)
                    click(ev.x, ev.y);
            } else if (ev.type == WEV_KEY) {
                key((char)ev.val);
            }
        }
        /* The figures move by themselves (the uptime, the memory, the
           address once DHCP answers) and a setting may be changed from
           the shell: a redraw a second keeps them true */
        if (++ticks >= 12) {
            ticks = 0;
            again = 1;
        }
        if (again)
            draw();
        sys_sleep(80);
    }
}
