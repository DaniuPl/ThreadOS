#ifndef THREADOS_LOOK_H
#define THREADOS_LOOK_H

/* How the desktop looks: every size and every colour, in one place.
 *
 * The desktop is drawn in real colours, with light let through glass:
 * a menu bar along the top that the background shows through, blurred;
 * windows with round corners standing on soft shadows, their three
 * buttons as coloured lights on the left; and at the bottom a dock -
 * one row of tiles that starts programs and holds the open windows.
 *
 * Two sets of colours, light and dark, picked by the 'theme' setting.
 * The shapes are the same in both.
 *
 * scripts/ptest.py reads this file - every #define in it - so the tests
 * know where things are and what colour they should be without one
 * number being written down twice. A number changed here moves the
 * tests with it.
 */

/* --- text -------------------------------------------------------------- */
/* The sizes of the lettering: the bar, the titles and the menu a pixel
   larger than the rest (UI_PX in uitext.h), because they are read from
   further off - across the whole screen. */
#define BAR_PX       12
#define TITLE_PX     12
#define MENU_PX      12

/* --- the menu bar ------------------------------------------------------ */
#define BAR_H        24      /* its height */
#define BAR_LOGO_X   10      /* the mark at its left end */
#define BAR_LOGO_D   12      /* ... and how large it is */
#define BAR_NAME_X   30      /* "ThreadOS", in bold */
#define BAR_APP_GAP  16      /* from that to the focused window's name */
#define BAR_RIGHT    10      /* air at the right-hand end */
#define BAR_ITEM_GAP 14      /* between the things on the right */
#define BAR_BLUR     8       /* how far the glass smears what is under it */
#define BAR_CLOCK_W  138     /* the clock's place, at the right-hand end */

/* the four desktops, as four small cells on the right of the bar */
#define DESK_CELL    16
#define DESK_CELL_H  14
/* how much of the memory is in use: a small capsule */
#define LOAD_W       36
#define LOAD_H       8
/* show the desktop: a button the size of a cell */
#define SHOW_W       18
#define STAT_W       16      /* the network and Bluetooth icons */
#define STAT_CARD_W  260     /* the card a click on one hangs from the bar */
#define STAT_ROW     20
#define STAT_PAD     12
#define STAT_LINES   8
#define STAT_SCAN_S  5       /* how long Bluetooth listens for devices */

/* --- windows ----------------------------------------------------------- */
/* The title bar. Twenty two pixels: the lights are eleven across and
   need air round them, and the title is drawn in bold. */
#define TITLE_H      22
#define WIN_R        8       /* the corner radius */
#define LIGHT_D      11      /* the close, minimise and zoom lights */
#define LIGHT_X      8       /* the first light's left edge, from the window's */
#define LIGHT_STEP   17      /* from one light to the next */
#define LIGHT_HIT    15      /* the square around a light that takes a click */
#define SHADOW_S     14      /* how far the shadow reaches */
#define SHADOW_DY    4       /* ... and how far it falls below */

/* the three lights: the same in both themes, because they are signals */
#define LIGHT_CLOSE  0xFF5F57
#define LIGHT_MIN    0xFEBC2E
#define LIGHT_ZOOM   0x28C840

/* --- the dock ----------------------------------------------------------- */
#define DOCK_TILE_S  26      /* the 'dock' setting: 0 small */
#define DOCK_TILE_M  34      /*                     1 medium */
#define DOCK_TILE_L  42      /*                     2 large */
#define DOCK_TILE_MIN 14     /* a crowded dock makes its tiles no smaller */
#define DOCK_GAP     6       /* between two tiles */
#define DOCK_SEP     13      /* between the programs and the windows */
#define DOCK_PAD     6       /* between the glass's edge and the tiles */
#define DOCK_R       14      /* the glass's corner radius */
#define DOCK_BOTTOM  6       /* from the screen's bottom edge */
#define DOCK_MARGIN  8       /* the least room left at either side */
#define DOCK_DOT     4       /* the dot under a program that is running */

/* --- the launch menu ---------------------------------------------------- */
#define MENU_W       176
#define MENU_ROW     24
#define MENU_PAD     5       /* above the first entry and below the last */
#define MENU_R       10
#define MENU_ICON    18
#define MENU_Y       (BAR_H + 4)  /* opened from the bar, it hangs here */
#define MENU_SEP     7       /* the line between the programs and the system */
#define TMENU_W      132     /* the right button's menu on a dock tile */
#define TMENU_GAP    8       /* between it and the dock */

/* --- how loud: the card the loudness keys bring up ------------------- */
#define VOL_W        240
#define VOL_H        52
#define VOL_R        14
#define VOL_DY       24      /* above the dock */
#define VOL_BAR_X    52      /* the bar, from the card's left edge */
#define VOL_BAR_W    128
#define VOL_BAR_H    6
#define SESS_ROW     22      /* an entry below it, a little lower */

/* --- the question before logging out, restarting, shutting down ------- */
#define ASK_W        320
#define ASK_H        150
#define ASK_R        14
#define ASK_PAD      20
#define ASK_TITLE_Y  40      /* the question's baseline, from the top */
#define ASK_BODY_Y   66      /* what it will do */
#define ASK_BTN_W    112
#define ASK_BTN_H    28
#define ASK_BTN_GAP  10
#define ABOUT_H      196     /* About ThreadOS: the same card, taller */
#define ABOUT_LINE_Y 66      /* its first line of facts */
#define ABOUT_LINE_DY 20

/* --- notifications: cards at the top right, newest on top ------------- */
#define NOTE_W       280
#define NOTE_H       56
#define NOTE_R       12
#define NOTE_GAP     8
#define NOTE_TOP     (BAR_H + 10)
#define NOTE_RIGHT   10
#define NOTE_ICON    28
#define NOTE_TEXT_X  52      /* the text, from the card's left edge */
#define NOTE_TITLE_Y 23      /* the title's baseline, from its top */
#define NOTE_BODY_Y  41      /* the line under it */
#define NOTE_PX      12
#define NOTE_SHOWN   4       /* at most this many at once */

/* --- the search: a field and what matches, under the bar ----------------- */
#define SEARCH_W       460
#define SEARCH_Y       (BAR_H + 60)
#define SEARCH_FIELD_H 44
#define SEARCH_PX      17
#define SEARCH_ROW     30
#define SEARCH_ROWS    8
#define SEARCH_R       14
#define SEARCH_TEXT_X  44    /* the typing, from the panel's left edge */
#define SEARCH_ICON    20

/* --- the lock: the time, and a field for the password ------------------ */
#define LOCK_TIME_PX  64
#define LOCK_FIELD_W  240
#define LOCK_FIELD_H  36

/* --- the hint ----------------------------------------------------------- */
#define HINT_H       20
#define HINT_R       6

/* --- light ---------------------------------------------------------------- */
#define LIGHT_BAR_TINT     0xF6F6F9
#define LIGHT_BAR_A        150
#define LIGHT_BAR_TEXT     0x1D1D1F
#define LIGHT_BAR_DIM      0x55555C
#define LIGHT_TITLE_BG     0xE6E6EB
#define LIGHT_TITLE_OFF    0xF3F3F6
#define LIGHT_TITLE_TEXT   0x1D1D1F
#define LIGHT_TITLE_DIM    0x9A9AA2
#define LIGHT_WIN_BG       0xF3F3F6
#define LIGHT_WIN_LINE     0xBDBDC5
#define LIGHT_LIGHT_OFF    0xCBCBD1
#define LIGHT_MENU_BG      0xF4F4F7
#define LIGHT_MENU_A       215
#define LIGHT_MENU_TEXT    0x1D1D1F
#define LIGHT_DOCK_TINT    0xFFFFFF
#define LIGHT_DOCK_A       95
#define LIGHT_DOCK_LINE    0xFFFFFF
#define LIGHT_DOCK_LINE_A  150
#define LIGHT_DOT          0x1D1D1F
#define LIGHT_HINT_BG      0xF2F2F5
#define LIGHT_HINT_TEXT    0x1D1D1F
#define LIGHT_SHADOW_A     110
#define LIGHT_ACCENT       0x0A7AFF
#define LIGHT_ACCENT_TEXT  0xFFFFFF

/* --- dark ----------------------------------------------------------------- */
#define DARK_BAR_TINT      0x1A1A20
#define DARK_BAR_A         150
#define DARK_BAR_TEXT      0xF5F5F7
#define DARK_BAR_DIM       0xA5A5AD
#define DARK_TITLE_BG      0x34343B
#define DARK_TITLE_OFF     0x29292E
#define DARK_TITLE_TEXT    0xEDEDF0
#define DARK_TITLE_DIM     0x85858D
#define DARK_WIN_BG        0x29292E
#define DARK_WIN_LINE      0x4A4A52
#define DARK_LIGHT_OFF     0x55555D
#define DARK_MENU_BG       0x2A2A30
#define DARK_MENU_A        215
#define DARK_MENU_TEXT     0xF5F5F7
#define DARK_DOCK_TINT     0x26262C
#define DARK_DOCK_A        120
#define DARK_DOCK_LINE     0xFFFFFF
#define DARK_DOCK_LINE_A   45
#define DARK_DOT           0xF5F5F7
#define DARK_HINT_BG       0x3A3A41
#define DARK_HINT_TEXT     0xF2F2F5
#define DARK_SHADOW_A      170
#define DARK_ACCENT        0x0A84FF
#define DARK_ACCENT_TEXT   0xFFFFFF

/* --- the programs' own colours ----------------------------------------------
 * What a program draws its window in (desktop_ui_look): its background
 * is the window's body (..._WIN_BG); these are the rest. */
#define LIGHT_APP_FIELD    0xFFFFFF   /* a field, a list - a surface to fill */
#define LIGHT_APP_TEXT     0x1D1D1F
#define LIGHT_APP_DIM      0x6E6E76
#define LIGHT_APP_ROW      0xE6E6EC   /* a row under the pointer, a stripe */
#define LIGHT_APP_DANGER   0xFF3B30
#define LIGHT_APP_GOOD     0x28A745
#define DARK_APP_FIELD     0x1C1C20
#define DARK_APP_TEXT      0xEDEDF0
#define DARK_APP_DIM       0x9A9AA2
#define DARK_APP_ROW       0x34343B
#define DARK_APP_DANGER    0xFF453A
#define DARK_APP_GOOD      0x30D158

/* The colours of one theme, as the compositor uses them */
struct look {
    uint32_t bar_tint;  int bar_a;
    uint32_t bar_text, bar_dim;
    uint32_t title_bg, title_off, title_text, title_dim;
    uint32_t win_bg, win_line, light_off;
    uint32_t menu_bg;   int menu_a;
    uint32_t menu_text;
    uint32_t dock_tint; int dock_a;
    uint32_t dock_line; int dock_line_a;
    uint32_t dot, hint_bg, hint_text;
    int shadow_a;
    uint32_t accent, accent_text;
};

#define LOOK_OF(T) { T##_BAR_TINT, T##_BAR_A, T##_BAR_TEXT, T##_BAR_DIM, \
    T##_TITLE_BG, T##_TITLE_OFF, T##_TITLE_TEXT, T##_TITLE_DIM, \
    T##_WIN_BG, T##_WIN_LINE, T##_LIGHT_OFF, T##_MENU_BG, T##_MENU_A, \
    T##_MENU_TEXT, T##_DOCK_TINT, T##_DOCK_A, T##_DOCK_LINE, \
    T##_DOCK_LINE_A, T##_DOT, T##_HINT_BG, T##_HINT_TEXT, T##_SHADOW_A, \
    T##_ACCENT, T##_ACCENT_TEXT }

/* --- the aurora ------------------------------------------------------------
 *
 * The background when no picture is chosen: a slope from the top of
 * the screen to the bottom, and over it soft clouds of colour. Each
 * cloud is a centre (in thousandths of the screen's width and height),
 * a radius (in thousandths of the width), a colour and a strength; the
 * light theme has its own slope and paler clouds. kernel/aurora.c draws
 * them and the test draws them again from these same numbers, so they
 * are written as plain numbers and nothing else. */
#define AURORA_N 5
/* top colour, bottom colour */
#define AURORA_DARK_TOP     0x0B1030
#define AURORA_DARK_BOTTOM  0x05060F
#define AURORA_LIGHT_TOP    0x8FA8F0
#define AURORA_LIGHT_BOTTOM 0xE9E2F7
/* x, y, radius, colour, strength - in that order */
#define AURORA_DARK_CLOUDS \
    {  180,  250, 520, 0x7846FF, 230 }, \
    {  820,  180, 480, 0x1EC8FF, 200 }, \
    {  700,  780, 560, 0xFF46A0, 190 }, \
    {  250,  900, 420, 0x5AFFC8, 140 }, \
    {  520,  430, 300, 0x3A5BFF, 120 }
#define AURORA_LIGHT_CLOUDS \
    {  180,  250, 520, 0xB49CFF, 220 }, \
    {  820,  180, 480, 0x8EE6FF, 210 }, \
    {  700,  780, 560, 0xFFA6D2, 200 }, \
    {  250,  900, 420, 0xA8FFE0, 170 }, \
    {  520,  430, 300, 0xC9D4FF, 150 }

#endif
