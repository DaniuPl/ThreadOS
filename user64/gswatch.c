/* gswatch - Swatch: every control of ProtonUI v2, live
 *
 * docs/weave/canvas/Swatch.dc.html. For someone writing a program for
 * ThreadOS: down the left the controls user/ugui.h has; in the middle
 * the one chosen in each of its states, drawn by the toolkit itself
 * (not a picture of it), and under them one to try - what it sends when
 * it is used is said beside the lines of C that make it, which are the
 * real calls, to copy. Night or Linen - in the title bar's theme bar
 * when started with --theme-bar, as the desktop starts it, otherwise on
 * the preview - shows the controls in either theme, whichever the
 * desktop is in. On the right, Start a
 * program: four templates, a name, and a project in ~/Projects that
 * Code opens and its Run builds with clang on ThreadOS and starts - a
 * window that is a ProtonUI v2 program to grow from.
 *
 * What happens is said on the serial port, for scripts/tests/t_swatch.py:
 *
 *   [swatch] drawn W H
 *   [swatch] chose NAME
 *   [swatch] theme Night | Linen
 *   [swatch] sent EVENT VALUE
 *   [swatch] made PATH from TEMPLATE
 *
 * Built against musl, as Code is: it writes files at paths of any
 * length and starts Code.
 */

#include "uapi.h"
#include "udraw.h"
#include "ugui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The layout, in pixels of the window's content (t_swatch reads these):
   the controls' list down the left, Start a program down the right, the
   preview and under it the code between them */
#define W0 760
#define H0 470
#define W1 1000
#define H1 620
#define SIDE_X 8
#define SIDE_Y 10
#define SIDE_W 150
#define RIGHT_W 220
#define GAP 8
#define CODE_H 150            /* the code under the preview */
#define HEAD_Y 64             /* the preview's heading, its baseline */
#define SLOT_W 86             /* a state, side by side */
#define STATE_Y 96            /* the states' top */
#define ROW_STEP 44           /* a state, one under another */
#define LABEL_DY 20           /* a state's name, below its control */
#define TRY_DY 56             /* "Try it" above the bottom of the preview */
#define CODE_PX 12
#define CODE_LINE 17
#define THEME_W 128           /* Night | Linen on the preview, when
                                 started without --theme-bar */
#define THEME_H 26
#define TPL_Y 76              /* the first template's top, in the right column */
#define TPL_H 50
#define TPL_STEP 58

static int win = -1, cw, ch;
static struct ug ui, smp, live;
static struct ug_item ui_room[16], smp_room[16], live_room[8];
static struct ug_modal ask;
static struct ud_look plook;          /* the preview's theme */
static int sel = 2, linen, tpl, phase;
static int side_id, theme_id, name_id, make_id;
static int in_bar;                    /* Night | Linen in the theme bar */
static char name[48] = "backup-check";
static char field_buf[64] = "threados-pc";
static char last[48] = "";            /* what the live one sent last */

/* --- the catalogue ------------------------------------------------------ */

enum { ROW, COLUMN };
struct entry {
    const char *name;
    const char *code[6];
    const char *sends;
    int layout, n;
    const char *state[4];
};

static const struct entry cat[] = {
    { "pu_button",
      { "int go = pu_button(&ui, x, y, 96, 34, \"Install\");",
        "pu_kind(&ui, go, PU_PRIMARY);", "",
        "/* PU_SECONDARY, PU_GHOST and PU_DANGER", "   for the others */" },
      "PU_PRESSED", ROW, 4, { "Rest", "Pressed", "Focused", "Unavailable" } },
    { "pu_field",
      { "static char name[64];",
        "int f = pu_field(&ui, x, y, 220, name, 64);", "",
        "/* wrong: why, under it */",
        "pu_field_error(&ui, f, \"1 to 65535\");" },
      "PU_TYPED, PU_ENTER", COLUMN, 4,
      { "Rest", "Focused", "Wrong", "Unavailable" } },
    { "pu_switch",
      { "int wifi = pu_switch(&ui, x, y, 200,", "                     \"Wi-Fi\", 1);",
        "", "/* later */", "if (pu_value(&ui, wifi))", "    join_network();" },
      "PU_CHANGED", ROW, 4, { "Off", "On", "Focused", "Unavailable" } },
    { "pu_segments",
      { "int v = pu_segments(&ui, x, y, 172, 34,",
        "                    view_of, 0, 3, 0);", "",
        "/* view_of(ctx, i): the i-th one's words */" },
      "PU_CHANGED", COLUMN, 3, { "First", "Another", "Focused" } },
    { "pu_progress",
      { "int bar = pu_progress(&ui, x, y, 240, 0, 100);",
        "pu_set_value(&ui, bar, done_percent());", "",
        "/* -1: the length is not known; */",
        "/* pu_set_phase moves it along */" },
      "nothing: it only shows", COLUMN, 4,
      { "Starting", "Half", "Almost", "Unknown" } },
    { "pu_spinner",
      { "int s = pu_spinner(&ui, x, y);", "",
        "/* every frame, of 1000 */", "pu_set_phase(&ui, s, phase);" },
      "nothing: it only shows", ROW, 1, { "Winding" } },
    { "pu_knots",
      { "int size = pu_knots(&ui, x, y, 172,",
        "                    size_of, 0, 3, 1);" },
      "PU_CHANGED", ROW, 3, { "Hollow", "Chosen", "Focused" } },
    { "pu_menu",
      { "/* on PU_MENU: a right click, a long press */",
        "pu_menu(&ui, id, ev.x, ev.y, lines, 4);", "",
        "/* then PU_CHOSEN: */", "int line = pu_menu_choice(&ui);" },
      "PU_MENU, then PU_CHOSEN", ROW, 1, { "Open" } },
    { "pu_card",
      { "pu_card(&ui, x, y, 300, 160, \"Window\");", "",
        "/* what is added after it lies on it */" },
      "nothing: it groups", ROW, 1, { "With a heading" } },
    { "pu_avatar",
      { "pu_avatar(&ui, x, y, 48, \"Alex\", 0);", "",
        "/* 0: the person's own thread colour */" },
      "nothing: it only shows", ROW, 4,
      { "The person's", "Cochineal", "Weld", "Small" } },
    { "pu_side",
      { "int page = pu_side(&ui, x, y, 160,",
        "                   page_of, 0, 5, 0);", "",
        "/* PU_SIDE_MONO as its kind: code names */" },
      "PU_CHANGED", ROW, 2, { "Chosen", "Focused" } },
    { "pu_sheet",
      { "static struct ug_modal ask;",
        "pu_sheet(&ask, \"Shut down now?\",",
        "         \"Work not saved is lost.\",",
        "         \"Shut down\", \"Cancel\", 1);", "",
        "ug_modal_event(&ui, &ask, &ev); /* answer */" },
      "PU_PRESSED: ask.answer 1 or 0", ROW, 0, { "" } },
};
#define CAT_N ((int)(sizeof(cat) / sizeof(cat[0])))

static const char *side_of(void *ctx, int i) { (void)ctx; return cat[i].name; }

static const char *const themes[2] = { "Night", "Linen" };
static const char *theme_of(void *ctx, int i) { (void)ctx; return themes[i]; }

static const char *const views[3] = { "List", "Icons", "Tree" };
static const char *view_of(void *ctx, int i) { (void)ctx; return views[i]; }
static const char *const sizes[3] = { "Small", "Medium", "Large" };
static const char *size_of(void *ctx, int i) { (void)ctx; return sizes[i]; }
static const char *const pages[4] = { "General", "Network", "Sound", "About" };
static const char *page_of(void *ctx, int i) { (void)ctx; return pages[i]; }
static const char *const menu_lines[4] = { "Open", "Rename", "Copy",
                                          "Move to Trash" };

/* --- the templates ------------------------------------------------------ */

/* How Code builds and runs a project made here (.code/project's run=):
   the program's name twice */
#define CLANG_RUN "/usr/bin/clang -O2 src/main.c -o %s && ./%s"

struct tpl {
    const char *name, *what;
    uint32_t dye;                 /* its tile: Weave's dyes, Night's */
    char letter;
    const char *main_c;           /* NAME stands for the project's name */
};

static const struct tpl tpls[4] = {
    { "A window with a list", "Rows, a toolbar, details", 0x4A6BE0,
      'L',
      "/* NAME: a window with a list - ProtonUI v2 (user/ugui.h) */\n\n"
      "#include \"udraw.h\"\n#include \"ugui.h\"\n\n"
      "static const char *const rows[] = { \"First\", \"Second\", \"Third\" };\n"
      "static const char *row_of(void *ctx, int i)\n"
      "{\n    (void)ctx;\n    return rows[i];\n}\n\n"
      "int main(void)\n{\n    static struct ug ui;\n"
      "    static struct ug_item room[16];\n"
      "    int win = (int)win_create(520, 340, \"NAME\");\n"
      "    pu_start(&ui, win, 520, 340, room, 16);\n"
      "    int add = pu_button(&ui, 12, 12, 80, 32, \"New\");\n"
      "    pu_kind(&ui, add, PU_PRIMARY);\n"
      "    int list = pu_list(&ui, 12, 56, 200, 270, row_of, 0, 3);\n"
      "    for (;;) {\n        struct win_event ev;\n"
      "        while (win_poll(win, &ev) == 1) {\n            int which;\n"
      "            int what = ug_event(&ui, &ev, &which);\n"
      "            if (what == UG_CLOSED)\n                return 0;\n"
      "            if (what == PU_CHANGED && which == list) {\n"
      "                /* show the row's details on the right */\n"
      "            }\n"
      "        }\n        ug_draw(&ui);\n        sys_sleep(50);\n    }\n}\n" },
    { "Settings-like", "Sections, then rows", 0x4B505E,
      'S',
      "/* NAME: sections and rows - ProtonUI v2 (user/ugui.h) */\n\n"
      "#include \"udraw.h\"\n#include \"ugui.h\"\n\n"
      "static const char *const pages[] = { \"General\", \"Network\", \"About\" };\n"
      "static const char *page_of(void *ctx, int i)\n"
      "{\n    (void)ctx;\n    return pages[i];\n}\n\n"
      "int main(void)\n{\n    static struct ug ui;\n"
      "    static struct ug_item room[16];\n"
      "    int win = (int)win_create(600, 380, \"NAME\");\n"
      "    pu_start(&ui, win, 600, 380, room, 16);\n"
      "    pu_side(&ui, 8, 10, 160, page_of, 0, 3, 0);\n"
      "    pu_switch(&ui, 200, 20, 360, \"Start at login\", 1);\n"
      "    pu_switch(&ui, 200, 60, 360, \"Send reports\", 0);\n"
      "    for (;;) {\n        struct win_event ev;\n"
      "        while (win_poll(win, &ev) == 1) {\n            int which;\n"
      "            if (ug_event(&ui, &ev, &which) == UG_CLOSED)\n"
      "                return 0;\n        }\n"
      "        ug_draw(&ui);\n        sys_sleep(50);\n    }\n}\n" },
    { "A phone program", "Finger sizes, a sheet", 0x2F9A5C,
      'P',
      "/* NAME: for the phone - ProtonUI v2 (user/ugui.h) lays it out for\n"
      "   a finger there: rows of 48, targets of 44 */\n\n"
      "#include \"udraw.h\"\n#include \"ugui.h\"\n\n"
      "int main(void)\n{\n    static struct ug ui;\n"
      "    static struct ug_item room[16];\n"
      "    static struct ug_modal ask;\n"
      "    int win = (int)win_create(360, 560, \"NAME\");\n"
      "    pu_start(&ui, win, 360, 560, room, 16);\n"
      "    int t = pu_target(&ui);\n"
      "    int go = pu_button(&ui, 12, 12, 336, t, \"Ask\");\n"
      "    pu_kind(&ui, go, PU_PRIMARY);\n"
      "    for (;;) {\n        struct win_event ev;\n"
      "        while (win_poll(win, &ev) == 1) {\n            int which;\n"
      "            if (ask.on) {\n"
      "                ug_modal_event(&ui, &ask, &ev);\n"
      "                continue;\n            }\n"
      "            int what = ug_event(&ui, &ev, &which);\n"
      "            if (what == UG_CLOSED)\n                return 0;\n"
      "            if (what == PU_PRESSED && which == go)\n"
      "                pu_sheet(&ask, \"Send it?\", \"It goes now.\",\n"
      "                         \"Send\", \"Not now\", 0);\n"
      "        }\n        ug_draw(&ui);\n        ug_modal_draw(&ui, &ask);\n"
      "        sys_sleep(50);\n    }\n}\n" },
    { "A game", "Sixty frames, the mouse", 0x6A4FD6, 'G',
      "/* NAME: a game - a loop at sixty frames, drawn with user/udraw.h */\n\n"
      "#include \"udraw.h\"\n\n"
      "int main(void)\n{\n    int win = (int)win_create(640, 400, \"NAME\");\n"
      "    int x = 320, y = 200, held = 0;\n"
      "    for (;;) {\n        struct win_event ev;\n"
      "        while (win_poll(win, &ev) == 1) {\n"
      "            if (ev.type == WEV_CLOSE)\n                return 0;\n"
      "            if (ev.type == WEV_PRESS)\n                held = 1;\n"
      "            if (ev.type == WEV_RELEASE)\n                held = 0;\n"
      "            if (ev.type == WEV_DRAG && held) {\n"
      "                x = ev.x;\n                y = ev.y;\n            }\n"
      "        }\n        ud_fill(win, 0, 0, 640, 400, 0x0D0F15);\n"
      "        ud_disc(win, x - 12, y - 12, 24, 0x5B7CFA);\n"
      "        sys_sleep(16);\n    }\n}\n" },
};

/* --- where things are -------------------------------------------------- */

static int prev_x(void) { return SIDE_X + SIDE_W + GAP; }
static int right_x(void) { return cw - RIGHT_W - GAP; }
static int prev_w(void) { return right_x() - GAP - prev_x(); }
static int prev_h(void) { return ch - CODE_H; }

static void say(const char *what, const char *arg)
{
    printf("[swatch] %s %s\n", what, arg);
    fflush(stdout);
}

/* --- the preview ------------------------------------------------------- */

static void look_now(void)
{
    ud_look_of(linen ? UD_LIGHT : UD_DARK, &plook);
    smp.look = plook;
    live.look = plook;
}

/* A state's slot: side by side, or one under another */
static void slot(int k, int *x, int *y)
{
    const struct entry *e = &cat[sel];
    if (e->layout == ROW) {
        int total = e->n * SLOT_W;
        *x = prev_x() + (prev_w() - total) / 2 + k * SLOT_W;
        *y = STATE_Y;
    } else {
        *x = prev_x() + 110;
        *y = STATE_Y + k * ROW_STEP;
    }
}

/* The states, as items the toolkit draws: not to be used, only seen */
static void samples(void)
{
    pu_start(&smp, win, cw, ch, smp_room, 16);
    smp.look = plook;
    smp.focus = -1;
    int x, y, id, colw = prev_w() - 130;
    for (int k = 0; k < cat[sel].n; k++) {
        slot(k, &x, &y);
        switch (sel) {
        case 0:                                       /* pu_button */
            if (k == 1)
                break;                                /* drawn pressed below */
            id = pu_button(&smp, x + 4, y, SLOT_W - 8, 32, "Install");
            pu_kind(&smp, id, PU_PRIMARY);
            if (k == 2)
                smp.focus = id;
            if (k == 3)
                pu_enable(&smp, id, 0);
            break;
        case 1:                                       /* pu_field */
            id = pu_field(&smp, x, y, colw, field_buf, sizeof(field_buf));
            if (k == 1)
                smp.focus = id;
            if (k == 2)
                pu_field_error(&smp, id, "A port is 1 to 65535");
            if (k == 3)
                pu_enable(&smp, id, 0);
            break;
        case 2:                                       /* pu_switch */
            id = pu_switch(&smp, x + (SLOT_W - PU_SWITCH_W) / 2, y,
                           PU_SWITCH_W, "", k == 1 || k == 2);
            if (k == 2)
                smp.focus = id;
            if (k == 3)
                pu_enable(&smp, id, 0);
            break;
        case 3:                                       /* pu_segments */
            id = pu_segments(&smp, x, y, colw < 200 ? colw : 200, 30,
                             view_of, 0, 3, k == 1 ? 1 : 0);
            if (k == 2)
                smp.focus = id;
            break;
        case 4:                                       /* pu_progress */
            id = pu_progress(&smp, x, y + 10, colw, k == 0 ? 8 : k == 1 ? 50
                             : k == 2 ? 92 : -1, 100);
            pu_set_phase(&smp, id, phase);
            break;
        case 5:                                       /* pu_spinner */
            id = pu_spinner(&smp, x + (SLOT_W - PU_SPINNER) / 2, y);
            pu_set_phase(&smp, id, phase);
            break;
        case 6:                                       /* pu_knots */
            id = pu_knots(&smp, x + 14, y, SLOT_W - 14, size_of, 0, 1,
                          k == 0 ? -1 : 0);
            if (k == 2)
                smp.focus = id;
            break;
        case 8:                                       /* pu_card */
            pu_card(&smp, x - 60, y - 10, 212, 110, "Window");
            pu_switch(&smp, x - 60 + PU_CARD_PAD, y + 40, 212 - 2 * PU_CARD_PAD,
                      "Bluetooth", 1);
            break;
        case 9: {                                     /* pu_avatar */
            static const uint32_t dyes[4] = { 0, 0xD152A6, 0x46C27A,
                                              0xF5A524 };
            static const char *const who[4] = { "Alex", "Maja", "Jan", "Ola" };
            int d = k == 3 ? 32 : 48;
            pu_avatar(&smp, x + (SLOT_W - d) / 2, y + (48 - d) / 2, d, who[k],
                      dyes[k]);
            break;
        }
        case 10:                                      /* pu_side */
            id = pu_side(&smp, x + 4, y, SLOT_W - 8, page_of, 0, 3, 1);
            if (k == 1)
                smp.focus = id;
            break;
        }
    }
}

/* One to try, under the states */
static int live_id = -1;
static void try_one(void)
{
    pu_start(&live, win, cw, ch, live_room, 8);
    live.look = plook;
    int y = prev_h() - TRY_DY + 8, x = prev_x() + 96, w = prev_w() - 120;
    live_id = -1;
    switch (sel) {
    case 0:
        live_id = pu_button(&live, x, y, 120, 32, "Install");
        pu_kind(&live, live_id, PU_PRIMARY);
        break;
    case 1:
        live_id = pu_field(&live, x, y, w, field_buf, sizeof(field_buf));
        break;
    case 2:
        live_id = pu_switch(&live, x, y, w < 220 ? w : 220, "Wi-Fi", 1);
        break;
    case 3:
        live_id = pu_segments(&live, x, y, w < 200 ? w : 200, 30, view_of, 0,
                              3, 0);
        break;
    case 4:
        live_id = pu_progress(&live, x, y + 10, w, 0, 100);
        break;
    case 5:
        live_id = pu_spinner(&live, x, y);
        break;
    case 6:
        live_id = pu_knots(&live, x, y - 24, w < 200 ? w : 200, size_of, 0, 3,
                           1);
        break;
    case 7:
        live_id = pu_button(&live, x, y, 200, 32, "Right-click or hold me");
        pu_kind(&live, live_id, PU_SECONDARY);
        break;
    case 10:                          /* two of its sections, clear of the
                                         code under the preview */
        live_id = pu_side(&live, x, prev_h() - 2 * pu_side_row(&live) - 6,
                          w < 200 ? w : 200, page_of, 0, 2, 0);
        break;
    case 11:
        live_id = pu_button(&live, x, y, 140, 32, "Ask");
        pu_kind(&live, live_id, PU_DANGER);
        break;
    }
}

static void build(void)
{
    pu_start(&ui, win, cw, ch, ui_room, 16);
    side_id = pu_side(&ui, SIDE_X, SIDE_Y, SIDE_W, side_of, 0, CAT_N, sel);
    pu_kind(&ui, side_id, PU_SIDE_MONO);
    pu_name(&ui, side_id, "Controls");
    /* Night | Linen is the window's: in its title bar, the system's
       theme bar, when Swatch was started with --theme-bar (as the
       desktop starts it); otherwise on the preview */
    theme_id = -1;
    if (!in_bar) {
        theme_id = pu_segments(&ui, prev_x() + prev_w() - THEME_W - 8, 8,
                               THEME_W, THEME_H, theme_of, 0, 2, linen);
        pu_name(&ui, theme_id, "Theme");
    }
    int rx = right_x() + 12, rw = RIGHT_W - 24;
    int fy = TPL_Y + 4 * TPL_STEP + 22;
    name_id = pu_field(&ui, rx, fy, rw, name, sizeof(name));
    pu_name(&ui, name_id, "Name");
    make_id = pu_button(&ui, rx, ch - 50, rw, 36, "Make it, and open in Code");
    pu_kind(&ui, make_id, PU_PRIMARY);
    look_now();
    samples();
    try_one();
}

/* What the code says it is, in the window's own colours: the lines,
   and what it sends - the last of it heard from the one to try */
static void draw_code(const struct ud_look *l)
{
    int x = prev_x(), y = prev_h(), w = prev_w();
    ud_fill(win, x, y, w, ch - y, l->field);
    ud_fill(win, x, y, w, 1, l->line);
    for (int i = 0; i < 6 && cat[sel].code[i]; i++)
        ud_text(win, x + 14, y + 24 + i * CODE_LINE, cat[sel].code[i], l->text,
                UD_MONO, CODE_PX);
    int sy = y + 24 + 6 * CODE_LINE + 6;
    ud_text(win, x + 14, sy, "WHAT IT SENDS", l->dim, UD_BOLD, 11);
    int ex = x + 14 + ud_width("WHAT IT SENDS", UD_BOLD, 11) + 10;
    ud_text(win, ex, sy, cat[sel].sends, l->accent, UD_MONO, CODE_PX);
}

static void draw_right(const struct ud_look *l)
{
    int x = right_x(), w = RIGHT_W;
    ud_fill(win, x - GAP / 2, 0, 1, ch, l->line);
    ud_text(win, x + 12, 28, "Start a program", l->text, UD_BOLD, 15);
    ud_text(win, x + 12, 50, "A project in Code, with a window", l->dim,
            UD_SANS, 12);
    ud_text(win, x + 12, 66, "to start from.", l->dim, UD_SANS, 12);
    for (int i = 0; i < 4; i++) {
        int y = TPL_Y + i * TPL_STEP;
        uint32_t bg = i == tpl ? pu_mix(l->accent, l->bg, 36) : l->field;
        ud_round(win, x + 12, y, w - 24, TPL_H, 10, bg);
        ud_ring(win, x + 12, y, w - 24, TPL_H, 10, i == tpl ? l->accent
                                                             : l->line);
        ud_round(win, x + 20, y + 10, 30, 30, 8, tpls[i].dye);
        char c[2] = { tpls[i].letter, 0 };
        ud_text_mid(win, x + 20, y + 10, 30, 30, c, 0xFFFFFF, UD_BOLD, 14);
        ud_text(win, x + 58, y + 22, tpls[i].name, l->text, UD_BOLD, UD_PX);
        ud_text(win, x + 58, y + 38, tpls[i].what, l->dim, UD_SANS, 11);
    }
    ud_text(win, x + 12, TPL_Y + 4 * TPL_STEP + 14, "Name", l->dim, UD_SANS,
            UD_PX);
}

static void draw_preview(void)
{
    const struct ud_look *p = &plook;
    int x = prev_x(), w = prev_w(), h = prev_h();
    ud_fill(win, x, 0, w, h, p->bg);
    for (int gx = x + 24; gx < x + w; gx += 24)          /* the grid */
        ud_fill_a(win, gx, 0, 1, h, p->dark ? 0xFFFFFF : 0x000000, 8);
    for (int gy = 24; gy < h; gy += 24)
        ud_fill_a(win, x, gy, w, 1, p->dark ? 0xFFFFFF : 0x000000, 8);
    char head[48];
    snprintf(head, sizeof(head), "%s", cat[sel].name);
    for (char *c = head; *c; c++)
        if (*c >= 'a' && *c <= 'z')
            *c = (char)(*c - 32);
    strncat(head, " - STATES", sizeof(head) - strlen(head) - 1);
    int hw = ud_width(head, UD_BOLD, UD_PX);
    ud_text(win, x + (w - hw) / 2, HEAD_Y, head, p->dim, UD_BOLD, UD_PX);

    ug_draw_controls(&smp);
    const struct entry *e = &cat[sel];
    for (int k = 0; k < e->n; k++) {
        int sx, sy;
        slot(k, &sx, &sy);
        if (sel == 0 && k == 1)                          /* pressed */
            pu_draw_button(win, p, sx + 4, sy, SLOT_W - 8, 32, "Install",
                           PU_PRIMARY, PU_DOWN);
        if (e->layout == ROW) {
            int lw = ud_width(e->state[k], UD_SANS, UD_PX);
            int below = sel == 8 ? 116 : sel == 10 ? 3 * PU_SIDE_ROW + 4 :
                        sel == 9 ? 52 : sel == 7 ? 130 : 36;
            ud_text(win, sx + (SLOT_W - lw) / 2, sy + below + LABEL_DY - 6,
                    e->state[k], p->dim, UD_SANS, UD_PX);
        } else {
            ud_text(win, x + 20, sy + 20, e->state[k], p->dim, UD_SANS, UD_PX);
        }
    }
    if (sel == 7) {                  /* a menu, open, as the toolkit draws it */
        struct ug m = smp;
        m.menu_n = 4;
        m.menu_items = menu_lines;
        m.menu_x = x + (w - PU_MENU_W) / 2;
        m.menu_y = STATE_Y - 10;
        m.menu_for = -1;
        pu_draw_menu(&m);
    }
    if (sel == 11)
        ud_text(win, x + 20, STATE_Y + 20,
                "It covers the window: try it below.", p->dim, UD_SANS, UD_PX);
    if (live_id >= 0 && sel != 8 && sel != 9)
        ud_text(win, x + 20, h - TRY_DY + 28, "Try it", p->dim, UD_BOLD,
                UD_PX);
    ug_draw_controls(&live);
    if (last[0]) {               /* what the one to try sent last */
        char said[64];
        snprintf(said, sizeof(said), "sent %s", last);
        ud_text(win, x + w - 14 - ud_width(said, UD_MONO, CODE_PX),
                h - 12, said, p->dim, UD_MONO, CODE_PX);
    }
}

static void draw(void)
{
    ug_draw(&ui);
    draw_preview();
    draw_code(&ui.look);
    draw_right(&ui.look);
    /* Night | Linen stands on the preview, which was just laid over it;
       nothing else of the window's is, and drawn twice its words would
       come out heavier */
    if (theme_id >= 0)
        ug_draw_one(&ui, theme_id);
    ug_modal_draw(&ui, &ask);
}

/* --- Start a program --------------------------------------------------- */

static int write_file(const char *path, const char *body, const char *nm)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    for (const char *p = body; *p; p++) {
        if (!strncmp(p, "NAME", 4)) {
            fputs(nm, f);
            p += 3;
        } else {
            fputc(*p, f);
        }
    }
    return fclose(f);
}

static int make_project(void)
{
    char home[256], dir[512], path[640], meta[512];
    if (!name[0] || strchr(name, '/') || !getcwd(home, sizeof(home)))
        return -1;
    snprintf(dir, sizeof(dir), "%s/Projects", home);
    mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/Projects/%s", home, name);
    if (mkdir(dir, 0755) < 0)
        return -1;                   /* there is one of that name */
    snprintf(path, sizeof(path), "%s/src", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/src/main.c", dir);
    if (write_file(path, tpls[tpl].main_c, name) < 0)
        return -1;
    snprintf(path, sizeof(path), "%s/README.md", dir);
    snprintf(meta, sizeof(meta), "# %s\n\n%s, from Swatch's template: a "
             "ProtonUI v2 program (user/ugui.h) to grow from.\n", name,
             tpls[tpl].name);
    write_file(path, meta, name);
    snprintf(path, sizeof(path), "%s/.code", dir);
    mkdir(path, 0755);
    /* Run in Code builds it with clang on ThreadOS - threados.cfg
       finds ugui.h - and starts what was built: two steps, each one
       program and at most eight words of 31 letters (user/uterm.h), so
       the program is called after the project only where its name is
       short and plain enough to be a word there */
    char prog[32] = "app";
    int plain = strlen(name) <= 24;
    for (const char *c = name; *c && plain; c++)
        plain = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                (*c >= '0' && *c <= '9') || *c == '-' || *c == '_';
    if (plain)
        snprintf(prog, sizeof(prog), "%s", name);
    snprintf(path, sizeof(path), "%s/.code/project", dir);
    snprintf(meta, sizeof(meta), "name=%s\nrun=" CLANG_RUN "\nopen=src/main.c\n"
             "active=src/main.c\n", name, prog, prog);
    write_file(path, meta, name);
    /* first among the projects Code opened lately: the one it opens */
    char list[4096] = "", *old = NULL;
    snprintf(path, sizeof(path), "%s/.code-projects", home);
    FILE *f = fopen(path, "r");
    if (f) {
        size_t n = fread(list, 1, sizeof(list) - 1, f);
        list[n] = 0;
        fclose(f);
        old = list;
    }
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s\n%s", dir, old ? old : "");
        fclose(f);
    }
    printf("[swatch] made %s from %s\n", dir, tpls[tpl].name);
    fflush(stdout);
    if (sys_fork() == 0) {
        sys_exec("gcode", (char *const *)0);
        sys_exit(1);
    }
    return 0;
}

/* --- events ------------------------------------------------------------- */

static void heard(int what, int which)
{
    static const char *const names[] = { "", "PU_PRESSED", "PU_CHANGED",
        "PU_TYPED", "PU_ENTER", "", "", "", "PU_MENU", "PU_CHOSEN" };
    if (what <= 0 || what > 9 || !names[what][0])
        return;
    int v = what == UG_CHOSEN ? pu_menu_choice(&live)
                              : which >= 0 ? pu_value(&live, which) : 0;
    snprintf(last, sizeof(last), "%s %d", names[what], v);
    printf("[swatch] sent %s\n", last);
    fflush(stdout);
}

/* The preview's own, but for Night | Linen at its top right, which is
   the window's */
static int in_preview(int x, int y)
{
    int tx = prev_x() + prev_w() - THEME_W - 8;
    if (theme_id >= 0 && x >= tx && x < tx + THEME_W && y >= 8 &&
        y < 8 + THEME_H)
        return 0;
    return x >= prev_x() && x < prev_x() + prev_w() && y < prev_h();
}

int main(int argc, char **argv)
{
    int want_bar = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], THEME_BAR_FLAG))
            want_bar = 1;
    static const int sz[2][2] = { { W1, H1 }, { W0, H0 } };
    for (int i = 0; i < 2 && win < 0; i++) {
        win = (int)win_create(sz[i][0], sz[i][1],
                              "Swatch: ProtonUI v2 \xC2\xB7 every control, live");
        cw = sz[i][0];
        ch = sz[i][1];
    }
    if (win < 0)
        return 1;
    win_truecolour(win);
    in_bar = want_bar && win_theme_bar(win, linen) == 0;
    build();
    draw();
    printf("[swatch] drawn %d %d\n", cw, ch);
    fflush(stdout);
    for (;;) {
        struct win_event ev;
        int redraw = 0, which;
        while (win_poll(win, &ev) == 1) {
            redraw = 1;
            if (ev.type == WEV_CLOSE)
                return 0;
            if (ev.type == WEV_LOOK) {
                look_now();
                continue;
            }
            if (ev.type == WEV_THEME && ev.val >= 0 && ev.val < 2) {
                linen = ev.val;
                say("theme", themes[linen]);
                build();
                continue;
            }
            if (ev.type == WEV_SIZE && ev.x > 0 && ev.y > 0) {
                cw = ev.x;
                ch = ev.y;
                build();
                continue;
            }
            if (ask.on) {                /* the question has every event */
                if (ug_modal_event(&ui, &ask, &ev) == UG_PRESSED) {
                    snprintf(last, sizeof(last), "PU_PRESSED %d", ask.answer);
                    printf("[swatch] sent %s\n", last);
                    fflush(stdout);
                }
                continue;
            }
            int pos = ev.type == WEV_CLICK || ev.type == WEV_PRESS ||
                      ev.type == WEV_DRAG || ev.type == WEV_RELEASE ||
                      ev.type == WEV_ALTCLICK;
            if ((pos && in_preview(ev.x, ev.y)) ||
                (!pos && live.focus >= 0) || live.menu_n) {
                int what = ug_event(&live, &ev, &which);
                if (what == UG_MENU && sel == 7) {
                    pu_menu(&live, which, ev.x, ev.y, menu_lines, 4);
                    heard(what, which);
                } else if (what == UG_PRESSED && sel == 11) {
                    pu_sheet(&ask, "Shut down now?",
                             "Work that is not saved is lost when it "
                             "shuts down.", "Shut down", "Cancel", 1);
                    heard(what, which);
                } else {
                    heard(what, which);
                }
                if (pos)
                    ui.focus = -1;
                continue;
            }
            if (ev.type == WEV_CLICK && ev.x >= right_x() + 12 &&
                ev.x < right_x() + RIGHT_W - 12 && ev.y >= TPL_Y &&
                ev.y < TPL_Y + 4 * TPL_STEP) {
                int i = (ev.y - TPL_Y) / TPL_STEP;
                if (ev.y < TPL_Y + i * TPL_STEP + TPL_H && i != tpl) {
                    tpl = i;
                    say("template", tpls[i].name);
                }
                continue;
            }
            int what = ug_event(&ui, &ev, &which);
            if (pos)
                live.focus = -1;
            if (what == UG_CLOSED)
                return 0;
            if (what == UG_CHANGED && which == side_id) {
                sel = pu_value(&ui, side_id);
                last[0] = 0;
                say("chose", cat[sel].name);
                build();
            } else if (what == UG_CHANGED && which == theme_id) {
                linen = pu_value(&ui, theme_id);
                say("theme", themes[linen]);
                build();
            } else if (what == UG_PRESSED && which == make_id) {
                if (make_project() < 0)
                    say("not made", name);
            }
        }
        /* what moves by itself: progress not known, the spinner, and
           the bar to try filling */
        if (sel == 4 || sel == 5) {
            phase = (phase + 20) % 1000;
            for (int i = 0; i < smp.n; i++)
                pu_set_phase(&smp, i, phase);
            if (live_id >= 0) {
                pu_set_phase(&live, live_id, phase);
                if (sel == 4)
                    pu_set_value(&live, live_id, phase / 10);
            }
            redraw = 1;
        }
        if (redraw)
            draw();
        sys_sleep(50);
    }
}
