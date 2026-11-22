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
#define BAR_LOGO_D   14      /* ... and how large it is */
#define BAR_NAME_X   30      /* the program in front, in bold */
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
/* "The camera is on": a green dot left of Bluetooth while a picture is
   being taken - drawn by the desktop from the driver's own state, so a
   program cannot hide it */
#define CAM_DOT_W    12      /* its place on the bar */
#define CAM_DOT_D    8       /* the dot */
#define CAM_DOT_RGB  0x30D158
/* ... and an orange one left of it while the microphone listens */
#define MIC_DOT_RGB  0xFF9F0A

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

/* The Windows style ('layout windows'): no bar at the top, a taskbar
   along the bottom - Start, the programs and the windows in the middle
   (as Windows 11 has them), the tray and the clock at the right - and
   a window's buttons at the right end of its title bar */
#define TB_H         40      /* the taskbar's height */
#define TB_SLOT      40      /* one button on it: Start, a program, a window */
#define TB_LOGO_D    20      /* the mark on Start */
#define TB_ICON      24      /* the picture in it */
#define TB_SHOW_W    6       /* "show the desktop", the sliver at the right */
#define TB_CLOCK_W   76      /* the time over the date */
#define TB_TRAY_W    28      /* one icon in the tray */
#define WB_W         34      /* minimise, maximise, close: one button */

/* the three lights: the same in both themes, because they are signals */
#define LIGHT_CLOSE  0xFF5F57
#define LIGHT_MIN    0xFEBC2E
#define LIGHT_ZOOM   0x28C840

/* --- Weave's dyes (docs/WEAVE.md) ------------------------------------------
   Six colours named after real dyes, each with one job, in the dark
   theme (Night) and the light one (Linen). Programs get them through
   SYS64_UI_LOOK (ProtonUI v2, user/udraw.h) in this order. */
#define DYE_INDIGO_N     0x5B7CFA    /* focus, the one thread; primary buttons */
#define DYE_INDIGO_L     0x3F5EE8
#define DYE_MADDER_N     0xE5484D    /* close, delete, errors */
#define DYE_MADDER_L     0xC9302C
#define DYE_SAFFRON_N    0xF5A524    /* warnings, the microphone is on */
#define DYE_SAFFRON_L    0xB86E00
#define DYE_WELD_N       0x46C27A    /* done, connected, the camera is on */
#define DYE_WELD_L       0x1E8A4C
#define DYE_WOAD_N       0x2EC4C4    /* links, the network, Weft */
#define DYE_WOAD_L       0x1E7F86
#define DYE_COCHINEAL_N  0xD152A6    /* media: music, pictures, sound */
#define DYE_COCHINEAL_L  0xC2366F

/* The grain: a 3x3 tile copied over a flat fill, at this much ink of
   255 (about 2 %) - under Weave only; the other styles have none */
#define GRAIN_TILE       3
#define GRAIN_INK        5
/* A control's corners (UG_R in user/ugui.h) */
#define CONTROL_R        6
/* How a window is used: with a pointer, or with a finger (the phone,
   a tablet in Touch mode) - rows and targets grow for a finger */
#define DENSITY_POINTER  0
#define DENSITY_FINGER   1

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

#define VOL_R        14      /* the corners of the status card */
#define SESS_ROW     22      /* an entry below it, a little lower */

/* --- the island: a capsule at the top centre for what is going on ----- */
#define ISL_TOP      8       /* from the top edge of the screen */
#define ISL_VOL_W    300     /* how loud: the wide capsule */
#define ISL_VOL_H    44
#define ISL_ICON_X   26      /* the speaker's middle, from the left end */
#define ISL_BAR_X    52      /* the slider, from the left end */
#define ISL_BAR_W    176
#define ISL_BAR_H    6
#define ISL_GRAB     10      /* how far above and below the slider grabs */
#define ISL_TEXT_X   244     /* the number, from the left end */
#define ISL_PX       14
#define ISL_PLAY_W   260     /* something is playing: the small capsule */
#define ISL_PLAY_H   32
#define ISL_NAME_X   16      /* its program's name, from the left end */
#define ISL_LEV_W    4       /* a level bar, and the gap after it */
#define ISL_LEV_GAP  3
#define ISL_LEV_RIGHT 16     /* the last bar's right edge, from the right end */
#define ISL_LEV_MIN  3       /* a bar for silence */
#define ISL_LEV_MAX  18      /* and for a full-scale sample */
#define ISL_NP_W     24      /* play or pause, when the program said what it
                                plays: a square left of the bars */
#define ISL_NP_GAP   8       /* between it and the first bar */
#define ISL_NP_HOLD  5       /* seconds it stays once paused from it, to
                                play again - then the island goes */

/* --- the question before logging out, restarting, shutting down ------- */
#define ASK_W        320
#define ASK_H        150
#define ASK_R        14
#define ASK_PAD      20
#define ABOUT_LOGO_D 24      /* the mark on the About box's disc */
#define ASK_TITLE_Y  40      /* the question's baseline, from the top */
#define ASK_BODY_Y   66      /* what it will do */
#define ASK_BTN_W    112
#define ASK_BTN_H    28
#define ASK_BTN_GAP  10
#define ABOUT_H      196     /* About ThreadOS: the same card, taller */
#define ABOUT_LINE_Y 66      /* its first line of facts */
#define ABOUT_LINE_DY 20

/* --- notifications: in the island, the newest shown, the rest behind -- */
#define NOTE_SHOWN   4       /* at most this many riding the island at once */
#define NOTE_KEPT    32      /* ... and kept, riding or waiting in Threads */
#define ACT_MAX      4       /* activities on the shuttle (one per window) */
#define ISL_NOTE_W   340     /* the capsule a notification makes */
#define ISL_NOTE_H   52
#define ISL_NOTE_ICON 30     /* the sender's initial, on the accent */
#define ISL_NOTE_ICON_X 12
#define ISL_NOTE_TEXT_X 54   /* the title and the line, from the left end */
#define ISL_NOTE_TITLE_Y 22  /* the title's baseline, from the top */
#define ISL_NOTE_BODY_Y 40   /* the line's */
#define ISL_NOTE_PX  13
#define ISL_MORE_RIGHT 18    /* "+2" (more behind it) ends this far from the right end */

/* --- Weave's shuttle (docs/weave/canvas/Shuttle.dc.html): the island
   above the loom, joined to it by SHT_LIFT of thread in the colour of
   whose it is - nothing at the top of the screen, over a window's
   title. It rests as a short pill (SHT_REST_*) and grows out of it into
   a capsule for what it carries: */
#define SHT_LIFT      22
#define SHT_REST_W    30
#define SHT_REST_H    6
#define SHT_VOL_W     340     /* how loud */
#define SHT_VOL_H     48
#define SHT_VOL_PAD   18
#define SHT_BAR_X     50      /* the slider, from the left end */
#define SHT_BAR_W     236
#define SHT_PLAY_W    380     /* what plays: art, title, levels, a button */
#define SHT_PLAY_H    52
#define SHT_ART       34
#define SHT_BTN       36
#define SHT_NOTE_W    460     /* a note: who, its line, its actions */
#define SHT_NOTE_H    66
#define SHT_NOTE_R    22
#define SHT_AVATAR    42
#define SHT_TEXT_X    66      /* its lines, from the left end */
#define SHT_LINE1_Y   28      /* their baselines, from the top */
#define SHT_LINE2_Y   47
#define SHT_ACT_H     34      /* an action: a pill */
#define SHT_ACT_PAD   14
#define SHT_ACT_GAP   8
#define SHT_ACT_RIGHT 10
#define SHT_ACT_PX    13
#define SHT_DL_W      360     /* a download, a call */
#define SHT_DL_H      48
#define SHT_DL_X      52      /* their lines, from the left end */
#define SHT_TIMER_H   44      /* a timer: as wide as its title */
#define SHT_RING      30
#define SHT_URGENT_T  2       /* an Urgent note's madder edge */
#define NIGHT_SHT_BG    0x1F2229
#define NIGHT_SHT_LINE  0x353A46
#define NIGHT_SHT_TRACK 0x3A3F4C
#define LINEN_SHT_BG    0xFFFFFF
#define LINEN_SHT_LINE  0xCFC8BA
#define LINEN_SHT_TRACK 0xCFC8BA

/* --- Weave's Threads (docs/weave/canvas/Notify.dc.html): the notes that
   wait, one thread a program, in a panel above the loom's right end,
   opened from the clock. A thread is a card: its knot, its name, how
   many; opened, its notes on it, the newest's actions and Mark read. */
#define THR_W         380
#define THR_R         16
#define THR_GAP       14      /* from the screen's top and right edge */
#define THR_HEAD_H    56      /* the selvedge, the title, Quiet, Clear all */
#define THR_TITLE_PX  18
#define THR_PILL_H    28
#define THR_PILL_PX   12
#define THR_PAD       12      /* the cards from the panel's sides */
#define THR_CARD_R    14
#define THR_CARD_GAP  10
#define THR_ROW_H     40      /* a thread's line: knot, name, count */
#define THR_KNOT      14
#define THR_NAME_PX   13
#define THR_ITEM_H    46      /* a note on it */
#define THR_ITEM_X    36      /* its lines, from the card's left */
#define THR_ITEMS     4       /* the most shown on one thread */
#define THR_FOOT_H    44      /* the actions and Mark read */
#define THR_BTN_H     32
#define THR_BANNER_H  34      /* "Quiet: ..." under the title */
#define NIGHT_THR_CARD 0x20232B
#define THR_QUIET_DYE  0x6A4FD6   /* Quiet's knot: the design's violet */
#define LINEN_THR_CARD 0xFFFFFF

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

/* --- Weave (docs/WEAVE.md): cloth, not glass ---------------------------------
 * The third style ('layout weave'): every surface opaque with the grain
 * over it, the focus one thread of colour. Its two themes are Night and
 * Linen; the fields are the same as the two above, so the compositor
 * takes either set as it takes those. Nothing is see-through: the
 * alphas are whole, which is what makes cloth cheap. */
#define NIGHT_BAR_TINT     0x181A20
#define NIGHT_BAR_A        255
#define NIGHT_BAR_TEXT     0xECEDF2
#define NIGHT_BAR_DIM      0x8A90A0
#define NIGHT_TITLE_BG     0x21242C
#define NIGHT_TITLE_OFF    0x1B1D24
#define NIGHT_TITLE_TEXT   0xECEDF2
#define NIGHT_TITLE_DIM    0x7D8391
#define NIGHT_WIN_BG       0x1A1C23
#define NIGHT_WIN_LINE     0x2E323D
#define NIGHT_LIGHT_OFF    0x5F6573
#define NIGHT_MENU_BG      0x1F2229
#define NIGHT_MENU_A       255
#define NIGHT_MENU_TEXT    0xECEDF2
#define NIGHT_DOCK_TINT    0x181A20
#define NIGHT_DOCK_A       255
#define NIGHT_DOCK_LINE    0x2E323D
#define NIGHT_DOCK_LINE_A  255
#define NIGHT_DOT          0x9AA0AE
#define NIGHT_HINT_BG      0x2A2D36
#define NIGHT_HINT_TEXT    0xECEDF2
#define NIGHT_SHADOW_A     140
#define NIGHT_ACCENT       0x5B7CFA   /* Indigo */
#define NIGHT_ACCENT_TEXT  0xFFFFFF
#define NIGHT_GROUND       0x0D0F15   /* the warp's dark loom */

#define LINEN_BAR_TINT     0xEFEBE3
#define LINEN_BAR_A        255
#define LINEN_BAR_TEXT     0x1D1C1A
#define LINEN_BAR_DIM      0x6E6A62
#define LINEN_TITLE_BG     0xEFEBE3
#define LINEN_TITLE_OFF    0xF3F0E9
#define LINEN_TITLE_TEXT   0x1D1C1A
#define LINEN_TITLE_DIM    0x8E897F
#define LINEN_WIN_BG       0xF6F3EC
#define LINEN_WIN_LINE     0xCFC8BA
#define LINEN_LIGHT_OFF    0xB4AEA2
#define LINEN_MENU_BG      0xF6F3EC
#define LINEN_MENU_A       255
#define LINEN_MENU_TEXT    0x1D1C1A
#define LINEN_DOCK_TINT    0xEFEBE3
#define LINEN_DOCK_A       255
#define LINEN_DOCK_LINE    0xCFC8BA
#define LINEN_DOCK_LINE_A  255
#define LINEN_DOT          0x6E6A62
#define LINEN_HINT_BG      0xFFFFFF
#define LINEN_HINT_TEXT    0x1D1C1A
#define LINEN_SHADOW_A     80
#define LINEN_ACCENT       0x3F5EE8
#define LINEN_ACCENT_TEXT  0xFFFFFF
#define LINEN_GROUND       0xE9E4DA

/* Weave's wallpaper, the warp (kernel/aurora.c): the ground, a faint
   6x6 weave over it (a warp thread down every sixth column at
   WARP_COL_INK, a weft thread along every sixth row, from the third,
   at WARP_ROW_INK - white on Night, black on Linen), and threads of
   the dyes crossing it. Each thread: its dye (0..5 as DYE_*, 6 the
   person's own thread colour), a glow's width (sixteenths of a pixel)
   and strength (of 255, 0: none), its core's width and strength, then
   two cubic curves joined smoothly - P0, C1, C2, P1, C2', P2 in
   thousandths of the screen's width and height, the second curve's
   first pull being the first's second mirrored through P1 */
#define WARP_COL_INK       6
#define WARP_ROW_INK       4
#define WARP_N             5
#define WARP_THREADS { \
    { 3, 192, 20, 24,  89, { -28, 578, 264, 422, 319, 733, 528, 522, 833, 211, 1042, 256 } }, \
    { 5, 224, 23, 32, 128, { -28, 644, 236, 500, 333, 800, 549, 589, 847, 278, 1042, 333 } }, \
    { 6, 256, 31, 48, 191, { -28, 711, 208, 600, 361, 867, 569, 644, 861, 356, 1042, 411 } }, \
    { 4, 192, 23, 32, 128, { -28, 778, 181, 689, 389, 933, 597, 700, 875, 433, 1042, 489 } }, \
    { 2,   0,  0, 24,  82, { -28, 844, 208, 800, 417, 978, 625, 756, 889, 522, 1042, 567 } } }

/* Weave's icons (kernel/weave.c, from docs/weave/canvas/Icon.dc.html):
   corners a quarter of the tile and a bit (percent), the grain - a dark
   warp every third column at WEAVE_WARP_INK, a light weft every third
   row from the bottom at WEAVE_WEFT_INK over it - a dark last row at
   WEAVE_EDGE_INK, and the thread along the foot: white at
   WEAVE_TINT_A, or WEAVE_GREY_TINT at WEAVE_GREY_TINT_A on the two
   grey dyes. Inks of 255. */
#define WEAVE_ICON_R       26
#define WEAVE_WARP_INK     15
#define WEAVE_WEFT_INK     18
#define WEAVE_EDGE_INK     46
#define WEAVE_TINT_A       195
#define WEAVE_GREY_TINT    0x9AA0AE
#define WEAVE_GREY_TINT_A  217
#define WEAVE_LETTER_DYE   0x4B505E

/* Weave's sizes */
#define SELVEDGE_H         3       /* the thread along the window in front */
/* The loom along the bottom (docs/weave/canvas/Main.dc.html), LOOM_SIDE
   from the screen's sides and LOOM_GAP from its foot, a pixel's line
   round it. Inside, LOOM_PAD from its ends and LOOM_SPACE apart: the
   spool, the field that finds anything (on a screen too narrow for it,
   a button of LOOM_FIND_H square), the programs - icons LOOM_ICON
   across, LOOM_ICON_GAP apart, LOOM_ICON_Y from the loom's top, smaller
   where they would not fit, on a thread LOOM_LINE_Y down - the
   desktops' knots, the tray (network, sound, the battery: pictures
   LOOM_GLYPH across, LOOM_TRAY_GAP apart) and, behind a stitch, the
   clock */
#define LOOM_H             58
#define LOOM_SIDE          16
#define LOOM_GAP           14
#define LOOM_R             14
#define LOOM_PAD           10
#define LOOM_SPACE         14
#define LOOM_SPOOL         40
#define LOOM_FIND_W        220
#define LOOM_FIND_H        36
#define LOOM_ICON          38
#define LOOM_ICON_GAP      10
#define LOOM_ICON_MIN      24
#define LOOM_ICON_Y        5
#define LOOM_LINE_Y        49
#define LOOM_THREAD_RUN    14      /* the thread under a program that runs */
#define LOOM_THREAD_FOCUS  30      /* ... and under the one in front */
#define LOOM_KNOT_D        10      /* a desktop's knot */
#define LOOM_KNOT_GAP      7
#define LOOM_GLYPH         18
#define LOOM_TRAY_GAP      12
#define LOOM_BATT_W        54      /* the battery and its percentage */
#define LOOM_CLOCK_W       84      /* the stitch, the time over the date */
#define LOOM_CLOCK_PX      15
#define LOOM_DATE_PX       11
#define LOOM_FIND_BG       0x121419   /* Night's field */
#define LOOM_FIND_LINE     0x3A3F4C   /* the stitch under it */
#define LOOM_TRAY_INK      0xC9CDD8
#define LOOM_KNOT_EMPTY    0x3A3F4C   /* a desktop with nothing on it */
#define LOOM_KNOT_USED     0x8A90A0   /* ... with windows */
/* A window in Weave (docs/weave/canvas/Main.dc.html): the selvedge,
   then a bar of its own height, the window's corners rounder than the
   other styles'. In the bar from the left, WEAVE_BAR_LEFT in: the
   program's icon, WEAVE_TITLE_GAP, its name in the bold and what the
   title says after it; at the right, WEAVE_CAP_RIGHT from the edge, the
   capsule - WEAVE_CAP_PAD round three round buttons WEAVE_BTN across,
   WEAVE_BTN_GAP apart: minimise, maximise, close */
#define WEAVE_BAR_H        38
#define WEAVE_TITLE_H      41      /* SELVEDGE_H + WEAVE_BAR_H */
#define WEAVE_WIN_R        10
#define WEAVE_BAR_LEFT     12
#define WEAVE_TITLE_ICON   18
#define WEAVE_TITLE_GAP    10
#define WEAVE_TITLE_PX     13
#define WEAVE_CAP_RIGHT    8
#define WEAVE_CAP_PAD      3
#define WEAVE_BTN          26
#define WEAVE_BTN_GAP      2
#define WEAVE_BTN_INK      0xB4B9C6   /* the buttons' signs, Night */
#define WEAVE_BTN_DIM      0x5F6573   /* ... on a window without focus */
#define WEAVE_CLOSE_INK    0xFF6B6F   /* close's, on its madder ground */
#define WEAVE_SHADOW_S     26      /* the shadow of the window in front ... */
#define WEAVE_SHADOW_DY    12      /* ... and how far it falls */
/* A choice a program puts in its title bar (SYS64_WIN_SEGMENTS - the
   Night | Linen of docs/weave/canvas/Swatch.dc.html): TSEG_GAP left of
   the buttons (of the bar's end, where they are on the left), a group
   TSEG_H high, TSEG_PAD inside its edge, each word TSEG_WORD_PAD to
   either side, the chosen one filled. The other styles' bars are lower:
   TSEG_H_SMALL high, its words a size smaller and closer. */
#define TSEG_MAX           4
#define TSEG_H             34
#define TSEG_PAD           4
#define TSEG_R             9
#define TSEG_ON_R          6
#define TSEG_WORD_PAD      12
#define TSEG_PX            12
#define TSEG_GAP           10
#define TSEG_H_SMALL       18
#define TSEG_PAD_SMALL     2
#define TSEG_WORD_SMALL    8
#define TSEG_PX_SMALL      11
#define NIGHT_TSEG_BG      0x17191F
#define NIGHT_TSEG_LINE    0x2E323D
#define NIGHT_TSEG_ON      0x2A2D36
#define NIGHT_TSEG_ON_INK  0xFFFFFF
#define NIGHT_TSEG_INK     0xB4B9C6
#define LINEN_TSEG_BG      0xEFEBE3
#define LINEN_TSEG_LINE    0xCFC8BA
#define LINEN_TSEG_ON      0xFFFFFF
#define LINEN_TSEG_ON_INK  0x1D1C1A
#define LINEN_TSEG_INK     0x5E5A52

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

#define NIGHT_APP_FIELD    0x121419
#define NIGHT_APP_TEXT     0xECEDF2
#define NIGHT_APP_DIM      0x8A90A0
#define NIGHT_APP_ROW      0x262A33
#define NIGHT_APP_DANGER   0xE5484D   /* Madder */
#define NIGHT_APP_GOOD     0x46C27A   /* Weld */
#define LINEN_APP_FIELD    0xFFFFFF
#define LINEN_APP_TEXT     0x1D1C1A
#define LINEN_APP_DIM      0x6E6A62
#define LINEN_APP_ROW      0xE6E1D6
#define LINEN_APP_DANGER   0xC9302C
#define LINEN_APP_GOOD     0x1E8A4C

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
