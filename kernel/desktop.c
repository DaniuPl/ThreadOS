/* The ThreadOS graphical desktop - a simple compositor
 *
 * It runs as a kernel thread: in a loop it redraws the whole frame
 * into the drawing buffer (background -> bar -> windows -> dock ->
 * cursor), hands it to the screen and gives up the CPU for ~30 ms.
 * Only the rows that changed cross over to the screen (see video.c),
 * so a frame that is drawn again identically costs the bus nothing.
 *
 * What it looks like is in kernel/include/look.h: a menu bar of glass
 * along the top, windows with round corners on soft shadows and their
 * three buttons as coloured lights, and a dock at the bottom that
 * starts programs and holds the open windows. The soft edges are
 * kernel/gfx.c's, the letters kernel/uitext.c's, and the background,
 * unless a picture is chosen, kernel/aurora.c's.
 */

#include <stdint.h>
#include "include/desktop.h"
#include "include/video.h"
#include "include/vga.h"
#include "include/mouse.h"
#include "include/task.h"
#include "include/heap.h"
#include "include/string.h"
#include "include/font8x8.h"
#include "include/uitext.h"
#include "include/gfx.h"
#include "include/look.h"
#include "include/aurora.h"
#include "include/acpi.h"
#include "include/io.h"
#include "include/keyboard.h"
#include "include/exec.h"
#include "include/fs.h"
#include "include/io.h"
#include "include/config.h"
#include "include/rtc.h"
#include "include/printk.h"
#include "include/pmm.h"
#include "include/pit.h"
#include "include/spinlock.h"

/* Palette indices.
 *
 * The desktop itself now draws in real colours. The palette is still
 * set, for two reasons: every program draws its window in palette
 * indices, and those indices have meant these colours since the first
 * window was opened - and on a screen of two hundred and fifty six
 * colours, which this machine falls back to when there is no
 * framebuffer, the palette is all there is.
 */
#define COL_BLACK  0
#define COL_TITLE  3      /* the accent (blue) */
#define COL_WHITE  4
#define COL_DESK   6      /* the desktop background */
#define COL_BAR    7      /* light grey */
#define COL_WIN    8      /* the window background (grey) */
#define COL_TBBTN  9      /* dark grey */
#define COL_ICON   10     /* orange */
#define COL_LINE     11   /* the line between one surface and the next */
#define COL_WIN_HI   12   /* a lighter surface */
#define COL_TITLE_OFF 13  /* a window without focus, in 256 colours */
#define COL_TBBTN_OFF 14  /* a quiet grey */
#define COL_DIM      15   /* text without weight */
#define COL_ICON_LO  16

/* The background in 256 colours: sixteen bands darkening towards the
   floor, sixteen entries. On a screen of real colours the slope is
   smooth and these are not used. */
#define COL_DESK0    20
#define COL_DESK_N   16

/* The colours in use: one of the two sets in look.h */
static const struct look look_dark = LOOK_OF(DARK);
static const struct look look_light = LOOK_OF(LIGHT);
static struct look L = LOOK_OF(DARK);

/* The programs the dock starts: what each is called, what it runs and
 * which of the pictures below it wears.
 *
 * The list of them used to be this array and nothing else - programs
 * decided when the kernel was built. It is now a file on the disk (see
 * icons_read_list), and this array is what a machine without that
 * file gets. */
#define ICON_MAX 12
struct icon {
    const char *label;
    const char *app;
    int art;                   /* which picture */
    char store[2][24];         /* room for a label and a name read from
                                  the disk - the built-in ones point at
                                  their own literals */
};
static struct icon icons[ICON_MAX] = {
    {"App", "guiapp", 0, {{0}}},
    {"Clock", "gclock", 1, {{0}}},
    {"Files", "gfiles", 2, {{0}}},
    {"Monitor", "gmon", 3, {{0}}},
    {"Term", "gterm", 4, {{0}}},
    {"Web", "gweb", 5, {{0}}},
    {"Settings", "gsettings", 6, {{0}}},
    {"Editor", "gedit", 7, {{0}}},
    {"Tasks", "gtasks", 8, {{0}}},
};
static int icon_n = 9;         /* how many of them there are */

/* How many pictures there are (see draw_art): one per program that
   comes with the system, and a last one - a letter - for anything
   else. */
#define ART_N      12
#define ART_LETTER 11

/* The programs that come with the system, by the name they run under:
   the picture each wears and what a person calls it - for a program
   that is not in the dock, say one started from the menu */
static const struct { const char *app, *label; } known_apps[] = {
    { "guiapp", "App" }, { "gclock", "Clock" }, { "gfiles", "Files" },
    { "gmon", "Monitor" }, { "gterm", "Term" }, { "gweb", "Web" },
    { "gsettings", "Settings" }, { "gedit", "Editor" },
    { "gtasks", "Tasks" }, { "gmikser", "Mixer" },
    { "gappear", "Appearance" },
};
#define KNOWN_N ((int)(sizeof(known_apps) / sizeof(known_apps[0])))

static int art_of_app(const char *app)
{
    for (int k = 0; k < KNOWN_N; k++)
        if (!strcmp(app, known_apps[k].app))
            return k;
    return ART_LETTER;
}

static const char *label_of_app(const char *app)
{
    int k = art_of_app(app);
    return k < KNOWN_N ? known_apps[k].label : app;
}

#define ICON_N icon_n

static int icon_of_app(const char *app)
{
    for (int k = 0; k < ICON_N; k++) {
        const char *a = icons[k].app, *b = app;
        while (*a && *a == *b) {
            a++;
            b++;
        }
        if (*a == *b)
            return k;
    }
    return -1;
}

/* Which picture belongs to which running program.
 *
 * A window knows the process that owns it and nothing else - not what
 * was clicked to start it. So the desktop remembers, when it launches
 * something, which icon it came from; the window created a moment
 * later by that process takes the same picture. Nothing else can
 * connect the two: the compositor sees a pid, and the dock's tiles are the
 * only place where a program has a face.
 */
#define LAUNCH_MAX 16
static struct { int pid; int icon; int art; } launched[LAUNCH_MAX];
static int launch_next;

/* 'icon' is the dock's tile it came from, or -1 when it came from the
   menu; 'art' is the picture either way */
static void remember_launch(int pid, int icon, int art)
{
    if (pid < 0)
        return;
    launched[launch_next].pid = pid;
    launched[launch_next].icon = icon;
    launched[launch_next].art = art;
    launch_next = (launch_next + 1) % LAUNCH_MAX;
}

/* When each program was last started from the dock: its tile hops
   for a moment, which is how a click that takes a second to show a
   window is seen to have been heard */
#define HOP_FRAMES 16
static uint32_t hop_at[ICON_MAX];
static int hop_set[ICON_MAX];

/* The tile a program was started from, or -1 */
static int icon_of_pid(int pid)
{
    for (int i = 0; i < LAUNCH_MAX; i++)
        if (launched[i].pid == pid)
            return launched[i].icon;
    return -1;
}

/* The picture a program was started with, or -1 */
static int art_of_pid(int pid)
{
    for (int i = 0; i < LAUNCH_MAX; i++)
        if (launched[i].pid == pid)
            return launched[i].art;
    return -1;
}

/* The file that says which programs the dock starts: one line each,
 *
 *     program  label  picture-number
 *
 * and nothing else. Lines that do not parse are passed over, an empty
 * or missing file leaves the built-in nine - a dock with nothing in it
 * would be a machine with nothing to click, and that is not something
 * a broken line should be able to do.
 *
 * It is plain text on purpose, like everything else the system keeps:
 * it can be read with 'cat' and built with 'echo ... >> desktop.icons'
 * from the shell in the terminal.
 */
#define ICON_LIST_FILE "desktop.icons"
#define ICON_LIST_MAX 512

static int icons_read_list(void)
{
    if (!fs_ready())
        return 0;

    char buf[ICON_LIST_MAX];
    int n = fs_read(ICON_LIST_FILE, buf, sizeof(buf) - 1);
    if (n <= 0)
        return 0;
    buf[n] = 0;

    int count = 0;
    int i = 0;
    while (i < n && count < ICON_MAX) {
        int start = i;
        while (i < n && buf[i] != '\n')
            i++;
        int end = i;
        if (i < n)
            i++;
        if (end > start && buf[end - 1] == '\r')
            end--;

        /* Three words: the program, what to call it, which picture */
        char *word[3];
        int nw = 0;
        int k = start;
        while (k < end && nw < 3) {
            while (k < end && buf[k] == ' ')
                k++;
            if (k >= end)
                break;
            word[nw++] = &buf[k];
            while (k < end && buf[k] != ' ')
                k++;
            if (k < end)
                buf[k++] = 0;
        }
        buf[end] = 0;
        if (nw < 3 || word[0][0] == '#')
            continue;                  /* a comment, or not three words */

        int art = 0;
        for (const char *p = word[2]; *p >= '0' && *p <= '9'; p++)
            art = art * 10 + (*p - '0');
        if (art >= ART_N)
            art = ART_LETTER;

        struct icon *ic = &icons[count];
        int j = 0;
        for (; word[1][j] && j < (int)sizeof(ic->store[0]) - 1; j++)
            ic->store[0][j] = word[1][j];
        ic->store[0][j] = 0;
        j = 0;
        for (; word[0][j] && j < (int)sizeof(ic->store[1]) - 1; j++)
            ic->store[1][j] = word[0][j];
        ic->store[1][j] = 0;

        ic->label = ic->store[0];
        ic->app = ic->store[1];
        ic->art = art;
        count++;
    }

    if (!count)
        return 0;                      /* nothing usable - keep the nine */
    icon_n = count;
    printk("[desktop] icons from the file '%s': %d\n", ICON_LIST_FILE, count);
    return 1;
}

#define COL_RED    1      /* the close button */

/* An event for an application (layout matching struct win_event in usys.h) */
struct wevent {
    int32_t type;          /* 0 = none, 1 = a mouse click */
    int32_t x, y;          /* coordinates relative to the content area */
    int32_t val;           /* the button mask */
};
#define WEV_NONE  0
#define WEV_CLICK 1
#define WEV_KEY   2
/* The window is being closed - the application is being ASKED to end.
   The compositor does not drop it from under a running process: first
   it says so, and only insists if nothing happens. */
#define WEV_CLOSE 3
/* The content area has a new size; x and y carry it. Sent once, when
   the drag ends - not on every frame of it. */
#define WEV_SIZE  4

/* Dragging inside a window's content: the button went down, the
   pointer moved with it held, the button came up. x and y are in the
   content's own coordinates, as with a click.
 *
 * A click alone says WHERE something happened; these three say what
 * happened BETWEEN two places, which is what selecting text is. The
 * compositor knows nothing about selections - it reports the pointer,
 * and what that means is the application's business. */
#define WEV_PRESS   5
#define WEV_DRAG    6
#define WEV_RELEASE 7
/* The wheel turned over the window - two fingers on a touchpad, most
   of the time. 'val' is how many notches and which way: positive is
   away from the person, which on a screen means towards the top of
   the document. x and y say where the pointer was, because a list and
   the panel beside it are two things to scroll and only the pointer
   says which one was meant. */
#define WEV_SCROLL  8
#define WEV_QLEN  8

struct window {
    int x, y, w, h;
    char title[24];
    int used;
    int has_button;        /* does the window have an action button */
    int counter;           /* the button's click counter */
    int owner;             /* the owning process's pid; -1 = a kernel window */
    uint8_t *content;      /* the application's content buffer (NULL = a kernel window) */
    /* What one pixel of that buffer means. A window starts holding
       palette indices, one byte each - which is what every program
       here drew in while the screen had two hundred and fifty six
       colours. A program that has better colours to show than that
       asks for the other kind, and then a pixel is four bytes of
       0x00RRGGBB. The compositor does not care which: it composes one
       with a row copy and the other a pixel at a time. */
    int truecolour;
    uint32_t content_bytes;   /* what was taken for it, so it can go back */
    int cw, ch;            /* dimensions of the content area */
    int z;                 /* drawing order (higher = on top) */
    /* Which desktop the window belongs to. A window is only ever on
       one of them - it is not hidden on the others, it is not THERE. */
    int desk;
    int minimized;         /* is it minimised (hidden, visible on the bar) */
    /* Maximised: the window fills the room between the bar and the dock. The
       geometry it came from is kept here, because restoring it has to
       put the window back exactly where it was - not somewhere
       reasonable. */
    int maxed;
    int rx, ry, rw, rh;
    /* The application has been asked to close and has not yet ended.
       'closing' counts down the frames it has left; when it runs out we
       stop asking - an application that does not answer must not hold
       the screen forever. */
    int closing;
    /* A window on its way: opening, going down into the dock or coming
       back out of it (ANIM_*), since frame 'anim_at'. What it means for
       the window happens at once - a minimised window is minimised the
       moment the light is clicked; the animation is only what the eye
       is shown of it. */
    int anim;
    uint32_t anim_at;
    struct wevent evq[WEV_QLEN];  /* the event queue for the application */
    int ev_head, ev_tail;
};

/* How many frames the desktop draws in a second (see "The beat"):
   everything below that is counted in frames is a time, and is written
   as one */
#define FRAME_HZ 50

/* How long an application has to end after being asked: two seconds -
   long enough for a program that is doing something else, short enough
   that a hung one does not hold the screen. */
#define CLOSE_FRAMES (2 * FRAME_HZ)

/* How many windows the compositor holds at once.
 *
 * Two of them are the kernel's own, so this is the number of
 * applications plus two - and there are seven icons on the desktop. Six
 * meant that opening a fifth application failed for no reason a person
 * could see. */
#define MAX_WIN 12
static struct window wins[MAX_WIN];

/* Drag state: the window index and the grab (the cursor's offset
   within the window) */
static int resize_content(struct window *w);
static void toggle_max(int i);
static void close_window(int i);
static void snap_window(int i, int where);

/* Frames since the desktop started - the compositor's own clock, and
   the only one it needs: everything here happens in frames. */
static uint32_t frames;

static int drag_idx = -1;
static int resize_idx = -1;
/* A drag INSIDE a window's content: the slot whose application is
   being dragged in, or -1. Kept apart from drag_idx, which moves the
   window itself - here the window stays put and the application is
   told where the pointer goes. */
static int content_drag = -1;
static int drag_dx, drag_dy;
/* A press on a maximised window's bar: it becomes a drag - and the
   window its own size again - only once the pointer really moves.
   Undone on the press itself, the second click of a double click
   found the window already somewhere else, under the pointer, instead
   of putting it back where it was. 1 = waiting to see; the press was
   at (unmax_mx, unmax_my). */
static int unmax_pending;
static int unmax_mx, unmax_my;
static int prev_pressed;    /* the previous left-button state - to detect a click */
/* Where the pointer was on the last pass. Drawing needs it for one
   thing only: the entry the cursor is over is drawn differently, which
   is what tells a list from a menu. */
static int mouse_x, mouse_y;
static int focus_idx = -1;  /* the window with keyboard focus */

/* The launch menu.
 *
 * Opened from the mark at the left end of the bar it hangs under it;
 * opened with the right button on the desktop it stands where the
 * pointer is - the same list either way, because a second list would
 * be a second thing to learn. */
static int menu_open;
static int menu_px, menu_py;
/* The installer is deliberately NOT here. It takes the whole screen
   and everything else has to be out of reach while it runs, so it is
   started as a kiosk ('install') and not as one more window among
   somebody's other windows. */
static const char *menu_items[] = {"guiapp", "gclock", "gfiles", "gweb",
                                  "gedit", "gtasks", "gmikser", "gappear"};

/* --- The kiosk -----------------------------------------------------
 *
 * One program, the whole screen, and no way to anything else. This is
 * what an installer is: while a machine is being installed onto,
 * having a dock and a menu and somebody's other windows within reach
 * is not a convenience, it is a way to click something else by
 * mistake in the middle of writing to a disk.
 *
 * So in this mode the compositor draws no bar, no dock, no menu and
 * no other window, starts exactly one program and puts it in the
 * middle of an empty screen. The only thing the environment itself
 * offers is a way out of it: restarting the machine, which is what one
 * does after installing anyway.
 */
static int kiosk;
static const char *kiosk_app;
static int kiosk_launches;         /* how many times we have started it */
static int kiosk_wait;             /* frames before starting it again */
static int kiosk_gave_up;

#define KIOSK_BTN_W 152
#define KIOSK_BTN_H 20
#define KIOSK_TRIES 3

static int kiosk_btn_x(void) { return video_w() - KIOSK_BTN_W - 8; }
static int kiosk_btn_y(void) { return 8; }
#define MENU_N ((int)(sizeof(menu_items) / sizeof(menu_items[0])))

/* Virtual desktops.
 *
 * Four of them, and a window belongs to exactly one. It is the oldest
 * answer there is to a screen that has run out of room: instead of a
 * dozen windows in one heap, four small heaps that are switched
 * between - and switching costs nothing, because nothing is drawn,
 * moved or resized. A window on another desktop keeps running and
 * keeps drawing into its own buffer; it is simply not on the screen.
 *
 * Which one is showing is drawn on the right of the menu bar, because
 * that is where a thing that is always true belongs - a person has to
 * be able to see WHERE they are without pressing anything. */
#define DESK_N 4
static int cur_desk;

/* --- the right-hand end of the bar ------------------------------------
 *
 * From the right: the clock, the four desktops, how much of the memory
 * is in use, and "show the desktop". Each has a fixed place - the
 * clock a fixed width - so nothing on the bar moves because a digit
 * changed. */
static int clock_x(void) { return video_w() - BAR_RIGHT - BAR_CLOCK_W; }
static int desk_x0(void) { return clock_x() - BAR_ITEM_GAP - DESK_N * DESK_CELL; }
static int load_x(void)  { return desk_x0() - BAR_ITEM_GAP - LOAD_W; }
static int show_x(void)  { return load_x() - BAR_ITEM_GAP - SHOW_W; }
/* The mark and the name at the left end: what opens the menu */
static int logo_w(void)
{
    return BAR_NAME_X + ui_font_width(UI_BOLD, BAR_PX, "ThreadOS") + 6;
}

/* --- the dock -------------------------------------------------------------
 *
 * One row of tiles on glass, in the middle of the bottom edge: first
 * the programs it starts, then - after a thin line - one tile for each
 * window on this desktop. It is exactly as wide as what is in it, and
 * when that would be wider than the screen the tiles get smaller
 * rather than the dock running off the edge, where they would be drawn
 * nowhere and clickable nowhere.
 *
 * Everything that draws the dock or looks for a tile under the pointer
 * asks dock_geom(), so a tile is clickable exactly where it is drawn.
 */
struct dock {
    int x, y, w, h;       /* the glass */
    int tile;             /* the side of one tile */
    int ty;               /* where the tiles' top edge is */
    int n_l, n_w;         /* programs, windows */
};

static int win_count(void);

/* The size the setting asks for */
static int dock_want(void)
{
    uint32_t s = config_get_num("dock", 1);
    return s == 0 ? DOCK_TILE_S : s == 2 ? DOCK_TILE_L : DOCK_TILE_M;
}

static void dock_geom(struct dock *d)
{
    int want = dock_want();
    d->n_l = icon_n;
    d->n_w = win_count();
    int n = d->n_l + d->n_w;
    int fixed = 2 * DOCK_PAD + (n > 0 ? (n - 1) * DOCK_GAP : 0) +
                (d->n_l && d->n_w ? DOCK_SEP - DOCK_GAP : 0);
    int room = video_w() - 2 * DOCK_MARGIN;
    int t = want;
    if (n && fixed + n * t > room)
        t = (room - fixed) / n;
    if (t < DOCK_TILE_MIN)
        t = DOCK_TILE_MIN;
    d->tile = t;
    d->w = fixed + n * t;
    d->h = want + 2 * DOCK_PAD;
    d->x = (video_w() - d->w) / 2;
    d->y = video_h() - DOCK_BOTTOM - d->h;
    d->ty = d->y + DOCK_PAD + (want - t) / 2;
}

/* The left edge of the k-th tile, counting the programs first */
static int dock_tile_x(const struct dock *d, int k)
{
    int x = d->x + DOCK_PAD + k * (d->tile + DOCK_GAP);
    if (d->n_l && k >= d->n_l)
        x += DOCK_SEP - DOCK_GAP;
    return x;
}

/* Which tile is under the point: 0.. for the programs, 100.. for the
   windows (counted among those on this desktop), -1 for none. A tile
   takes clicks over the dock's whole height and half the gap on either
   side - a tile that has to be hit to the pixel is a tile that gets
   missed. */
static int dock_hit(int mx, int my)
{
    struct dock d;
    dock_geom(&d);
    if (my < d.y || my >= d.y + d.h)
        return -1;
    for (int k = 0; k < d.n_l + d.n_w; k++) {
        int tx = dock_tile_x(&d, k);
        if (mx >= tx - DOCK_GAP / 2 && mx < tx + d.tile + DOCK_GAP / 2)
            return k < d.n_l ? k : 100 + (k - d.n_l);
    }
    return -1;
}

/* Where windows may be put: under the bar, above the dock. A
   maximised window fills exactly this, and so do the halves a window
   snaps to - the bar and the dock are how a person reaches everything
   else, so a window that covered them would be one there was no way
   out of. */
static int work_top(void) { return BAR_H; }
static int work_bottom(void)
{
    struct dock d;
    dock_geom(&d);
    return d.y - DOCK_BOTTOM;
}

static int menu_h(void) { return 2 * MENU_PAD + MENU_N * MENU_ROW; }

static int menu_item_y(int i)
{
    return menu_py + MENU_PAD + i * MENU_ROW;
}

/* Puts the menu somewhere it fits: never off the right-hand edge,
   never over the dock. */
static void menu_at(int x, int y)
{
    int hgt = menu_h();
    if (x + MENU_W > video_w())
        x = video_w() - MENU_W;
    if (x < 0)
        x = 0;
    if (y + hgt > work_bottom())
        y = work_bottom() - hgt;
    if (y < BAR_H)
        y = BAR_H;
    menu_px = x;
    menu_py = y;
    menu_open = 1;
}

/* How much of the machine is in use.
 *
 * A number would say "4128 KiB", which is a thing to read; a capsule
 * filled to a third is a thing to SEE. The exact figures - how much
 * memory, of how much, and how many tasks are behind it - are one
 * hover away, so the bar stays quiet until it is asked. */

/* "Show the desktop": everything out of the way, and then everything
   back. The second click is the whole of the idea - a button that only
   hides windows is a button that costs you your work arrangement, and
   nobody presses it twice. So which windows were hidden BY IT is
   remembered, and only those come back; a window minimised by hand
   stays minimised, because that was somebody's decision. */
static unsigned hidden_by_show;   /* a bit per window slot */

/* The reading itself, refreshed about once a second - memory does not
   change fast enough to be worth a fresh count sixty times a second,
   and every count walks the task list under a lock. */
static int load_percent;
static int load_used_mib, load_ram_mib, load_tasks;
static int load_wait;

static void load_refresh(void)
{
    if (--load_wait > 0)
        return;
    load_wait = FRAME_HZ;              /* once a second */

    uint32_t ram_kib = pmm_ram_kib();
    uint32_t used_kib = pmm_used_frames() * 4;
    if (used_kib > ram_kib)
        used_kib = ram_kib;
    load_percent = ram_kib ? (int)((used_kib * 100) / ram_kib) : 0;
    load_used_mib = (int)(used_kib / 1024);
    load_ram_mib = (int)(ram_kib / 1024);
    load_tasks = task_count();
}

/* Is this window on the desktop being shown? Everything that draws a
   window, counts one or looks for one under the cursor asks here -
   that one question is the whole of what a virtual desktop is. */
static int here(int i)
{
    return wins[i].used && wins[i].desk == cur_desk;
}

static int win_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_WIN; i++)
        if (here(i))
            n++;
    return n;
}

/* The dock's tile a window's program was started from, or -1 */
static int launch_icon_of(const struct window *w)
{
    return w->owner >= 0 ? icon_of_pid(w->owner) : -1;
}

/* The picture a window wears in the dock: its program's, the plain
   window for the kernel's own, and a letter for anything started some
   other way - from a shell, say */
static int launch_art_of(const struct window *w)
{
    if (w->owner < 0)
        return 0;
    int art = art_of_pid(w->owner);
    return art >= 0 ? art : ART_LETTER;
}

/* Is anything started from this tile still running - on any desktop?
   That is what the dot under it says. */
static int launcher_running(int k)
{
    for (int i = 0; i < MAX_WIN; i++)
        if (wins[i].used && launch_icon_of(&wins[i]) == k)
            return 1;
    return 0;
}

static int z_top;           /* the highest Z order handed out */

/* Brings a window to the top (the highest Z) */
static void raise_win(int i)
{
    wins[i].z = ++z_top;
}

/* Fills out[] with the slots of used windows in Z order (from the
   bottom); returns their count */
static int zorder(int *out)
{
    int n = 0;
    for (int i = 0; i < MAX_WIN; i++)
        if (here(i))
            out[n++] = i;
    for (int a = 1; a < n; a++) {     /* insertion sort by z */
        int key = out[a], b = a - 1;
        while (b >= 0 && wins[out[b]].z > wins[key].z) {
            out[b + 1] = out[b];
            b--;
        }
        out[b + 1] = key;
    }
    return n;
}

/* Puts an event into a window's queue (protected - called from the
   compositor, the keyboard IRQ and indirectly from a syscall) */
static void win_enqueue(struct window *w, int type, int x, int y, int val)
{
    if (!w->content)
        return;
    uint32_t flags = irq_save();
    int next = (w->ev_head + 1) % WEV_QLEN;
    if (next != w->ev_tail) {
        w->evq[w->ev_head] = (struct wevent){type, x, y, val};
        w->ev_head = next;
    }
    irq_restore(flags);
}

#define GRIP_SZ  10         /* the resize grip (bottom right corner) */
#define BTN_W 44
#define BTN_H 18
#define WIN_MIN_W 60
#define WIN_MIN_H 40

static int in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

/* The three lights on the left of the title bar: close, minimise,
   zoom - in that order, as everywhere they are lights. k = 0, 1, 2. */
static int light_x(const struct window *w, int k)
{
    return w->x + LIGHT_X + k * LIGHT_STEP;
}
static int light_y(const struct window *w)
{
    return w->y + (TITLE_H - LIGHT_D) / 2 + 1;
}
/* Is the point on light k? The square around it is a little larger
   than the light: eleven pixels is a small thing to aim at. */
static int on_light(const struct window *w, int k, int x, int y)
{
    int pad = (LIGHT_HIT - LIGHT_D) / 2;
    return in_rect(x, y, light_x(w, k) - pad, light_y(w) - pad,
                   LIGHT_HIT, LIGHT_HIT);
}
static int grip_x(const struct window *w)  { return w->x + w->w - GRIP_SZ; }
static int grip_y(const struct window *w)  { return w->y + w->h - GRIP_SZ; }
/* Every window may be resized. An application window costs more than a
   kernel one - its content buffer has to be made anew - so that is done
   once, when the drag ends, and not on every frame of it. */
static int resizable(const struct window *w) { (void)w; return 1; }
static int btn_x(const struct window *w)   { return w->x + 10; }
static int btn_y(const struct window *w)    { return w->y + TITLE_H + 12; }
/* An 8x12 cursor arrow: set bits = cursor pixels.
   The highest bit (0x80) is the leftmost column. */
static const uint8_t cursor_bmp[12] = {
    0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC,
    0xFE, 0xFF, 0xF8, 0xD8, 0x88, 0x18,
};

static void draw_cursor(int mx, int my)
{
    for (int row = 0; row < 12; row++) {
        for (int col = 0; col < 8; col++) {
            if (!(cursor_bmp[row] & (0x80 >> col)))
                continue;
            /* A black outline (neighbouring pixels) for visibility on any
               background, with a white middle on top */
            video_putpixel(mx + col - 1, my + row, COL_BLACK);
            video_putpixel(mx + col + 1, my + row, COL_BLACK);
            video_putpixel(mx + col, my + row - 1, COL_BLACK);
            video_putpixel(mx + col, my + row + 1, COL_BLACK);
        }
    }
    for (int row = 0; row < 12; row++)
        for (int col = 0; col < 8; col++)
            if (cursor_bmp[row] & (0x80 >> col))
                video_putpixel(mx + col, my + row, COL_WHITE);
}

/* --- Where a window's pixels live -----------------------------------
 *
 * Not on the kernel's heap. A window's content buffer is the largest
 * single thing this system allocates: at a laptop's own resolution a
 * maximised window in colours is eight megabytes, which is the whole
 * heap - and the heap is a first-fit list shared with every small
 * allocation the kernel makes. Putting the biggest and the smallest
 * objects in one list is how a list gets fragmented.
 *
 * So these come whole pages at a time, straight from the frame
 * allocator. That is also what makes the size a question of the panel
 * rather than of a number written here: a screen twice as large asks
 * for twice the frames and gets them, or does not and the window keeps
 * the size it had. */
static uint8_t *win_buf_alloc(uint32_t bytes)
{
    uint32_t addr = pmm_alloc_contiguous((bytes + 4095) / 4096);
    return addr ? (uint8_t *)(uintptr_t)addr : (uint8_t *)0;
}

static void win_buf_free(uint8_t *p, uint32_t bytes)
{
    if (!p)
        return;
    uint32_t addr = (uint32_t)(uintptr_t)p;
    for (uint32_t n = (bytes + 4095) / 4096; n; n--, addr += 4096)
        pmm_free_frame(addr);
}

/* --- Drawing into an application's content buffer -------------------
 *
 * The colour below is whatever the window's own pixels are: an index
 * for an ordinary window, 0x00RRGGBB for a true-colour one. Every
 * caller in this file passes an index, and the two entry points that
 * applications reach translate one into the other, so the shape of the
 * drawing code does not change with the format. */


static void buf_px(struct window *w, int x, int y, uint32_t c)
{
    if (x < 0 || x >= w->cw || y < 0 || y >= w->ch)
        return;
    if (w->truecolour)
        ((uint32_t *)w->content)[y * w->cw + x] = c;
    else
        w->content[y * w->cw + x] = (uint8_t)c;
}

/* An index means one thing to an ordinary window and another to a
   true-colour one, and every caller inside the compositor speaks
   indices - so this is where the two meet. */
static uint32_t buf_colour(const struct window *w, uint8_t idx)
{
    return w->truecolour ? video_palette_rgb(idx) : idx;
}

static void buf_rect(struct window *w, int x, int y, int rw, int rh,
                     uint32_t c)
{
    for (int j = 0; j < rh; j++)
        for (int i = 0; i < rw; i++)
            buf_px(w, x + i, y + j, c);
}

static void buf_char(struct window *w, int x, int y, char ch, uint32_t c)
{
    if (ch < 0x20 || ch > 0x7E)
        return;
    const uint8_t *g = font8x8[ch - 0x20];
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++)
            if (g[row] & (1 << col))
                buf_px(w, x + col, y + row, c);
}

static void buf_text(struct window *w, int x, int y, const char *s, uint32_t c)
{
    for (int i = 0; s[i]; i++)
        buf_char(w, x + i * 8, y, s[i], c);
}

/* --- The pictures ----------------------------------------------------------
 *
 * A word under a coloured square is a label, not an icon - it says
 * nothing until it is read. A picture is recognised before it is read,
 * which is the whole reason icons exist. Each of these is one idea on
 * a tile of its own colour: a window, a clock face, a folder, bars of
 * a chart, a prompt, a globe, a cogwheel, a page, a list, sliders.
 *
 * They are drawn from shapes rather than kept as pixels, so they come
 * out sharp at any size - the dock's, which is a setting, and the
 * menu's. Positions are given on a grid of 320 across the tile (tenths
 * of a thirty-second of it), which is what D() turns into the
 * sixteenths of a pixel kernel/gfx.c draws in.
 *
 * A drawn picture is kept (see art_get), because the shapes in one cost
 * a few thousand pixels of arithmetic and the dock is drawn thirty
 * times a second.
 */

/* The twelve hours and the eight points of the compass, as
   thousandths: x = sin, y = -cos, counted clockwise from the top */
static const int16_t sin12[12] = { 0, 500, 866, 1000, 866, 500,
                                   0, -500, -866, -1000, -866, -500 };
static const int16_t cos12[12] = { 1000, 866, 500, 0, -500, -866,
                                   -1000, -866, -500, 0, 500, 866 };
static const int16_t sin8[8] = { 0, 707, 1000, 707, 0, -707, -1000, -707 };
static const int16_t cos8[8] = { 1000, 707, 0, -707, -1000, -707, 0, 707 };

static void draw_art(int art, int x, int y, int s, char letter)
{
    int X = x * 16, Y = y * 16;
#define D(v) ((v) * s / 20)
#define PX(v) (X + D(v))
#define PY(v) (Y + D(v))
    static const uint32_t grad[ART_N][2] = {
        { 0x64D2FF, 0x0A84FF },   /* App */
        { 0x48484A, 0x1C1C1E },   /* Clock */
        { 0x8FD8FF, 0x2F8EF0 },   /* Files */
        { 0x3A3A3C, 0x111113 },   /* Monitor */
        { 0x3A3A3C, 0x141416 },   /* Term */
        { 0x64D2FF, 0x0A6CFF },   /* Web */
        { 0xAEAEB2, 0x636366 },   /* Settings */
        { 0xFFD60A, 0xFF9F0A },   /* Editor */
        { 0xFF6B8A, 0xFF2D55 },   /* Tasks */
        { 0xDA8FFF, 0x8944E0 },   /* Mixer */
        { 0xFFB340, 0xFF375F },   /* Appearance */
        { 0x8E8E93, 0x48484A },   /* anything else: a letter */
    };
    if (art < 0 || art >= ART_N)
        art = ART_LETTER;
    gfx_fill_grad(x, y, s, s, grad[art][0], grad[art][1], 255);
    /* a little light along the top edge, as on glass */
    gfx_fill(x, y, s, 1, 0xFFFFFF, 60);

    switch (art) {
    case 0:                                       /* a window */
        gfx_rrect16(PX(70), PY(85), D(180), D(150), D(25), 0xFFFFFF, 245);
        gfx_line16(PX(78), PY(128), PX(242), PY(128), D(8), 0xBCD3EE, 255);
        gfx_disc16(PX(97), PY(106), D(11), 0xFF5F57, 255);
        gfx_disc16(PX(126), PY(106), D(11), 0xFEBC2E, 255);
        gfx_disc16(PX(155), PY(106), D(11), 0x28C840, 255);
        break;
    case 1:                                       /* a clock face at ten past ten */
        gfx_disc16(PX(160), PY(160), D(118), 0xFFFFFF, 255);
        for (int k = 0; k < 12; k++)
            gfx_disc16(PX(160) + D(96 * sin12[k] / 1000),
                       PY(160) - D(96 * cos12[k] / 1000), D(7),
                       0x8E8E93, 255);
        gfx_line16(PX(160), PY(160), PX(117), PY(135), D(18), 0x1C1C1E, 255);
        gfx_line16(PX(160), PY(160), PX(225), PY(122), D(13), 0x1C1C1E, 255);
        gfx_line16(PX(160), PY(160), PX(160), PY(245), D(6), 0xFF3B30, 255);
        gfx_disc16(PX(160), PY(160), D(10), 0xFF3B30, 255);
        break;
    case 2:                                       /* a folder */
        gfx_rrect16(PX(55), PY(80), D(95), D(50), D(15), 0xCDE9FF, 255);
        gfx_rrect16(PX(55), PY(95), D(210), D(150), D(20), 0xCDE9FF, 255);
        gfx_rrect16(PX(55), PY(122), D(210), D(123), D(20), 0xFFFFFF, 255);
        break;
    case 3:                                       /* bars of a chart */
        for (int k = 0; k < 4; k++) {
            static const int hgt[4] = { 80, 140, 100, 170 };
            gfx_rrect16(PX(70 + 45 * k), PY(245 - hgt[k]), D(34), D(hgt[k]),
                        D(8), 0x30D158, 255);
        }
        break;
    case 4:                                       /* a prompt */
        gfx_line16(PX(88), PY(108), PX(136), PY(150), D(22), 0xFFFFFF, 255);
        gfx_line16(PX(136), PY(150), PX(88), PY(192), D(22), 0xFFFFFF, 255);
        gfx_line16(PX(152), PY(200), PX(228), PY(200), D(22), 0x30D158, 255);
        break;
    case 5:                                       /* a globe */
        gfx_circle16(PX(160), PY(160), D(105), D(15), 0xFFFFFF, 255);
        gfx_rring16(PX(115), PY(55), D(90), D(210), D(45), D(15),
                    0xFFFFFF, 255);
        gfx_line16(PX(58), PY(160), PX(262), PY(160), D(14), 0xFFFFFF, 255);
        gfx_line16(PX(76), PY(106), PX(244), PY(106), D(12), 0xFFFFFF, 220);
        gfx_line16(PX(76), PY(214), PX(244), PY(214), D(12), 0xFFFFFF, 220);
        break;
    case 6:                                       /* a cogwheel */
        for (int k = 0; k < 8; k++)
            gfx_line16(PX(160) + D(62 * sin8[k] / 1000),
                       PY(160) - D(62 * cos8[k] / 1000),
                       PX(160) + D(98 * sin8[k] / 1000),
                       PY(160) - D(98 * cos8[k] / 1000), D(42),
                       0xEDEDF0, 255);
        gfx_disc16(PX(160), PY(160), D(80), 0xEDEDF0, 255);
        gfx_disc16(PX(160), PY(160), D(32), 0x8A8A90, 255);
        break;
    case 7:                                       /* a page with writing */
        gfx_rrect16(PX(85), PY(55), D(150), D(210), D(18), 0xFFFFFF, 255);
        for (int k = 0; k < 4; k++)
            gfx_line16(PX(112), PY(105 + 35 * k),
                       PX(k == 3 ? 170 : 208), PY(105 + 35 * k), D(12),
                       0xC7C7CC, 255);
        break;
    case 8:                                       /* a list, one row done */
        for (int k = 0; k < 3; k++) {
            gfx_circle16(PX(95), PY(100 + 60 * k), D(22), D(14), 0xFFFFFF, 255);
            gfx_line16(PX(140), PY(100 + 60 * k), PX(240), PY(100 + 60 * k),
                       D(16), 0xFFFFFF, 255);
        }
        gfx_disc16(PX(95), PY(100), D(11), 0xFFFFFF, 255);
        break;
    case 9:                                       /* sliders */
        for (int k = 0; k < 3; k++) {
            static const int knob[3] = { 185, 110, 205 };
            gfx_line16(PX(100 + 60 * k), PY(70), PX(100 + 60 * k), PY(250),
                       D(12), 0xFFFFFF, 140);
            gfx_disc16(PX(100 + 60 * k), PY(knob[k]), D(24), 0xFFFFFF, 255);
        }
        break;
    case 10:                                      /* day and night */
        gfx_disc16(PX(160), PY(160), D(96), 0xFFFFFF, 255);
        gfx_disc16(PX(205), PY(125), D(76), 0xFF6A50, 255);
        gfx_circle16(PX(160), PY(160), D(96), D(14), 0xFFFFFF, 255);
        break;
    default: {                                    /* the first letter */
        char t[2] = { letter, 0 };
        int px = s * 11 / 20;
        int tw = ui_font_width(UI_BOLD, px, t);
        ui_font_text(UI_BOLD, px, x + (s - tw) / 2,
                     ui_font_baseline_in(px, y, s), t, 0xFFFFFF);
        break;
    }
    }
#undef D
#undef PX
#undef PY
}

/* The pictures drawn so far, at the sizes they were asked for. The
   room for them comes from the frame allocator the first time; when it
   is full, everything is forgotten and drawn again - simpler than
   choosing what to throw away, and with a dozen programs at two sizes
   it does not happen. */
#define ART_CACHE 32
#define ART_POOL_PAGES 64
static struct { int art, s; char letter; uint32_t *px; } art_cache[ART_CACHE];
static int art_cached;
static uint32_t *art_pool;
static uint32_t art_pool_used;           /* in pixels */

static const uint32_t *art_get(int art, int s, char letter)
{
    if (art != ART_LETTER)
        letter = 0;
    for (int i = 0; i < art_cached; i++)
        if (art_cache[i].art == art && art_cache[i].s == s &&
            art_cache[i].letter == letter)
            return art_cache[i].px;
    if (!art_pool) {
        uint32_t a = pmm_alloc_contiguous(ART_POOL_PAGES);
        if (!a)
            return (const uint32_t *)0;
        art_pool = (uint32_t *)(uintptr_t)a;
    }
    uint32_t need = (uint32_t)(s * s);
    if (art_cached == ART_CACHE ||
        art_pool_used + need > ART_POOL_PAGES * 1024u) {
        art_cached = 0;
        art_pool_used = 0;
        if (need > ART_POOL_PAGES * 1024u)
            return (const uint32_t *)0;
    }
    uint32_t *px = art_pool + art_pool_used;
    struct gfx_surface surf = { px, s, s, s };
    gfx_target(&surf);
    draw_art(art, 0, 0, s, letter);
    gfx_target((const struct gfx_surface *)0);
    art_cache[art_cached].art = art;
    art_cache[art_cached].s = s;
    art_cache[art_cached].letter = letter;
    art_cache[art_cached].px = px;
    art_cached++;
    art_pool_used += need;
    return px;
}

/* A tile: the picture with round corners, the way the dock and the
   menu show it. 'a' below 255 is a tile that is there and not there -
   a window that is minimised. */
static void draw_tile(int art, int x, int y, int s, char letter, int a)
{
    const uint32_t *px = art_get(art, s, letter);
    int r = s * 9 / 40;
    if (px) {
        gfx_blit_round(x, y, s, s, r, px, s, a);
    } else {                      /* no room to keep it: draw it here */
        gfx_rrect(x, y, s, s, r, 0x8E8E93, a);
    }
}


/* A text cut to a width, with an ellipsis where it was cut */
static const char *fit_text(const char *s, int font, int px, int maxw,
                            char *out, int cap)
{
    if (ui_font_width(font, px, s) <= maxw)
        return s;
    int n = 0;
    while (s[n] && n < cap - 4)
        n++;
    while (n > 0) {
        /* back to the start of a character, never into the middle */
        n--;
        while (n > 0 && ((uint8_t)s[n] & 0xC0) == 0x80)
            n--;
        memcpy(out, s, (uint32_t)n);
        out[n] = (char)0xE2;
        out[n + 1] = (char)0x80;
        out[n + 2] = (char)0xA6;
        out[n + 3] = 0;
        if (ui_font_width(font, px, out) <= maxw)
            return out;
    }
    return "";
}
/* What was under a window's four corners before it was drawn - so the
   corners can be made round after its content is in (see
   gfx_round_save) */
static uint32_t corner_keep[4 * WIN_R * WIN_R];

static void draw_lights(const struct window *win, int focused)
{
    static const uint32_t on[3] = { LIGHT_CLOSE, LIGHT_MIN, LIGHT_ZOOM };
    int ly = light_y(win);
    /* The signs on them only while the pointer is over them: three
       coloured dots are calm, three dots with symbols are a toolbar */
    int near = in_rect(mouse_x, mouse_y, light_x(win, 0) - 3, ly - 3,
                       2 * LIGHT_STEP + LIGHT_D + 6, LIGHT_D + 6);
    for (int k = 0; k < 3; k++) {
        int cx = light_x(win, k) * 16 + LIGHT_D * 8;
        int cy = ly * 16 + LIGHT_D * 8;
        gfx_disc16(cx, cy, LIGHT_D * 8, focused || near ? on[k] : L.light_off,
                   255);
        gfx_circle16(cx, cy, LIGHT_D * 8, 12, 0x000000, 40);
        if (!near)
            continue;
        int d = LIGHT_D * 4 - 10;                 /* the sign's half size */
        if (k == 0) {
            gfx_line16(cx - d, cy - d, cx + d, cy + d, 22, 0x4D0000, 210);
            gfx_line16(cx - d, cy + d, cx + d, cy - d, 22, 0x4D0000, 210);
        } else if (k == 1) {
            gfx_line16(cx - d - 4, cy, cx + d + 4, cy, 22, 0x5A3A00, 210);
        } else {
            gfx_line16(cx - d - 4, cy, cx + d + 4, cy, 22, 0x003A00, 210);
            gfx_line16(cx, cy - d - 4, cx, cy + d + 4, 22, 0x003A00, 210);
        }
    }
}

/* Draws a decimal number as text */
static void draw_int(int x, int baseline, int v, uint32_t rgb)
{
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    char s[12];
    int j = 0;
    while (n)
        s[j++] = tmp[--n];
    s[j] = '\0';
    ui_font_text(UI_SANS, UI_PX, x, baseline, s, rgb);
}

/* Everything a window is but its shadow and its round corners, at
   win->x, win->y. 'off' says it goes into a picture of its own rather
   than onto the screen - the content then goes through gfx too. */
static void draw_body(const struct window *win, int focused, int off)
{
    int x = win->x, y = win->y, w = win->w, h = win->h;

    /* The title bar and the body. The bar is a shade apart from the
       body when the window has the focus and almost the body's own
       colour when it has not - with the lights, that is the whole of
       how focus is shown. */
    gfx_fill(x, y, w, TITLE_H + 1, focused ? L.title_bg : L.title_off, 255);
    gfx_fill(x, y + TITLE_H, w, 1, L.win_line, 150);
    gfx_fill(x, y + TITLE_H + 1, w, h - TITLE_H - 1, L.win_bg, 255);

    /* The title in the middle of the bar, in bold - and kept clear of
       the lights, so a long one is cut short rather than run under
       them */
    int left = kiosk ? x + 8 : light_x(win, 2) + LIGHT_D + 8;
    int room = x + w - 8 - left;
    char cut[sizeof(win->title) + 4];
    const char *t = fit_text(win->title, UI_BOLD, TITLE_PX,
                             room > 0 ? room : 0, cut, sizeof(cut));
    int tw = ui_font_width(UI_BOLD, TITLE_PX, t);
    int tx = x + (w - tw) / 2;
    if (tx < left)
        tx = left;
    ui_font_text(UI_BOLD, TITLE_PX, tx,
                 ui_font_baseline_in(TITLE_PX, y, TITLE_H), t,
                 focused ? L.title_text : L.title_dim);

    /* Not in the kiosk. There is nothing to minimise the window to,
       nowhere for it to grow into and nothing behind it to close it
       back to - three buttons that would all be lies. */
    if (!kiosk)
        draw_lights(win, focused);

    /* The grip: two short strokes the way the corner is pulled. A hint,
       not a handle to be modelled - and an application's content covers
       it, as it always did. */
    if (!kiosk && resizable(win)) {
        int gx = (x + w - 3) * 16, gy = (y + h - 3) * 16;
        for (int d = 3; d <= 7; d += 4)
            gfx_line16(gx, gy - d * 16, gx - d * 16, gy, 14, L.win_line, 255);
    }

    /* An optional action button plus a click counter */
    if (win->has_button) {
        int bx = btn_x(win), by = btn_y(win);
        gfx_rrect(bx, by, BTN_W, BTN_H, 6, L.accent, 255);
        int lw = ui_font_width(UI_SANS, UI_PX, "Click");
        ui_font_text(UI_SANS, UI_PX, bx + (BTN_W - lw) / 2,
                     ui_baseline_in(by, BTN_H), "Click", L.accent_text);
        draw_int(bx + BTN_W + 8, ui_baseline_in(by, BTN_H), win->counter,
                 L.title_text);
    }

    /* An application window: compose its content buffer into the work
       area. A true-colour window goes a row at a time - its pixels are
       already what the framebuffer wants - and that is the faster of
       the two, not the slower. */
    if (win->content) {
        int bx = x + 1;
        int by = y + TITLE_H + 1;
        if (off) {
            const uint32_t *rgb = (const uint32_t *)win->content;
            for (int j = 0; j < win->ch; j++)
                for (int i = 0; i < win->cw; i++)
                    gfx_put(bx + i, by + j, win->truecolour
                            ? rgb[j * win->cw + i]
                            : video_palette_rgb(win->content[j * win->cw + i]));
        } else if (win->truecolour) {
            video_blit_rgb(bx, by, win->cw, win->ch,
                           (const uint32_t *)win->content, win->cw);
        } else {
            for (int j = 0; j < win->ch; j++)
                for (int i = 0; i < win->cw; i++)
                    video_putpixel(bx + i, by + j,
                                   win->content[j * win->cw + i]);
        }
    }
}

/* --- Windows on their way ---------------------------------------------------
 *
 * A window that appears out of nowhere, or vanishes into the dock
 * without a trace, makes the eye search for where it went. So for a
 * few frames it is shown on its way: opening, it grows the last tenth
 * of its size as it comes up out of nothing; minimised, it shrinks into
 * its tile in the dock and fades; brought back, it comes out of the
 * tile the same way.
 *
 * The window is drawn once into a picture of its own and the picture
 * drawn at the size of the moment - which costs a window's worth of
 * memory, so a window too large for the picture just appears, as all
 * of them used to.
 */
#define ANIM_NONE    0
#define ANIM_OPEN    1
#define ANIM_MIN     2
#define ANIM_RESTORE 3
#define ANIM_FRAMES  10             /* a fifth of a second at 50 frames */
#define ANIM_PX      (640 * 480)    /* the largest window that is animated */

static uint32_t *anim_px;
static int anim_tried;

/* How long a way takes: the 'motion' setting - 0 is none at all (for
   whoever would rather the screen stayed still), 1 the usual, and more
   is that many times slower, slow enough to watch every frame */
static uint32_t anim_frames = ANIM_FRAMES;

static void anim_start(int i, int kind)
{
    if (kiosk)                     /* one window, which goes nowhere */
        return;
    uint32_t m = config_get_num("motion", 1);
    if (m == 0)
        return;
    anim_frames = ANIM_FRAMES * (m > 10 ? 10 : m);
    wins[i].anim = kind;
    wins[i].anim_at = frames;
}

/* How far along, 0..256 - easing out: quick at first, settling at the
   end, the way a thing that is thrown and caught moves */
static int anim_progress(const struct window *win)
{
    uint32_t age = frames - win->anim_at;
    if (age >= anim_frames)
        return 256;
    int u = 256 - (int)(age * 256 / anim_frames);
    return 256 - u * u / 256 * u / 256;
}

static int lerp(int a, int b, int p)
{
    return a + (b - a) * p / 256;
}

/* Where the window's tile in the dock is: (x, y) and its side. 0 when
   the window has none - it is on another desktop. */
static int win_tile(const struct window *win, int *x, int *y, int *t)
{
    struct dock d;
    dock_geom(&d);
    int j = 0;
    for (int i = 0; i < MAX_WIN; i++) {
        if (!here(i))
            continue;
        if (&wins[i] == win) {
            *x = dock_tile_x(&d, d.n_l + j);
            *y = d.ty;
            *t = d.tile;
            return 1;
        }
        j++;
    }
    return 0;
}

/* Draws the window as it is on its way; 0 when it cannot be (and then
   it is drawn, or not, as if it were not moving) */
static int draw_animated(const struct window *win, int focused)
{
    if (!anim_px && !anim_tried) {
        anim_tried = 1;
        uint32_t a = pmm_alloc_contiguous((ANIM_PX * 4 + 4095) / 4096);
        anim_px = (uint32_t *)(uintptr_t)a;
    }
    if (!anim_px || win->w * win->h > ANIM_PX)
        return 0;

    int p = anim_progress(win);
    int x = win->x, y = win->y, w = win->w, h = win->h;
    /* from (x0, y0, w0, h0, a0) to (x1, ...) */
    int x0 = x, y0 = y, w0 = w, h0 = h, a0 = 255;
    int x1 = x, y1 = y, w1 = w, h1 = h, a1 = 255;
    int tx, ty, t;
    if (win->anim == ANIM_OPEN) {
        w0 = w * 9 / 10;
        h0 = h * 9 / 10;
        x0 = x + (w - w0) / 2;
        y0 = y + (h - h0) / 2;
        a0 = 0;
    } else if (win_tile(win, &tx, &ty, &t)) {
        /* the tile keeps the window's proportions, its middle on the
           tile's middle */
        int sw = w > h ? t : t * w / h, sh = w > h ? t * h / w : t;
        int sx = tx + (t - sw) / 2, sy = ty + (t - sh) / 2;
        if (win->anim == ANIM_MIN) {
            x1 = sx; y1 = sy; w1 = sw; h1 = sh; a1 = 0;
        } else {
            x0 = sx; y0 = sy; w0 = sw; h0 = sh; a0 = 0;
        }
    } else {
        return 0;
    }
    int cx = lerp(x0, x1, p), cy = lerp(y0, y1, p);
    int cw = lerp(w0, w1, p), ch = lerp(h0, h1, p), a = lerp(a0, a1, p);
    if (cw < 2 || ch < 2 || a <= 0)
        return 1;

    /* The window at its own size, into the picture */
    struct window at0 = *win;
    at0.x = 0;
    at0.y = 0;
    struct gfx_surface s = { anim_px, w, h, w };
    gfx_target(&s);
    draw_body(&at0, focused, 1);
    gfx_target((const struct gfx_surface *)0);

    int r = WIN_R * cw / w;
    gfx_shadow(cx, cy, cw, ch, r, SHADOW_S,
               (focused ? L.shadow_a : L.shadow_a * 3 / 5) * a / 255,
               SHADOW_DY);
    gfx_blit_scaled(cx, cy, cw, ch, r, anim_px, w, h, w, a);
    return 1;
}

/* The windows whose way is over stop being on it */
static void anim_settle(void)
{
    for (int i = 0; i < MAX_WIN; i++)
        if (wins[i].anim && frames - wins[i].anim_at >= anim_frames)
            wins[i].anim = ANIM_NONE;
}

static void draw_window(const struct window *win)
{
    int focused = focus_idx >= 0 && win == &wins[focus_idx];
    if (win->anim && draw_animated(win, focused))
        return;
    if (win->minimized)            /* minimised - visible only in the dock */
        return;

    int x = win->x, y = win->y, w = win->w, h = win->h;

    /* The shadow first, under everything: deeper under the window that
       is listening, which is how the eye finds it without reading */
    gfx_shadow(x, y, w, h, WIN_R, SHADOW_S,
               focused ? L.shadow_a : L.shadow_a * 3 / 5, SHADOW_DY);
    gfx_round_save(x, y, w, h, WIN_R, corner_keep);

    draw_body(win, focused, 0);

    /* Round corners - the content's too - and one soft line round the
       whole, which is what says where a light window ends on a light
       background */
    gfx_round_apply(x, y, w, h, WIN_R, corner_keep);
    gfx_rring16(x * 16, y * 16, w * 16, h * 16, WIN_R * 16, 16, L.win_line,
                focused ? 220 : 170);
}

/* Is the point on a window's title bar (the drag handle) */
static int on_titlebar(const struct window *win, int x, int y)
{
    return x >= win->x && x < win->x + win->w &&
           y >= win->y && y < win->y + TITLE_H;
}

/* Goes to another desktop. Nothing moves and nothing is redrawn by
   hand: the next frame simply asks here(i) about other windows.
   Whatever the hand was in the middle of ends - a drag that carried
   on across a switch would drop a window on a desktop nobody was
   looking at. */
static void desk_switch(int d)
{
    if (d < 0 || d >= DESK_N || d == cur_desk)
        return;
    cur_desk = d;
    drag_idx = -1;
    resize_idx = -1;
    content_drag = -1;
    menu_open = 0;

    /* The focus goes to the topmost window that is there, so that
       typing after a switch lands somewhere a person can see. */
    focus_idx = -1;
    int ord[MAX_WIN];
    int n = zorder(ord);
    for (int k = n - 1; k >= 0; k--)
        if (!wins[ord[k]].minimized && wins[ord[k]].content) {
            focus_idx = ord[k];
            break;
        }
}

/* Takes the window that has the focus along. Moving it out of sight
   and staying behind would be a way of losing a window, so we follow
   it - which is also what the hand expects, having pushed it. */
static void desk_take(int d)
{
    if (d < 0 || d >= DESK_N || d == cur_desk)
        return;
    if (focus_idx >= 0 && wins[focus_idx].used)
        wins[focus_idx].desk = d;
    desk_switch(d);
}

static void toggle_show_desktop(void)
{
    if (hidden_by_show) {                 /* bring back what it hid */
        for (int i = 0; i < MAX_WIN; i++)
            if (hidden_by_show & (1u << i)) {
                if (wins[i].used) {
                    wins[i].minimized = 0;
                    anim_start(i, ANIM_RESTORE);
                }
            }
        hidden_by_show = 0;
        return;
    }
    for (int i = 0; i < MAX_WIN; i++) {
        if (!here(i) || wins[i].minimized)
            continue;
        wins[i].minimized = 1;
        anim_start(i, ANIM_MIN);
        hidden_by_show |= 1u << i;
        if (focus_idx == i)
            focus_idx = -1;
    }
}


/* Which window is under that point, or -1. Asked from the top of the
   pile down, because that is the one a person sees there. The top bar
   and the desktop below it are DRAWN under the windows, so what is
   drawn under them must not take their clicks either. */
static int window_at(int x, int y)
{
    int ord[MAX_WIN];
    int n = zorder(ord);
    for (int k = n - 1; k >= 0; k--) {
        struct window *w = &wins[ord[k]];
        if (!w->minimized && in_rect(x, y, w->x, w->y, w->w, w->h))
            return ord[k];
    }
    return -1;
}

/* Snapping a window to an edge of the screen.
 *
 * A window pulled against the left edge takes the left half, against
 * the right edge the right half, against the top the whole screen.
 * It is the fastest way there is to put two things side by side, and
 * it costs nothing to learn: the window goes where it was pushed.
 *
 * While the button is still down the place the window WOULD take is
 * drawn as an outline. A window that jumps somewhere on release,
 * unannounced, is a window that got away - the outline is the
 * question, and letting go is the answer.
 *
 * The decision is made from the WINDOW's position, not the pointer's:
 * the pointer can be at the very edge while the window is still short
 * of it (a drag is clamped so a window never leaves the screen), and
 * what a person is aiming with is the window. The pointer only says
 * which edge was meant, so that a window wide enough to touch both
 * sides at once does not snap wherever it happens to be let go. */
#define SNAP_NONE  0
#define SNAP_LEFT  1
#define SNAP_RIGHT 2
#define SNAP_FULL  3
#define SNAP_EDGE  8              /* how near the edge counts as at it */
static int snap_hint;             /* where the dragged window would land */

/* What the window would become. The halves fill the space between the
   bar and the dock, as a maximised window does. */
static void snap_geom(int where, int *x, int *y, int *w, int *h)
{
    *x = 0;
    *y = work_top();
    *w = video_w();
    *h = work_bottom() - work_top();
    if (where == SNAP_LEFT)
        *w = video_w() / 2;
    else if (where == SNAP_RIGHT) {
        *x = video_w() / 2;
        *w = video_w() - *x;
    }
}

static int snap_where(int i, int mx, int my)
{
    const struct window *win = &wins[i];
    int sw = video_w(), sh = video_h();

    if (mx < sw / 4 && win->x <= SNAP_EDGE)
        return SNAP_LEFT;
    if (mx > sw - sw / 4 && win->x + win->w >= sw - SNAP_EDGE)
        return SNAP_RIGHT;
    /* The top comes last, so that a corner gives a half - two windows
       side by side is what somebody dragging into a corner wants, and
       the whole screen is one button away anyway. */
    if (my < sh / 4 && win->y <= BAR_H + SNAP_EDGE)
        return SNAP_FULL;
    return SNAP_NONE;
}

/* Mouse handling: clicks (close, buttons) and window dragging.
   Clicks take precedence over dragging; both are counted on the
   left-button press edge, so that one click = one action. */
static int prev_right;      /* the previous right-button state */


/* Two clicks on the same title bar, close together in time, are one
   gesture: fill the screen, or give the window its size back. Every
   system does this, and a person who has used one tries it here within
   the first minute.
 *
 * "Close together" is six tenths of a second - by the clock, not in
 * frames, since a busy machine draws fewer of them and the hand is no
 * slower for it. That is the value every system settles near: shorter
 * and a hand misses it on a machine that is busy, longer and two
 * separate clicks (which is how a window is raised, then raised again)
 * start counting as one gesture. */
#define DOUBLE_TICKS 60
static uint32_t last_title_click;
static int last_title_win = -1;

/* The search and the notifications (below): what the input asks of them */
static volatile int search_open;
static void search_click(int mx, int my);
static void search_close(void);
static int search_key(char c);
static int note_at(int x, int y);
static void note_drop(int i);

static void handle_input(int mx, int my, int buttons)
{
    int pressed = buttons & 0x01;
    int right = buttons & 0x02;

    mouse_x = mx;
    mouse_y = my;

    /* In the kiosk there is nothing to click but the one window and
       the one button the environment itself offers. Everything the
       dock, the menu, the bar and the desktop switch would have
       done is simply not reachable - which is the whole point of the
       mode. */
    if (kiosk) {
        int notch = mouse_take_scroll();
        int wi = window_at(mx, my);
        if (notch && wi >= 0 && wins[wi].content) {
            struct window *w = &wins[wi];
            int cx = mx - (w->x + 1), cy = my - (w->y + TITLE_H + 1);
            if (cx >= 0 && cy >= 0 && cx < w->cw && cy < w->ch)
                win_enqueue(w, WEV_SCROLL, cx, cy, notch);
        }
        if (pressed && !prev_pressed) {
            if (in_rect(mx, my, kiosk_btn_x(), kiosk_btn_y(),
                        KIOSK_BTN_W, KIOSK_BTN_H)) {
                printk("Restarting...\n");
                if (!acpi_reboot())
                    outb(0x64, 0xFE);
            } else if (wi >= 0 && wins[wi].content) {
                struct window *w = &wins[wi];
                int cx = mx - (w->x + 1), cy = my - (w->y + TITLE_H + 1);
                if (cx >= 0 && cy >= 0 && cx < w->cw && cy < w->ch)
                    win_enqueue(w, WEV_CLICK, cx, cy, 1);
            }
        }
        prev_pressed = pressed;
        prev_right = right;
        return;
    }

    /* The wheel goes to the window under the POINTER, not to the one
       with the focus. That is how every other machine does it and it
       is the right way round: a person looking at a list scrolls it by
       pointing at it, without first making it the window that hears
       the keyboard. */
    int notches = mouse_take_scroll();
    if (notches) {
        int wi = window_at(mx, my);
        if (wi >= 0 && wins[wi].content) {
            struct window *w = &wins[wi];
            int cx = mx - (w->x + 1);
            int cy = my - (w->y + TITLE_H + 1);
            if (cx >= 0 && cy >= 0 && cx < w->cw && cy < w->ch)
                win_enqueue(w, WEV_SCROLL, cx, cy, notches);
        }
    }

    /* The right button on the desktop opens the same menu where the
       pointer is. On a window it does nothing yet - a window's own
       menu is the window's business, and none of them has one. */
    if (right && !prev_right) {
        if (window_at(mx, my) < 0 && my >= BAR_H && my < work_bottom() &&
            dock_hit(mx, my) < 0)
            menu_at(mx, my);
        else
            menu_open = 0;
    }
    prev_right = right;

    /* The search and the notifications are above everything else, and
       take a click before anything under them can */
    if (pressed && !prev_pressed && search_open) {
        search_click(mx, my);
        prev_pressed = pressed;
        return;
    }
    if (pressed && !prev_pressed && note_at(mx, my) >= 0) {
        note_drop(note_at(mx, my));
        prev_pressed = pressed;
        return;
    }

    if (pressed && !prev_pressed) {     /* a press edge = a click */
        /* The menu bar is above everything, the way it is drawn. Its
           left end opens the menu; on the right are the desktop switch,
           and "show the desktop". */
        if (my < BAR_H) {
            if (mx < logo_w()) {
                if (menu_open && menu_py == MENU_Y)
                    menu_open = 0;
                else
                    menu_at(6, MENU_Y);
            } else if (in_rect(mx, my, show_x(), 0, SHOW_W, BAR_H)) {
                menu_open = 0;
                toggle_show_desktop();
            } else if (mx >= desk_x0() && mx < desk_x0() + DESK_N * DESK_CELL) {
                menu_open = 0;
                desk_switch((mx - desk_x0()) / DESK_CELL);
            } else {
                menu_open = 0;
            }
            prev_pressed = pressed;
            return;
        }
        if (menu_open) {
            for (int i = 0; i < MENU_N; i++) {
                if (in_rect(mx, my, menu_px, menu_item_y(i), MENU_W,
                            MENU_ROW)) {
                    /* Started from the menu, which names programs -
                       so its face is looked up by name */
                    int from = icon_of_app(menu_items[i]);
                    remember_launch(exec_spawn(menu_items[i]), from,
                                    from >= 0 ? icons[from].art
                                              : art_of_app(menu_items[i]));
                    menu_open = 0;
                    prev_pressed = pressed;
                    return;
                }
            }
            menu_open = 0;                /* a click outside the menu - close it */
            if (in_rect(mx, my, menu_px, menu_py, MENU_W, menu_h())) {
                prev_pressed = pressed;   /* on its edge: nothing else */
                return;
            }
        }
        /* The dock, which is above the windows: a program's tile starts
           it, a window's tile brings that window up - from minimised,
           if it was - and gives it the keyboard. The same function the
           drawing asks, so a tile is clickable exactly where it is. */
        int tile = dock_hit(mx, my);
        if (tile >= 0 && tile < 100) {
            remember_launch(exec_spawn(icons[tile].app), tile, icons[tile].art);
            hop_at[tile] = frames;
            hop_set[tile] = 1;
            prev_pressed = pressed;
            return;
        }
        if (tile >= 100) {
            int j = 0;
            for (int i = 0; i < MAX_WIN; i++) {
                if (!here(i))
                    continue;
                if (j++ != tile - 100)
                    continue;
                if (wins[i].minimized)   /* restore it if minimised */
                    anim_start(i, ANIM_RESTORE);
                wins[i].minimized = 0;
                focus_idx = i;
                raise_win(i);
                break;
            }
            prev_pressed = pressed;
            return;
        }
        {
            struct dock d;
            dock_geom(&d);
            if (in_rect(mx, my, d.x, d.y, d.w, d.h)) {
                prev_pressed = pressed;   /* the glass between tiles */
                return;
            }
        }

        int hit = 0;
        int ord[MAX_WIN];
        int n = zorder(ord);
        for (int k = n - 1; k >= 0; k--) {  /* from the topmost window downwards */
            int i = ord[k];
            struct window *w = &wins[i];
            if (w->minimized)               /* minimised - out of reach */
                continue;
            /* The lights - close, minimise, zoom. Not in the kiosk,
               where they are not drawn.
             *
             * Closing: a kernel window owns nothing, so it simply goes.
             * An application window belongs to a running process:
             * dropping it from under that process would leave it
             * drawing into a window that no longer exists. So we ASK -
             * and only insist when the asking goes unanswered. */
            if (!kiosk && on_light(w, 0, mx, my)) {
                close_window(i);
                hit = 1;
                break;
            }
            if (!kiosk && on_light(w, 1, mx, my)) {
                w->minimized = 1;
                anim_start(i, ANIM_MIN);
                if (focus_idx == i)
                    focus_idx = -1;
                hit = 1;
                break;
            }
            /* zoom - and the same light puts it back, so one click
               undoes the other */
            if (!kiosk && on_light(w, 2, mx, my)) {
                toggle_max(i);
                hit = 1;
                break;
            }
            /* the resize grip (bottom right corner). Pulling a
               maximised window's corner means it is no longer
               maximised - what it is now is whatever it is pulled to,
               and the size it came from is no longer worth going back
               to. */
            if (resizable(w) &&
                in_rect(mx, my, grip_x(w), grip_y(w), GRIP_SZ, GRIP_SZ)) {
                w->maxed = 0;
                resize_idx = i;
                focus_idx = i;
                raise_win(i);
                hit = 1;
                break;
            }
            /* an action button inside the window */
            if (w->has_button &&
                in_rect(mx, my, btn_x(w), btn_y(w), BTN_W, BTN_H)) {
                w->counter++;
                focus_idx = i;
                raise_win(i);
                hit = 1;
                break;
            }
            /* the title bar -> start dragging (and focus).
               Dragging a maximised window puts it back to its own size
               first - as everywhere else - and it comes back under the
               cursor at the same place along its bar, so it does not
               jump out from under the hand. */
            if (on_titlebar(w, mx, my)) {
                if (last_title_win == i &&
                    pit_ticks() - last_title_click <= DOUBLE_TICKS) {
                    last_title_win = -1;      /* one gesture, once */
                    toggle_max(i);
                    hit = 1;
                    break;
                }
                last_title_win = i;
                last_title_click = pit_ticks();
                unmax_pending = w->maxed;
                unmax_mx = mx;
                unmax_my = my;
                drag_idx = i;
                drag_dx = mx - w->x;
                drag_dy = my - w->y;
                focus_idx = i;
                raise_win(i);
                hit = 1;
                break;
            }
            /* a click into an application window's body -> focus plus an event
                   to the process */
            if (in_rect(mx, my, w->x, w->y, w->w, w->h)) {
                focus_idx = i;
                raise_win(i);
                hit = 1;
                int cx = mx - (w->x + 1);
                int cy = my - (w->y + TITLE_H + 1);
                if (cx >= 0 && cx < w->cw && cy >= 0 && cy < w->ch) {
                    win_enqueue(w, WEV_CLICK, cx, cy, buttons & 0x07);
                    /* And the beginning of a possible drag. A program
                       that does not care about dragging sees only the
                       click it already knew about. */
                    win_enqueue(w, WEV_PRESS, cx, cy, buttons & 0x07);
                    content_drag = i;
                }
                break;            /* swallow it - do not fall through to lower ones */
            }
        }
        if (!hit)
            focus_idx = -1;            /* a click on the desktop: nobody listens */
    }

    if (!pressed) {
        /* Let go against an edge: the window takes that edge's place.
           This has to happen before drag_idx is forgotten - it is the
           whole of what the drag was for. */
        if (drag_idx >= 0 && snap_hint != SNAP_NONE)
            snap_window(drag_idx, snap_hint);
        snap_hint = SNAP_NONE;
        drag_idx = -1;
        unmax_pending = 0;
        /* The end of a drag inside a window: the application is told
           where the pointer was let go, and that is the whole of what
           the compositor has to say about it. */
        if (content_drag >= 0) {
            struct window *w = &wins[content_drag];
            if (w->used && w->content) {
                int cx = mx - (w->x + 1);
                int cy = my - (w->y + TITLE_H + 1);
                if (cx < 0) cx = 0;
                if (cy < 0) cy = 0;
                if (cx >= w->cw) cx = w->cw - 1;
                if (cy >= w->ch) cy = w->ch - 1;
                win_enqueue(w, WEV_RELEASE, cx, cy, 0);
            }
            content_drag = -1;
        }
        if (resize_idx >= 0) {
            /* The drag is over - only now does the application window
               get a new content buffer. Doing it on every frame of the
               drag would mean allocating and freeing dozens of times
               for one pull of the mouse. */
            resize_content(&wins[resize_idx]);
            resize_idx = -1;
        }
    }

    /* Dragging inside a window: the pointer's place, once per frame.
       Clamped to the content, so an application dragging past the edge
       gets its own edge rather than somebody else's coordinates. */
    if (pressed && content_drag >= 0) {
        struct window *w = &wins[content_drag];
        if (!w->used || !w->content) {
            content_drag = -1;
        } else {
            int cx = mx - (w->x + 1);
            int cy = my - (w->y + TITLE_H + 1);
            if (cx < 0) cx = 0;
            if (cy < 0) cy = 0;
            if (cx >= w->cw) cx = w->cw - 1;
            if (cy >= w->ch) cy = w->ch - 1;
            win_enqueue(w, WEV_DRAG, cx, cy, 0);
        }
    }

    /* The pointer moved with the button down on a maximised window's
       bar: now it is a drag. The window takes its own size back, under
       the pointer at the same place along its bar, so it does not jump
       out from under the hand. */
    if (pressed && drag_idx >= 0 && unmax_pending) {
        int dx = mx - unmax_mx, dy = my - unmax_my;
        if (dx * dx + dy * dy >= 16) {
            struct window *w = &wins[drag_idx];
            int along = w->w > 0 ? (unmax_mx - w->x) * w->rw / w->w : 0;
            unmax_pending = 0;
            toggle_max(drag_idx);
            w->x = mx - along;
            w->y = my - 3;
            if (w->x < 0)
                w->x = 0;
            if (w->x + w->w > video_w())
                w->x = video_w() - w->w;
            if (w->y < BAR_H)
                w->y = BAR_H;
            drag_dx = mx - w->x;
            drag_dy = my - w->y;
        }
    }

    if (pressed && drag_idx >= 0 && !unmax_pending) {  /* in progress: the window follows the cursor */
        int nx = mx - drag_dx;
        int ny = my - drag_dy;
        if (nx < 0) nx = 0;
        if (ny < BAR_H) ny = BAR_H;            /* never over the bar */
        if (nx > video_w() - wins[drag_idx].w) nx = video_w() - wins[drag_idx].w;
        if (ny > video_h() - wins[drag_idx].h) ny = video_h() - wins[drag_idx].h;
        wins[drag_idx].x = nx;
        wins[drag_idx].y = ny;
        snap_hint = snap_where(drag_idx, mx, my);
    }

    if (pressed && resize_idx >= 0) {   /* in progress: the bottom right corner follows the cursor */
        struct window *w = &wins[resize_idx];
        int nw = mx - w->x + 1;
        int nh = my - w->y + 1;
        if (nw < WIN_MIN_W) nw = WIN_MIN_W;
        if (nh < WIN_MIN_H) nh = WIN_MIN_H;
        if (w->x + nw > video_w()) nw = video_w() - w->x;
        if (w->y + nh > video_h()) nh = video_h() - w->y;
        w->w = nw;
        w->h = nh;
    }

    prev_pressed = pressed;
}

/* Gives an application window a content buffer matching its new size
   and tells the application about it.
 *
 * The old content is not carried over. A program lays its window out
 * itself and is about to be told the new size, so it will draw the
 * whole thing again in a moment - copying the old picture would only
 * mean showing a stretched one until it does.
 *
 * When there is no memory for the new buffer, the window keeps the size
 * it had. Better a window that will not grow than one that has nothing
 * to draw into. */
/* Gives the window's content buffer the size the window now has.
   Returns -1 when there is no memory for the new buffer - and then the
   window keeps the size it had, because a window bigger than its buffer
   would be drawn from memory belonging to something else. */
static int resize_content(struct window *w)
{
    if (!w->content || w->owner < 0)
        return 0;                      /* a kernel window has no buffer */

    int cw = w->w - 2;
    int ch = w->h - TITLE_H - 2;
    if (cw < 8) cw = 8;
    if (ch < 8) ch = 8;
    if (cw == w->cw && ch == w->ch)
        return 0;                      /* the drag changed nothing */

    uint32_t bytes = (uint32_t)(cw * ch) * (w->truecolour ? 4u : 1u);
    uint8_t *buf = win_buf_alloc(bytes);
    if (!buf) {
        w->w = w->cw + 2;              /* back to the size that has a buffer */
        w->h = w->ch + TITLE_H + 2;
        return -1;
    }
    win_buf_free(w->content, w->content_bytes);
    w->content = buf;
    w->content_bytes = bytes;
    w->cw = cw;
    w->ch = ch;
    buf_rect(w, 0, 0, cw, ch, buf_colour(w, COL_WIN));
    win_enqueue(w, WEV_SIZE, cw, ch, 0);
    return 0;
}

/* Maximises the window, or puts it back where it came from.
 *
 * Maximised means: the whole width of the screen, from under the menu
 * bar down to the dock - over neither. They are how a person reaches
 * everything else, so a window that covered them would be a window
 * there was no way out of.
 *
 * An application's window is a buffer as well as a rectangle, so the
 * buffer has to grow too and the application has to be told. If there
 * is no memory for a buffer that size, nothing happens at all: the
 * window keeps its size and its old buffer, and stays where it was.
 * Half a maximise would be worse than none. */
static void toggle_max(int i)
{
    struct window *w = &wins[i];

    if (!w->maxed) {
        int ox = w->x, oy = w->y, ow = w->w, oh = w->h;
        w->x = 0;
        w->y = work_top();
        w->w = video_w();
        w->h = work_bottom() - work_top();
        if (resize_content(w) < 0) {   /* no memory for that buffer */
            w->x = ox;
            w->y = oy;
            w->w = ow;
            w->h = oh;
            return;
        }
        w->rx = ox;
        w->ry = oy;
        w->rw = ow;
        w->rh = oh;
        w->maxed = 1;
    } else {
        w->x = w->rx;
        w->y = w->ry;
        w->w = w->rw;
        w->h = w->rh;
        w->maxed = 0;
        resize_content(w);             /* shrinking always has the memory */
    }

    w->minimized = 0;
    focus_idx = i;
    raise_win(i);
}

/* Puts the window where the edge it was dragged to says. The size it
   had before is kept the way maximising keeps it, so the [] button
   gives the window its own size back - a window held at half the
   screen has no more lost its size than a maximised one has. */
static void snap_window(int i, int where)
{
    struct window *w = &wins[i];
    int nx, ny, nw, nh;

    snap_geom(where, &nx, &ny, &nw, &nh);
    if (w->x == nx && w->y == ny && w->w == nw && w->h == nh)
        return;

    int ox = w->x, oy = w->y, ow = w->w, oh = w->h;
    w->x = nx;
    w->y = ny;
    w->w = nw;
    w->h = nh;
    if (resize_content(w) < 0) {       /* no memory for that buffer */
        w->x = ox;
        w->y = oy;
        w->w = ow;
        w->h = oh;
        return;
    }
    if (!w->maxed) {                   /* remember where it came from */
        w->rx = ox;
        w->ry = oy;
        w->rw = ow;
        w->rh = oh;
    }
    w->maxed = 1;
    w->minimized = 0;
    focus_idx = i;
    raise_win(i);
}

/* The wallpaper: a small picture stretched over the screen.
 *
 * A screen-sized image is three hundred kilobytes and the kernel's
 * heap is one megabyte, so the picture kept here is small - 160 by 120
 * at most - and every one of its pixels covers a block of the screen.
 * It looks like what it is: a picture, softly. That is the honest
 * trade at this size of machine, and it is the whole of the format:
 *
 *   "PWAL", width and height (two bytes each), then 192 colours of
 *   three bytes (the palette entries from WALL_COL0 up), then one byte
 *   per pixel - an index into those colours.
 *
 * The desktop's own colours live below WALL_COL0 and are not touched,
 * so a wallpaper can never repaint a window.
 *
 * And a second shape of the same file, for a screen that holds
 * colours: "TWL2", width and height, then three bytes a pixel and no
 * palette at all. A hundred and ninety two colours is plenty for rings
 * and a weave, and visibly not plenty for a slope - the bands are
 * there to be counted. The old shape is still read, because a file
 * written before this is still a wallpaper.
 */
#define WALL_MAX_W 160
#define WALL_MAX_H 120
#define WALL_COL0  64
#define WALL_COLS  192
#define WALL_HDR   (4 + 2 + 2 + WALL_COLS * 3)

static uint8_t *wall;              /* the picture, or NULL */
static int wall_w, wall_h;
static int wall_rgb;               /* three bytes a pixel, no palette */

/* Reads the wallpaper named in the settings. Anything wrong with the
   file - missing, too big, not a picture - leaves the gradient in
   place: a background is not something to fail on. */
static void wallpaper_load(void)
{
    wall = (uint8_t *)0;
    wall_w = wall_h = 0;
    wall_rgb = 0;

    const char *name = config_get("wallpaper");
    if (!name || !name[0] || !fs_ready())
        return;

    uint8_t head[WALL_HDR];
    int slot = fs_open(FS_ROOT, name, 0);
    if (slot < 0)
        return;
    if (fs_read_at(slot, 0, head, sizeof(head)) != (int)sizeof(head))
        return;
    if (head[0] != 'T' || head[1] != 'W')
        return;
    int rgb = head[2] == 'L' && head[3] == '2';
    if (!rgb && (head[2] != 'A' || head[3] != 'L'))
        return;

    int w = head[4] | (head[5] << 8);
    int h = head[6] | (head[7] << 8);
    if (w <= 0 || h <= 0 || w > WALL_MAX_W || h > WALL_MAX_H)
        return;

    /* The colours-per-pixel file carries no palette, so its pixels
       begin right after the eight bytes of header. */
    uint32_t at = rgb ? 8 : WALL_HDR;
    uint32_t bytes = (uint32_t)(w * h) * (rgb ? 3 : 1);
    uint8_t *buf = kmalloc(bytes);
    if (!buf)
        return;
    if (fs_read_at(slot, at, buf, bytes) != (int)bytes) {
        kfree(buf);
        return;
    }
    if (rgb) {
        wall = buf;
        wall_w = w;
        wall_h = h;
        wall_rgb = 1;
        printk("[desktop] wallpaper '%s': %dx%d in colour\n", name, w, h);
        return;
    }

    /* The picture's own colours go in above the desktop's, so the two
       never argue about an index */
    for (int i = 0; i < WALL_COLS; i++) {
        const uint8_t *c = head + 8 + i * 3;
        video_set_palette((uint8_t)(WALL_COL0 + i), c[0], c[1], c[2]);
    }
    wall = buf;
    wall_w = w;
    wall_h = h;
    printk("[desktop] wallpaper '%s': %dx%d\n", name, w, h);
}

/* --- The background ---------------------------------------------------------
 *
 * Drawn once into a buffer the size of the screen and copied from there
 * every frame: the aurora is a few million operations, a wallpaper
 * stretched over the screen a multiply per pixel, and neither changes
 * from one frame to the next. With it, a second buffer: the strip of
 * the background under the menu bar, already blurred and tinted - the
 * bar's glass. Nothing but the background is ever under the bar
 * (windows stay below it), so the glass is drawn once too.
 *
 * Both are drawn again when what they show changes - the theme, the
 * background or the wallpaper in the settings - which is checked about
 * once a second. */
static uint32_t *bg_buf, *bar_buf;
static uint32_t bg_pages, bar_pages;
static int bg_key = -1;            /* what they were drawn for */
static int theme_dark = 1;
static int look_wait;

/* The six flat choices, from a little brighter at the top to a little
   darker at the floor - 0 is the aurora */
static const struct { uint8_t r, g, b; } backgrounds[] = {
    {  11,  16,  48 },   /* 0 - the aurora (this is its 256-colour stand-in) */
    {   0,   0, 130 },   /* 1 - navy */
    {  81,   0, 121 },   /* 2 - violet */
    {   0, 113,   0 },   /* 3 - green */
    { 162,  81,   0 },   /* 4 - brown */
    {  48,  48,  48 },   /* 5 - grey */
    {   0, 130, 162 },   /* 6 - sea */
};
#define BG_N ((int)(sizeof(backgrounds) / sizeof(backgrounds[0])))

static int background_choice(void)
{
    uint32_t bg = config_get_num("background", 0);
    return bg < (uint32_t)BG_N ? (int)bg : 0;
}

static uint32_t *take_pages(uint32_t bytes, uint32_t *have)
{
    uint32_t pages = (bytes + 4095) / 4096;
    if (*have >= pages)
        return (uint32_t *)0;          /* the old one will do */
    uint32_t a = pmm_alloc_contiguous(pages);
    if (!a)
        return (uint32_t *)0;
    *have = pages;
    return (uint32_t *)(uintptr_t)a;
}

/* The 256-colour palette for the background bands */
static void desk_palette(int bg)
{
    video_set_palette(COL_DESK, backgrounds[bg].r, backgrounds[bg].g,
                      backgrounds[bg].b);
    for (int i = 0; i < COL_DESK_N; i++) {
        /* 115% at the top, 75% at the bottom, in even steps */
        int num = 115 - i * (115 - 75) / (COL_DESK_N - 1);
        int r = backgrounds[bg].r * num / 100;
        int g = backgrounds[bg].g * num / 100;
        int b = backgrounds[bg].b * num / 100;
        video_set_palette((uint8_t)(COL_DESK0 + i),
                          (uint8_t)(r > 255 ? 255 : r),
                          (uint8_t)(g > 255 ? 255 : g),
                          (uint8_t)(b > 255 ? 255 : b));
    }
}

static void paint_colour(uint32_t *buf, int w, int h, int bg);

static void paint_background(uint32_t *buf, int w, int h, int bg)
{
    if (wall) {
        /* A picture: every pixel of it covers a block of the screen */
        for (int y = 0; y < h; y++) {
            int sy = y * wall_h / h;
            for (int x = 0; x < w; x++) {
                int sx = x * wall_w / w;
                uint32_t c;
                if (wall_rgb) {
                    const uint8_t *p = wall + ((long)sy * wall_w + sx) * 3;
                    c = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
                } else {
                    c = video_palette_rgb((uint8_t)(WALL_COL0 +
                                                    wall[sy * wall_w + sx]));
                }
                buf[(long)y * w + x] = c;
            }
        }
        return;
    }
    if (bg == 0) {
        aurora_draw(buf, w, h, w, theme_dark);
        return;
    }
    paint_colour(buf, w, h, bg);
}

int desktop_bg_preview(int bg, int dark, int w, int h, uint32_t *out)
{
    if (bg < 0 || bg >= BG_N || w <= 0 || h <= 0 || w * h > BG_PREVIEW_MAX)
        return -1;
    if (bg == 0)
        aurora_draw(out, w, h, w, dark);
    else
        paint_colour(out, w, h, bg);
    return 0;
}

/* A colour: smooth, from 115% at the top to 75% at the floor */
static void paint_colour(uint32_t *buf, int w, int h, int bg)
{
    for (int y = 0; y < h; y++) {
        int num = 115 - y * (115 - 75) / (h > 1 ? h - 1 : 1);
        int r = backgrounds[bg].r * num / 100;
        int g = backgrounds[bg].g * num / 100;
        int b = backgrounds[bg].b * num / 100;
        uint32_t c = ((uint32_t)(r > 255 ? 255 : r) << 16) |
                     ((uint32_t)(g > 255 ? 255 : g) << 8) |
                     (uint32_t)(b > 255 ? 255 : b);
        for (int x = 0; x < w; x++)
            buf[(long)y * w + x] = c;
    }
}

/* Reads the theme and the background from the settings and, when they
   changed, draws what depends on them again */
static void look_refresh(int force)
{
    if (!force && --look_wait > 0)
        return;
    look_wait = FRAME_HZ;

    const char *t = config_get("theme");
    int dark = !(t && !strcmp(t, "light"));
    int bg = background_choice();
    int key = (dark << 8) | bg;
    if (!force && key == bg_key)
        return;
    bg_key = key;
    theme_dark = dark;
    L = dark ? look_dark : look_light;
    desk_palette(bg);

    if (!video_true_colour())
        return;                        /* bands of the palette will do */
    int w = video_w(), h = video_h();
    uint32_t *nb = take_pages((uint32_t)(w * h) * 4, &bg_pages);
    if (nb)
        bg_buf = nb;
    if (!bg_buf)
        return;
    paint_background(bg_buf, w, h, bg);

    /* The bar's glass: the strip under it, with what lies below it for
       the blur to read, blurred and tinted */
    int strip = BAR_H + 4 * BAR_BLUR;
    if (strip > h)
        strip = h;
    uint32_t *nbar = take_pages((uint32_t)(w * strip) * 4, &bar_pages);
    if (nbar)
        bar_buf = nbar;
    if (!bar_buf)
        return;
    memcpy(bar_buf, bg_buf, (uint32_t)(w * strip) * 4);
    struct gfx_surface s = { bar_buf, w, strip, w };
    gfx_target(&s);
    gfx_blur(0, 0, w, BAR_H, BAR_BLUR);
    gfx_fill(0, 0, w, BAR_H, L.bar_tint, L.bar_a);
    gfx_target((const struct gfx_surface *)0);
}

static void draw_background(void)
{
    if (bg_buf) {
        video_blit_rgb(0, 0, video_w(), video_h(), bg_buf, video_w());
        return;
    }
    /* Without a buffer: sixteen bands of the palette, or the picture
       stretched pixel by pixel */
    int h = video_h();
    if (wall && !wall_rgb) {
        int sw = video_w();
        for (int y = 0; y < h; y++) {
            const uint8_t *row = wall + (y * wall_h / h) * wall_w;
            for (int x = 0; x < sw; x++)
                video_putpixel(x, y, (uint8_t)(WALL_COL0 +
                                               row[x * wall_w / sw]));
        }
        return;
    }
    for (int i = 0; i < COL_DESK_N; i++) {
        int y0 = i * h / COL_DESK_N;
        int y1 = (i + 1) * h / COL_DESK_N;
        video_fill_rect(0, y0, video_w(), y1 - y0,
                        (uint8_t)(COL_DESK0 + i));
    }
}

/* A small unsigned number into a buffer; returns how many characters
   it wrote. */
static int put_num(char *d, int v)
{
    char tmp[8];
    int n = 0;
    if (v < 0)
        v = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 7);
    for (int i = 0; i < n; i++)
        d[i] = tmp[n - 1 - i];
    return n;
}

/* The date and the time on the bar: "Sat 13 Jun 12:34:56".
 *
 * Reading the CMOS clock means waiting out an update in progress and
 * then reading it twice until two reads agree, so it is not something
 * to do thirty times a second for a digit that changes once a second.
 * The reading is kept and refreshed about five times a second, which is
 * often enough that no second is ever skipped on screen. */
static char clock_text[24] = "--:--:--";
static int clock_wait;

/* The day of the week, 0 = Sunday (Sakamoto's method) */
static int weekday(int y, int m, int d)
{
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3)
        y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[(m - 1) % 12] + d) % 7;
}

static void clock_refresh(void)
{
    if (--clock_wait > 0)
        return;
    clock_wait = 6;

    static const char *days[7] = { "Sun", "Mon", "Tue", "Wed", "Thu",
                                   "Fri", "Sat" };
    static const char *months[12] = { "Jan", "Feb", "Mar", "Apr", "May",
                                      "Jun", "Jul", "Aug", "Sep", "Oct",
                                      "Nov", "Dec" };
    struct rtc_time t;
    rtc_read(&t);
    int n = 0;
    if (t.month >= 1 && t.month <= 12 && t.day >= 1 && t.day <= 31) {
        const char *dn = days[weekday(t.year, t.month, t.day)];
        for (int i = 0; i < 3; i++)
            clock_text[n++] = dn[i];
        clock_text[n++] = ' ';
        n += put_num(clock_text + n, t.day);
        clock_text[n++] = ' ';
        for (int i = 0; i < 3; i++)
            clock_text[n++] = months[t.month - 1][i];
        clock_text[n++] = ' ';
    }
    clock_text[n++] = (char)('0' + (t.hour / 10) % 10);
    clock_text[n++] = (char)('0' + t.hour % 10);
    clock_text[n++] = ':';
    clock_text[n++] = (char)('0' + (t.min / 10) % 10);
    clock_text[n++] = (char)('0' + t.min % 10);
    clock_text[n++] = ':';
    clock_text[n++] = (char)('0' + (t.sec / 10) % 10);
    clock_text[n++] = (char)('0' + t.sec % 10);
    clock_text[n] = 0;
}

/* --- The hint ------------------------------------------------------------
 *
 * A name in a small rounded box: what a tile in the dock is, what a
 * button on the bar does. Remembered rather than drawn on the spot - it
 * belongs on top of everything, the menu included - and it lives one
 * frame, so it is asked for again for as long as the pointer stays.
 *
 * Given as the point it belongs to and which side of it to go: above
 * a tile in the dock, below a button on the bar. */
static int hint_x, hint_y, hint_centred;
static const char *hint_text;
/* What was just saved, and for how many frames to keep saying it: a
   hint lives one frame, and a message about a file wants a moment. */
static const char *shot_note;
static int shot_frames;

static int hint_w(const char *text)
{
    return ui_text_width(text) + 16;
}

/* Over something, centred on x, its bottom edge at y */
static void hint_above(int x, int y, const char *text)
{
    hint_x = x - hint_w(text) / 2;
    hint_y = y - HINT_H;
    hint_text = text;
    hint_centred = 1;
}

/* At a place: its top left corner */
static void hint_at(int x, int y, const char *text)
{
    hint_x = x;
    hint_y = y;
    hint_text = text;
    hint_centred = 0;
}

static void draw_hint(void)
{
    if (!hint_text || !hint_text[0])
        return;
    int w = hint_w(hint_text);
    int x = hint_x, y = hint_y;
    if (x + w > video_w() - 4)
        x = video_w() - 4 - w;
    if (x < 4)
        x = 4;
    gfx_shadow(x, y, w, HINT_H, HINT_R, 6, 70, 2);
    gfx_rrect(x, y, w, HINT_H, HINT_R, L.hint_bg, 240);
    gfx_rring16(x * 16, y * 16, w * 16, HINT_H * 16, HINT_R * 16, 16,
                L.win_line, 160);
    ui_font_text(UI_SANS, UI_PX, x + 8, ui_baseline_in(y, HINT_H), hint_text,
                 L.hint_text);
    hint_text = (const char *)0;      /* one frame, one hint */
}

/* --- The menu bar ----------------------------------------------------------- */

/* The mark: a ring with a thread through it */
static void draw_logo(int x, int y, uint32_t c)
{
    int r = BAR_LOGO_D * 8;
    int cx = x * 16 + r, cy = y * 16 + r;
    gfx_circle16(cx, cy, r, 26, c, 255);
    gfx_disc16(cx, cy, 34, c, 255);
    gfx_line16(cx - r + 10, cy + r - 10, cx + r - 10, cy - r + 10, 18, c, 200);
}

static void draw_bar(void)
{
    int w = video_w();
    if (bar_buf)
        video_blit_rgb(0, 0, w, BAR_H, bar_buf, w);
    else
        gfx_fill(0, 0, w, BAR_H, L.bar_tint, 255);
    gfx_fill(0, BAR_H - 1, w, 1, 0x000000, theme_dark ? 90 : 40);

    int base = ui_font_baseline_in(BAR_PX, 0, BAR_H);

    /* The left end: the mark and the name, which open the menu - lit
       while it is open, like a button held down */
    if (menu_open && menu_py == MENU_Y)
        gfx_rrect(4, 3, logo_w() - 4, BAR_H - 6, 6, L.bar_text, 40);
    draw_logo(BAR_LOGO_X, (BAR_H - BAR_LOGO_D) / 2, L.bar_text);
    int x = ui_font_text(UI_BOLD, BAR_PX, BAR_NAME_X, base, "ThreadOS",
                         L.bar_text);
    /* ... and after them the name of the window that is listening:
       what the keyboard will reach */
    if (focus_idx >= 0 && wins[focus_idx].used && here(focus_idx)) {
        char cut[sizeof(wins[0].title) + 4];
        int room = show_x() - BAR_ITEM_GAP - (x + BAR_APP_GAP);
        ui_font_text(UI_SANS, BAR_PX, x + BAR_APP_GAP, base,
                     fit_text(wins[focus_idx].title, UI_SANS, BAR_PX,
                              room > 0 ? room : 0, cut, sizeof(cut)),
                     L.bar_text);
    }

    /* "Show the desktop": a window drawn small - filled while the
       windows are away, an outline while they are here */
    int sx = show_x(), sy = (BAR_H - 12) / 2;
    if (hidden_by_show)
        gfx_rrect(sx, sy, SHOW_W, 12, 3, L.accent, 255);
    gfx_rring16(sx * 16, sy * 16, SHOW_W * 16, 12 * 16, 3 * 16, 20,
                hidden_by_show ? L.accent_text : L.bar_text, 230);
    gfx_fill(sx + 3, sy + 3, SHOW_W - 6, 2,
             hidden_by_show ? L.accent_text : L.bar_text, 200);
    if (in_rect(mouse_x, mouse_y, sx, 0, SHOW_W, BAR_H)) {
        const char *what = hidden_by_show ? "Restore windows"
                                          : "Show desktop";
        hint_at(sx - hint_w(what) / 2 + SHOW_W / 2, BAR_H + 4, what);
    }

    /* How much of the machine is in use: a capsule, filled */
    load_refresh();
    int lx = load_x(), ly = (BAR_H - LOAD_H) / 2;
    gfx_rrect(lx, ly, LOAD_W, LOAD_H, LOAD_H / 2, L.bar_text, 60);
    int fill = load_percent * LOAD_W / 100;
    if (fill > LOAD_W)
        fill = LOAD_W;
    if (fill < LOAD_H && load_percent > 0)
        fill = LOAD_H;                /* something in use looks like it */
    if (fill > 0)
        gfx_rrect(lx, ly, fill, LOAD_H, LOAD_H / 2, L.accent, 255);
    /* Asked about, it says everything: how much of how much, and how
       many tasks are behind it. */
    if (in_rect(mouse_x, mouse_y, lx, 0, LOAD_W, BAR_H)) {
        static char text[48];
        int n = 0;
        n += put_num(text + n, load_percent);
        const char *a = "% (";
        while (*a)
            text[n++] = *a++;
        n += put_num(text + n, load_used_mib);
        text[n++] = '/';
        n += put_num(text + n, load_ram_mib);
        a = " MiB), ";
        while (*a)
            text[n++] = *a++;
        n += put_num(text + n, load_tasks);
        a = " tasks";
        while (*a)
            text[n++] = *a++;
        text[n] = 0;
        hint_at(lx + LOAD_W - hint_w(text), BAR_H + 4, text);
    }

    /* The four desktops: the one being shown filled with the accent, a
       desktop that holds windows written in the bar's text colour, an
       empty one dimmed - "where am I" and "is there anything over
       there" in one glance */
    int dy = (BAR_H - DESK_CELL_H) / 2;
    for (int d = 0; d < DESK_N; d++) {
        int dx = desk_x0() + d * DESK_CELL;
        int busy = 0;
        for (int i = 0; i < MAX_WIN; i++)
            if (wins[i].used && wins[i].desk == d && wins[i].content)
                busy = 1;
        if (d == cur_desk)
            gfx_rrect(dx + 1, dy, DESK_CELL - 2, DESK_CELL_H, 4, L.accent, 255);
        char n[2] = {(char)('1' + d), 0};
        int nw = ui_font_width(UI_BOLD, UI_PX, n);
        ui_font_text(UI_BOLD, UI_PX, dx + (DESK_CELL - nw) / 2,
                     ui_baseline_in(dy, DESK_CELL_H), n,
                     d == cur_desk ? L.accent_text
                                   : busy ? L.bar_text : L.bar_dim);
    }

    /* The clock: at the right-hand end, set against it */
    clock_refresh();
    int cw = ui_font_width(UI_SANS, BAR_PX, clock_text);
    ui_font_text(UI_SANS, BAR_PX, w - BAR_RIGHT - cw, base, clock_text,
                 L.bar_text);
}

/* --- The dock ------------------------------------------------------------- */


/* The dock does not change from one frame to the next unless
 * something it shows does - and its glass, blurred out of whatever is
 * behind it, is the most expensive thing on the screen. So the frame
 * it was last drawn in is kept, together with two fingerprints: of
 * everything it shows (its size, the theme, which programs run, which
 * windows there are and which one listens) and of the pixels it was
 * drawn over. While both stay the same, the kept picture is put back
 * instead of drawing it again. A tile hopping is a dock that changes
 * every frame, and is just drawn.
 */
#define DOCK_KEEP_M 16                 /* the shadow's reach round the glass */

static uint32_t *dock_keep;
static int dock_keep_tried, dock_kept;
static int dk_x, dk_y, dk_w, dk_h;
static uint32_t dk_state, dk_under;

static uint32_t fp_mix(uint32_t h, uint32_t v)
{
    return (h ^ v) * 16777619u;
}

static uint32_t dock_state_fp(const struct dock *d)
{
    uint32_t h = 2166136261u;
    h = fp_mix(h, (uint32_t)d->x);
    h = fp_mix(h, (uint32_t)d->y);
    h = fp_mix(h, (uint32_t)d->w);
    h = fp_mix(h, (uint32_t)d->tile);
    h = fp_mix(h, (uint32_t)d->n_l);
    h = fp_mix(h, (uint32_t)d->n_w);
    h = fp_mix(h, (uint32_t)theme_dark);
    h = fp_mix(h, L.dock_tint);
    h = fp_mix(h, (uint32_t)focus_idx);
    for (int k = 0; k < d->n_l; k++)
        h = fp_mix(h, (uint32_t)launcher_running(k));
    for (int i = 0; i < MAX_WIN; i++) {
        if (!here(i))
            continue;
        h = fp_mix(h, (uint32_t)i);
        h = fp_mix(h, (uint32_t)(uintptr_t)launch_art_of(&wins[i]));
        h = fp_mix(h, (uint32_t)wins[i].minimized);
        h = fp_mix(h, (uint32_t)(uint8_t)wins[i].title[0]);
    }
    return h;
}

static uint32_t dock_under_fp(int x, int y, int w, int h)
{
    uint32_t f = 2166136261u;
    for (int j = 0; j < h; j++) {
        const uint32_t *row = video_row32(y + j);
        for (int i = 0; i < w; i++)
            f = fp_mix(f, row[x + i]);
    }
    return f;
}

static void draw_dock_glass(const struct dock *d)
{
    gfx_shadow(d->x, d->y, d->w, d->h, DOCK_R, 12, theme_dark ? 120 : 70, 3);
    gfx_frost(d->x, d->y, d->w, d->h, DOCK_R, 8, L.dock_tint, L.dock_a);
    gfx_rring16(d->x * 16, d->y * 16, d->w * 16, d->h * 16, DOCK_R * 16, 16,
                L.dock_line, L.dock_line_a);

    int t = d->tile;
    /* the programs */
    for (int k = 0; k < d->n_l; k++) {
        int tx = dock_tile_x(d, k), ty = d->ty;
        if (hop_set[k]) {
            uint32_t age = frames - hop_at[k];
            if (age < HOP_FRAMES) {
                int up = (int)(age < HOP_FRAMES / 2 ? age : HOP_FRAMES - age);
                ty -= up;
            } else {
                hop_set[k] = 0;
            }
        }
        draw_tile(icons[k].art, tx, ty, t, icons[k].label[0], 255);
        if (launcher_running(k))
            gfx_disc16(tx * 16 + t * 8, (d->ty + t + 1) * 16 + DOCK_DOT * 8,
                       DOCK_DOT * 8, L.dot, 230);
    }
    /* the line between the programs and the windows */
    if (d->n_l && d->n_w) {
        int lx = dock_tile_x(d, d->n_l) - (DOCK_SEP + 1) / 2;
        gfx_fill(lx, d->y + 8, 1, d->h - 16, L.bar_text, 70);
    }
    /* the windows: their program's picture; a minimised one faded, the
       one that is listening with its dot in the accent colour */
    int j = 0;
    for (int i = 0; i < MAX_WIN; i++) {
        if (!here(i))
            continue;
        int tx = dock_tile_x(d, d->n_l + j);
        int from = launch_icon_of(&wins[i]);
        draw_tile(launch_art_of(&wins[i]), tx, d->ty, t,
                  from >= 0 ? icons[from].label[0] : wins[i].title[0],
                  wins[i].minimized ? 110 : 255);
        gfx_disc16(tx * 16 + t * 8, (d->ty + t + 1) * 16 + DOCK_DOT * 8,
                   DOCK_DOT * 8, i == focus_idx ? L.accent : L.dot, 230);
        j++;
    }
}

static void draw_dock(void)
{
    struct dock d;
    dock_geom(&d);

    /* What is kept: the glass and the reach of its shadow */
    int x = d.x - DOCK_KEEP_M, y = d.y - DOCK_KEEP_M;
    int w = d.w + 2 * DOCK_KEEP_M, h = d.h + 2 * DOCK_KEEP_M;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > video_w()) w = video_w() - x;
    if (y + h > video_h()) h = video_h() - y;
    int hopping = 0;
    for (int k = 0; k < d.n_l; k++)
        if (hop_set[k])
            hopping = 1;
    int keep = !hopping && video_row32(0) && w > 0 && h > 0;
    if (keep && !dock_keep && !dock_keep_tried) {
        dock_keep_tried = 1;
        uint32_t px = (uint32_t)video_w() * (DOCK_TILE_L + 2 * DOCK_PAD +
                                             2 * DOCK_KEEP_M);
        dock_keep = (uint32_t *)(uintptr_t)pmm_alloc_contiguous(
            (px * 4 + 4095) / 4096);
    }
    keep = keep && dock_keep && w * h <= video_w() * (DOCK_TILE_L +
                                    2 * DOCK_PAD + 2 * DOCK_KEEP_M);

    uint32_t state = 0, under = 0;
    if (keep) {
        state = dock_state_fp(&d);
        under = dock_under_fp(x, y, w, h);
    }
    if (keep && dock_kept && x == dk_x && y == dk_y && w == dk_w &&
        h == dk_h && state == dk_state && under == dk_under) {
        video_blit_rgb(x, y, w, h, dock_keep, w);
    } else {
        draw_dock_glass(&d);
        dock_kept = 0;
        if (keep) {
            for (int j = 0; j < h; j++)
                memcpy(dock_keep + j * w, video_row32(y + j) + x,
                       (uint32_t)w * 4);
            dk_x = x;
            dk_y = y;
            dk_w = w;
            dk_h = h;
            dk_state = state;
            dk_under = under;
            dock_kept = 1;
        }
    }

    /* The name of the tile under the pointer, over it */
    int t = d.tile;
    for (int k = 0; k < d.n_l + d.n_w; k++) {
        int tx = dock_tile_x(&d, k);
        if (!in_rect(mouse_x, mouse_y, tx - DOCK_GAP / 2, d.y,
                     t + DOCK_GAP, d.h))
            continue;
        if (k < d.n_l) {
            hint_above(tx + t / 2, d.y - 6, icons[k].label);
        } else {
            int j = 0;
            for (int i = 0; i < MAX_WIN; i++) {
                if (!here(i))
                    continue;
                if (j++ == k - d.n_l)
                    hint_above(tx + t / 2, d.y - 6, wins[i].title);
            }
        }
    }
}

/* --- Notifications ----------------------------------------------------------
 *
 * Something happened that is worth a glance, and nobody asked: a picture
 * was saved, a program has news. A card slides in at the top right,
 * stays five seconds and goes; a click sends it away sooner. At most
 * four are shown, the newest on top - a fifth pushes the oldest out.
 *
 * desktop_notify() is called from anywhere - a system call on another
 * core, the keyboard's interrupt - so the cards are handed over under a
 * lock and the desktop thread alone draws them.
 */
#define NOTE_LIFE  (5 * FRAME_HZ)
#define NOTE_SLIDE 8                     /* frames to come in */
#define NOTE_FADE  12                    /* frames to go */

struct note {
    char title[40];
    char body[80];
    uint32_t born;                       /* the frame it came in on */
};
static struct note notes[NOTE_SHOWN];    /* [0] is the newest */
static int note_n;
static struct spinlock note_lock = SPINLOCK_FREE;

static void copy_cut(char *d, const char *s, int cap)
{
    int i = 0;
    for (; s && s[i] && i < cap - 1; i++)
        d[i] = s[i];
    d[i] = '\0';
}

void desktop_notify(const char *title, const char *body)
{
    irqflags_t fl = spin_lock_irq(&note_lock);
    if (note_n < NOTE_SHOWN)
        note_n++;
    for (int i = note_n - 1; i > 0; i--)
        notes[i] = notes[i - 1];
    copy_cut(notes[0].title, title, sizeof(notes[0].title));
    copy_cut(notes[0].body, body, sizeof(notes[0].body));
    notes[0].born = frames;
    spin_unlock_irq(&note_lock, fl);
}

static void note_drop(int i)
{
    irqflags_t fl = spin_lock_irq(&note_lock);
    if (i >= 0 && i < note_n) {
        for (int k = i; k < note_n - 1; k++)
            notes[k] = notes[k + 1];
        note_n--;
    }
    spin_unlock_irq(&note_lock, fl);
}

static int note_x(void) { return video_w() - NOTE_RIGHT - NOTE_W; }
static int note_y(int i) { return NOTE_TOP + i * (NOTE_H + NOTE_GAP); }

/* Which card is under the point, or -1 */
static int note_at(int x, int y)
{
    for (int i = 0; i < note_n; i++)
        if (in_rect(x, y, note_x(), note_y(i), NOTE_W, NOTE_H))
            return i;
    return -1;
}

static void draw_notes(void)
{
    /* The old ones go first: a card past its time is not drawn */
    for (int i = note_n - 1; i >= 0; i--)
        if (frames - notes[i].born >= NOTE_LIFE)
            note_drop(i);

    uint32_t m = config_get_num("motion", 1);
    for (int i = 0; i < note_n; i++) {
        struct note n = notes[i];
        uint32_t age = frames - n.born;
        int x = note_x(), y = note_y(i), a = 255;
        if (m && age < NOTE_SLIDE) {     /* in from the right, slowing */
            int u = (int)(NOTE_SLIDE - age);
            x += (NOTE_W + NOTE_RIGHT) * u * u / (NOTE_SLIDE * NOTE_SLIDE);
        }
        if (m && age + NOTE_FADE > NOTE_LIFE)
            a = 255 * (int)(NOTE_LIFE - age) / NOTE_FADE;
        gfx_shadow(x, y, NOTE_W, NOTE_H, NOTE_R, 14,
                   (theme_dark ? 150 : 90) * a / 255, 4);
        gfx_frost(x, y, NOTE_W, NOTE_H, NOTE_R, 10, L.menu_bg,
                  L.menu_a * a / 255);
        gfx_rring16(x * 16, y * 16, NOTE_W * 16, NOTE_H * 16, NOTE_R * 16,
                    16, L.win_line, 200 * a / 255);
        if (a < 160)
            continue;                    /* going: the glass alone */
        int iy = y + (NOTE_H - NOTE_ICON) / 2;
        gfx_rrect(x + 12, iy, NOTE_ICON, NOTE_ICON, 7, L.accent, 255);
        char first[2] = { n.title[0] ? n.title[0] : '!', 0 };
        int fw = ui_font_width(UI_BOLD, 15, first);
        ui_font_text(UI_BOLD, 15, x + 12 + (NOTE_ICON - fw) / 2,
                     ui_font_baseline_in(15, iy, NOTE_ICON), first,
                     L.accent_text);
        char cut[sizeof(n.body) + 4];
        int room = NOTE_W - NOTE_TEXT_X - 12;
        ui_font_text(UI_BOLD, NOTE_PX, x + NOTE_TEXT_X, y + NOTE_TITLE_Y,
                     fit_text(n.title, UI_BOLD, NOTE_PX, room, cut,
                              sizeof(cut)), L.menu_text);
        ui_font_text(UI_SANS, NOTE_PX, x + NOTE_TEXT_X, y + NOTE_BODY_Y,
                     fit_text(n.body, UI_SANS, NOTE_PX, room, cut,
                              sizeof(cut)), L.menu_text);
    }
}

/* --- The search -------------------------------------------------------------
 *
 * Ctrl+Space, and a field under the bar: what is typed is looked for
 * among the programs, the open windows, the settings and the files at
 * the top of the disk, and whatever matches is listed under it - the
 * names that begin with it first. The arrows pick, Enter takes: a
 * program starts, a window comes up (out of the dock, if it was
 * minimised), a setting opens the settings, a file opens in the
 * program for it. Esc, a click outside or Ctrl+Space again closes it.
 *
 * The keys arrive in the keyboard's interrupt; what they do to the
 * screen - and starting programs above all - is done by the desktop's
 * own thread, which looks at what was asked once per frame.
 */
#define FIND_APP     0
#define FIND_WINDOW  1
#define FIND_SETTING 2
#define FIND_FILE    3
#define FIND_MAX     SEARCH_ROWS

struct found {
    int kind;
    int index;                       /* the program, the window */
    int art;
    char name[32];
};

static char search_q[32];
static volatile int search_len;
static volatile int search_pick;
static volatile int search_take;     /* Enter: the desktop thread acts */
static volatile uint32_t search_gen; /* bumped by every key that changes q */
static uint32_t found_gen = (uint32_t)-1;
static struct found found[FIND_MAX];
static int found_n;

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

/* 2 = the name begins with q, 1 = it has it inside, 0 = no */
static int match(const char *name, const char *q, int qn)
{
    if (!qn)
        return 0;
    for (int at = 0; name[at]; at++) {
        int k = 0;
        while (k < qn && name[at + k] && lower(name[at + k]) == lower(q[k]))
            k++;
        if (k == qn)
            return at == 0 ? 2 : 1;
    }
    return 0;
}

static void found_add(int kind, int index, int art, const char *name,
                      int score, int *scores)
{
    /* Kept in order: better first, and of two as good the one found
       first - a program before a window before a setting before a file */
    int at = found_n;
    while (at > 0 && scores[at - 1] < score)
        at--;
    if (at >= FIND_MAX)
        return;
    int last = found_n < FIND_MAX ? found_n : FIND_MAX - 1;
    for (int i = last; i > at; i--) {
        found[i] = found[i - 1];
        scores[i] = scores[i - 1];
    }
    found[at].kind = kind;
    found[at].index = index;
    found[at].art = art;
    copy_cut(found[at].name, name, sizeof(found[at].name));
    scores[at] = score;
    if (found_n < FIND_MAX)
        found_n++;
}

static void search_refresh(void)
{
    if (found_gen == search_gen)
        return;
    found_gen = search_gen;
    found_n = 0;
    int scores[FIND_MAX];
    char q[sizeof(search_q)];
    int qn = search_len;
    for (int i = 0; i < qn; i++)
        q[i] = search_q[i];

    for (int k = 0; k < KNOWN_N; k++) {
        int s = match(known_apps[k].label, q, qn);
        int s2 = match(known_apps[k].app, q, qn);
        if (s2 > s)
            s = s2;
        if (s)
            found_add(FIND_APP, k, k, known_apps[k].label, s, scores);
    }
    for (int i = 0; i < MAX_WIN; i++) {
        if (!wins[i].used || wins[i].desk != cur_desk)
            continue;
        int s = match(wins[i].title, q, qn);
        if (s)
            found_add(FIND_WINDOW, i, launch_art_of(&wins[i]), wins[i].title,
                      s, scores);
    }
    for (int i = 0; i < config_count(); i++) {
        const char *n = config_name(i);
        if (!n || config_is_hidden(n))
            continue;
        int s = match(n, q, qn);
        if (s)
            found_add(FIND_SETTING, i, art_of_app("gsettings"), n, s, scores);
    }
    char name[64];
    uint32_t size;
    int dir;
    for (int i = 0; i < 64 && fs_readdir(FS_ROOT, i, name, &size, &dir); i++) {
        if (dir)
            continue;
        int s = match(name, q, qn);
        if (s)
            found_add(FIND_FILE, i, art_of_app("gedit"), name, s, scores);
    }
    if (search_pick >= found_n)
        search_pick = found_n ? found_n - 1 : 0;
}

static void search_close(void)
{
    search_open = 0;
    search_len = 0;
    search_q[0] = '\0';
    search_pick = 0;
    search_take = 0;
    search_gen++;
}

/* A key, while the search is open; 1 when it was taken */
static int search_key(char c)
{
    if (!search_open)
        return 0;
    if (c == 27) {
        search_close();
    } else if (c == '\n' || c == '\r') {
        search_take = 1;
    } else if (c == KEY_DOWN) {
        if (search_pick < found_n - 1)
            search_pick++;
    } else if (c == KEY_UP) {
        if (search_pick > 0)
            search_pick--;
    } else if (c == '\b') {
        if (search_len > 0) {
            search_q[--search_len] = '\0';
            search_pick = 0;
            search_gen++;
        }
    } else if (c >= 0x20 && c < 0x7F &&
               search_len < (int)sizeof(search_q) - 1) {
        search_q[search_len++] = c;
        search_q[search_len] = '\0';
        search_pick = 0;
        search_gen++;
    }
    return 1;
}

static int ends_with(const char *s, const char *tail)
{
    int n = (int)strlen(s), t = (int)strlen(tail);
    return n >= t && !strcmp(s + n - t, tail);
}

/* What a result does when it is taken */
static void search_go(int i)
{
    if (i < 0 || i >= found_n)
        return;
    struct found f = found[i];
    search_close();
    if (f.kind == FIND_APP) {
        int k = icon_of_app(known_apps[f.index].app);
        remember_launch(exec_spawn(known_apps[f.index].app), k, f.art);
        if (k >= 0) {
            hop_at[k] = frames;
            hop_set[k] = 1;
        }
    } else if (f.kind == FIND_WINDOW) {
        if (wins[f.index].used) {
            if (wins[f.index].minimized)
                anim_start(f.index, ANIM_RESTORE);
            wins[f.index].minimized = 0;
            focus_idx = f.index;
            raise_win(f.index);
        }
    } else if (f.kind == FIND_SETTING) {
        remember_launch(exec_spawn("gsettings"), icon_of_app("gsettings"),
                        f.art);
    } else {
        /* A picture to the viewer, anything else to the editor */
        const char *with = ends_with(f.name, ".png") ||
                           ends_with(f.name, ".jpg") ||
                           ends_with(f.name, ".tsc") ? "gimg " : "gedit ";
        char cmd[96];
        int n = 0;
        for (int k = 0; with[k]; k++)
            cmd[n++] = with[k];
        for (int k = 0; f.name[k] && n < (int)sizeof(cmd) - 1; k++)
            cmd[n++] = f.name[k];
        cmd[n] = '\0';
        int art = art_of_app(with[1] == 'i' ? "gimg" : "gedit");
        remember_launch(exec_spawn(cmd), -1, art);
    }
}

static int search_x(void) { return (video_w() - SEARCH_W) / 2; }

static int search_h(void)
{
    return SEARCH_FIELD_H + (found_n ? 8 + found_n * SEARCH_ROW + 8 : 0);
}

static int search_row_y(int i)
{
    return SEARCH_Y + SEARCH_FIELD_H + 8 + i * SEARCH_ROW;
}

/* A click while the search is open: on a result it takes it, anywhere
   else it closes the search */
static void search_click(int mx, int my)
{
    for (int i = 0; i < found_n; i++)
        if (in_rect(mx, my, search_x(), search_row_y(i), SEARCH_W,
                    SEARCH_ROW)) {
            search_go(i);
            return;
        }
    if (!in_rect(mx, my, search_x(), SEARCH_Y, SEARCH_W, search_h()))
        search_close();
}

static void draw_search(void)
{
    if (!search_open)
        return;
    search_refresh();
    if (search_take) {
        search_take = 0;
        if (found_n) {
            search_go(search_pick);
            return;
        }
    }
    int x = search_x(), y = SEARCH_Y, h = search_h();
    gfx_shadow(x, y, SEARCH_W, h, SEARCH_R, 20, theme_dark ? 170 : 100, 6);
    gfx_frost(x, y, SEARCH_W, h, SEARCH_R, 12, L.menu_bg, L.menu_a);
    gfx_rring16(x * 16, y * 16, SEARCH_W * 16, h * 16, SEARCH_R * 16, 16,
                L.win_line, 200);

    /* The field: a surface of its own on the glass, nearly even - what
       is typed is read against it, and a glass that shows the
       background through makes a restless ground for letters */
    gfx_rrect(x + 6, y + 6, SEARCH_W - 12, SEARCH_FIELD_H - 12, SEARCH_R - 4,
              L.menu_bg, 215);
    /* The magnifying glass, and what was typed (or what the field is) */
    int gy = y + SEARCH_FIELD_H / 2;
    gfx_circle16((x + 22) * 16, (gy - 2) * 16, 7 * 16, 30, L.menu_text, 220);
    gfx_line16((x + 27) * 16, (gy + 3) * 16, (x + 32) * 16, (gy + 8) * 16,
               36, L.menu_text, 220);
    int base = ui_font_baseline_in(SEARCH_PX, y, SEARCH_FIELD_H);
    if (search_len) {
        int end = ui_font_text(UI_SANS, SEARCH_PX, x + SEARCH_TEXT_X, base,
                               search_q, L.menu_text);
        gfx_fill(end + 1, y + 12, 2, SEARCH_FIELD_H - 24, L.accent, 255);
    } else {
        ui_font_text(UI_SANS, SEARCH_PX, x + SEARCH_TEXT_X, base,
                     "Search", L.title_dim);
        gfx_fill(x + SEARCH_TEXT_X, y + 12, 2, SEARCH_FIELD_H - 24,
                 L.accent, 255);
    }
    if (!found_n)
        return;
    gfx_fill(x + 12, y + SEARCH_FIELD_H, SEARCH_W - 24, 1, L.win_line, 160);

    static const char *const kinds[] = { "Program", "Window", "Setting",
                                         "File" };
    for (int i = 0; i < found_n; i++) {
        int ry = search_row_y(i);
        int on = i == search_pick;
        if (on)
            gfx_rrect(x + 8, ry, SEARCH_W - 16, SEARCH_ROW, 8, L.accent, 255);
        draw_tile(found[i].art, x + 16, ry + (SEARCH_ROW - SEARCH_ICON) / 2,
                  SEARCH_ICON, found[i].name[0], 255);
        int rb = ui_font_baseline_in(MENU_PX, ry, SEARCH_ROW);
        ui_font_text(UI_SANS, MENU_PX, x + 16 + SEARCH_ICON + 12, rb,
                     found[i].name, on ? L.accent_text : L.menu_text);
        const char *k = kinds[found[i].kind];
        int kw = ui_font_width(UI_SANS, UI_PX, k);
        ui_font_text(UI_SANS, UI_PX, x + SEARCH_W - 18 - kw, rb, k,
                     on ? L.accent_text : L.title_dim);
    }
}

/* --- The launch menu ------------------------------------------------------ */

static void draw_menu(void)
{
    if (!menu_open)
        return;
    int x = menu_px, y = menu_py, h = menu_h();
    gfx_shadow(x, y, MENU_W, h, MENU_R, 14, theme_dark ? 150 : 90, 4);
    gfx_frost(x, y, MENU_W, h, MENU_R, 10, L.menu_bg, L.menu_a);
    gfx_rring16(x * 16, y * 16, MENU_W * 16, h * 16, MENU_R * 16, 16,
                L.win_line, 200);
    for (int i = 0; i < MENU_N; i++) {
        int iy = menu_item_y(i);
        int from = icon_of_app(menu_items[i]);
        int art = from >= 0 ? icons[from].art : art_of_app(menu_items[i]);
        /* The name a person knows it by, which is the one the dock
           shows - the program's own name is for the shell */
        const char *label = from >= 0 ? icons[from].label
                                      : label_of_app(menu_items[i]);
        /* The entry under the pointer wears the accent colour. Without
           it a menu is a list of words that happens to be clickable;
           with it, it answers. */
        int over = in_rect(mouse_x, mouse_y, x, iy, MENU_W, MENU_ROW);
        if (over)
            gfx_rrect(x + 5, iy + 1, MENU_W - 10, MENU_ROW - 2, 6, L.accent,
                      255);
        draw_tile(art, x + 10, iy + (MENU_ROW - MENU_ICON) / 2, MENU_ICON,
                  label[0], 255);
        ui_font_text(UI_SANS, MENU_PX, x + 10 + MENU_ICON + 10,
                     ui_font_baseline_in(MENU_PX, iy, MENU_ROW), label,
                     over ? L.accent_text : L.menu_text);
    }
}

/* Copies a title into a window (trimming it) */
static void set_title(struct window *w, const char *t)
{
    int i = 0;
    for (; t[i] && i < (int)sizeof(w->title) - 1; i++)
        w->title[i] = t[i];
    w->title[i] = '\0';
}

/* Releases windows whose owning process is no longer alive, and
   insists on the ones that were asked to close and did not.

   The insisting is the part that matters. An application that answers
   WEV_CLOSE by ending is reaped here a frame later, having tidied up
   after itself; one that does not answer is killed, because a window
   nobody can close is worse than an application that loses its say. */
static void reap_windows(void)
{
    for (int i = 0; i < MAX_WIN; i++) {
        struct window *w = &wins[i];
        if (!w->used || w->owner < 0)
            continue;

        if (!task_alive(w->owner)) {
            win_buf_free(w->content, w->content_bytes);
            w->content = (void *)0;
            w->content_bytes = 0;
            w->used = 0;
            w->closing = 0;
            if (focus_idx == i)
                focus_idx = -1;
            continue;
        }

        if (w->closing > 0 && --w->closing == 0) {
            /* Time is up. The process is killed and the window goes
               with it on the next pass through here. */
            task_kill(w->owner);
        }
    }
}

/* The desktop's keyboard handler: a key goes to the focused
   application window. When nothing has focus, we pass the key on to
   the previous handler (the kernel shell) - thanks to that commands
   ('run ...') still work. Runs in IRQ1 context. */
static keyboard_char_handler_t prev_key_handler;

/* Alt+Tab: the next window takes the focus and comes to the top.
 *
 * Round the slots in order, from the one that has the focus now. Slot
 * order rather than the order they were last used: it is the order a
 * person can see in the dock, so pressing it twice takes you where
 * looking at the dock says it will.
 *
 * A minimised window is skipped - it is not on the screen, and a focus
 * that cannot be seen is a keyboard typing into nowhere. So are the
 * kernel's own two windows: they have no application behind them to
 * take a key. */
static void focus_next(void)
{
    int start = focus_idx >= 0 ? focus_idx : MAX_WIN - 1;
    for (int k = 1; k <= MAX_WIN; k++) {
        int i = (start + k) % MAX_WIN;
        if (!here(i) || wins[i].minimized || !wins[i].content)
            continue;
        focus_idx = i;
        raise_win(i);
        return;
    }
}

/* Asks the window to close, exactly as its red light does: an application
   is told and given time to end; a kernel window simply goes. */
static void close_window(int i)
{
    struct window *w = &wins[i];
    if (!w->used)
        return;
    if (w->content && w->owner >= 0) {
        win_enqueue(w, WEV_CLOSE, 0, 0, 0);
        w->closing = CLOSE_FRAMES;
        /* The focus STAYS while the window does. A program may decline
           to close - and then Alt+F4 has to reach it a second time,
           which it cannot do if asking once took the keyboard away
           from the very window that is being asked. The focus is let
           go when the window really goes, in reap_windows(). */
    } else {
        w->used = 0;
        if (focus_idx == i)
            focus_idx = -1;
    }
}

/* Alt and a function key. In text mode these switch consoles; here
   there is one screen and nothing to switch, so Alt+F4 means what it
   has meant everywhere for thirty years. */
static void desktop_on_fkey(int fkey)
{
    if (fkey == 3 && focus_idx >= 0)      /* F4 counted from zero */
        close_window(focus_idx);
}

/* A picture of what is on the screen, written to the disk.
 *
 * The format is the machine's own: four bytes "TSCR", the width and
 * the height, the whole palette (two hundred and fifty six colours,
 * three bytes each, so the file says what its numbers mean), and then
 * one byte per pixel - the palette index. That is exactly what the
 * compositor drew, so the viewer has nothing to interpret: it puts the
 * bytes back into a window and they are the same colours again.
 *
 * The rows go out one at a time. A whole screen is three hundred
 * kilobytes and the kernel heap is one megabyte - a buffer that big
 * would be most of it, for a picture. */
#define SHOT_HDR (4 + 2 + 2 + 256 * 3)

static int shot_number;
static volatile int shot_wanted;       /* Print Screen was pressed */

static void save_screenshot(void)
{
    int w = video_w(), h = video_h();
    uint8_t *row = kmalloc((uint32_t)w);
    if (!row)
        return;

    char name[16];
    int p = 0;
    const char *pre = "shot";
    for (int i = 0; pre[i]; i++)
        name[p++] = pre[i];
    p += put_num(name + p, ++shot_number);
    name[p++] = '.';
    name[p++] = 'p';
    name[p++] = 's';
    name[p++] = 'c';
    name[p] = '\0';

    int slot = fs_open(FS_ROOT, name, 1);
    if (slot < 0) {
        kfree(row);
        return;
    }
    fs_truncate(slot);

    /* A screen of real colours is saved in real colours - "TSC2", the
       size, then three bytes a pixel. Trading the glass and the shadows
       for the nearest of 256 palette entries would save a picture of
       some other desktop. */
    if (video_true_colour()) {
        uint8_t hdr2[8] = { 'T', 'S', 'C', '2', (uint8_t)(w & 0xFF),
                            (uint8_t)((w >> 8) & 0xFF), (uint8_t)(h & 0xFF),
                            (uint8_t)((h >> 8) & 0xFF) };
        uint8_t *rgb = kmalloc((uint32_t)w * 3);
        if (!rgb || fs_write_at(slot, 0, hdr2, 8) != 8) {
            if (rgb)
                kfree(rgb);
            kfree(row);
            return;
        }
        uint32_t off = 8;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint32_t c = video_getpixel_rgb(x, y);
                rgb[x * 3] = (uint8_t)(c >> 16);
                rgb[x * 3 + 1] = (uint8_t)(c >> 8);
                rgb[x * 3 + 2] = (uint8_t)c;
            }
            if (fs_write_at(slot, off, rgb, (uint32_t)w * 3) != w * 3)
                break;
            off += (uint32_t)w * 3;
        }
        kfree(rgb);
    } else {
        uint8_t hdr[SHOT_HDR];
        hdr[0] = 'T'; hdr[1] = 'S'; hdr[2] = 'C'; hdr[3] = 'R';
        hdr[4] = (uint8_t)(w & 0xFF);
        hdr[5] = (uint8_t)((w >> 8) & 0xFF);
        hdr[6] = (uint8_t)(h & 0xFF);
        hdr[7] = (uint8_t)((h >> 8) & 0xFF);
        for (int i = 0; i < 256; i++) {
            uint32_t c = video_palette_rgb((uint8_t)i);
            hdr[8 + i * 3 + 0] = (uint8_t)((c >> 16) & 0xFF);
            hdr[8 + i * 3 + 1] = (uint8_t)((c >> 8) & 0xFF);
            hdr[8 + i * 3 + 2] = (uint8_t)(c & 0xFF);
        }
        uint32_t off = 0;
        if (fs_write_at(slot, off, hdr, SHOT_HDR) != SHOT_HDR) {
            kfree(row);
            return;
        }
        off += SHOT_HDR;

        for (int y = 0; y < h; y++) {
            int n = video_read_row(y, row, w);
            if (n <= 0)
                break;
            if (fs_write_at(slot, off, row, (uint32_t)n) != n)
                break;
            off += (uint32_t)n;
        }
    }
    kfree(row);

    /* Said on the screen, because a picture saved in silence is a
       picture nobody knows about. */
    static char note[32];
    int q = 0;
    for (int i = 0; name[i]; i++)
        note[q++] = name[i];
    const char *tail = " saved";
    for (int i = 0; tail[i]; i++)
        note[q++] = tail[i];
    note[q] = '\0';
    shot_note = note;
    shot_frames = 2 * FRAME_HZ;   /* two seconds */
    desktop_notify("Screenshot", note);
}

static void desktop_on_key(char c)
{
    if (c == KEY_SEARCH) {
        if (search_open)
            search_close();
        else {
            search_close();
            search_open = 1;
            menu_open = 0;
        }
        return;
    }
    if (search_key(c))
        return;
    if (c == KEY_SHOT) {
        /* Not here: this is the keyboard's interrupt, and the frame on
           the screen may be half drawn at this moment - the glass of
           the dock and none of its tiles. The desktop's own thread
           takes the picture, between one finished frame and the next. */
        shot_wanted = 1;
        return;
    }
    if (c == KEY_NEXTWIN) {
        focus_next();
        return;
    }
    if (c == KEY_DESK_PREV || c == KEY_DESK_NEXT) {
        desk_switch((cur_desk + (c == KEY_DESK_NEXT ? 1 : DESK_N - 1))
                    % DESK_N);
        return;
    }
    if (c == KEY_DESK_TAKE_PREV || c == KEY_DESK_TAKE_NEXT) {
        desk_take((cur_desk + (c == KEY_DESK_TAKE_NEXT ? 1 : DESK_N - 1))
                  % DESK_N);
        return;
    }
    if (focus_idx >= 0 && focus_idx < MAX_WIN && wins[focus_idx].used &&
        wins[focus_idx].content) {
        win_enqueue(&wins[focus_idx], WEV_KEY, 0, 0, (unsigned char)c);
    } else if (prev_key_handler) {
        prev_key_handler(c);
    }
}

/* --- API for application syscalls (syscall context = IF=0) --- */

int desktop_win_create(int owner, int cw, int ch, const char *title)
{
    if (cw <= 0 || ch <= 0 || cw > video_w() || ch > video_h())
        return -1;
    for (int i = 0; i < MAX_WIN; i++) {
        if (wins[i].used)
            continue;
        uint32_t bytes = (uint32_t)(cw * ch);
        uint8_t *buf = win_buf_alloc(bytes);
        if (!buf)
            return -1;
        memset(buf, COL_WIN, bytes);       /* a grey background to start with */
        struct window *w = &wins[i];
        w->truecolour = 0;                 /* indices until asked otherwise */
        w->content_bytes = bytes;
        w->w = cw + 2;
        w->h = ch + TITLE_H + 2;
        /* New windows go in a cascade, twelve pixels apart. Far enough
           down that cascade, or with a window large enough, the next
           step would put it off the screen - so we walk back up until
           it fits. Walking back rather than clamping keeps the windows
           apart: two clamped to the same corner would look like one. */
        int k = i;
        while (k > 0 && (70 + k * 12 + w->w > video_w() ||
                         30 + k * 12 + w->h > work_bottom()))
            k -= 3;
        if (k < 0)
            k = 0;
        w->x = 70 + k * 12;
        w->y = 30 + k * 12;
        /* Bigger than the screen even at the start of the cascade - all
           that is left is to push it against the edge */
        if (w->x + w->w > video_w())
            w->x = video_w() > w->w ? video_w() - w->w : 0;
        if (w->y + w->h > work_bottom())
            w->y = work_bottom() - w->h > work_top() ? work_bottom() - w->h
                                                     : work_top();
        set_title(w, title);
        w->has_button = 0;
        w->counter = 0;
        w->owner = owner;
        w->content = buf;
        w->cw = cw;
        w->ch = ch;
        w->used = 1;
        w->desk = cur_desk;        /* where the person is looking */
        raise_win(i);              /* a new window on top */
        focus_idx = i;             /* and with focus */
        anim_start(i, ANIM_OPEN);
        return i;
    }
    return -1;
}

/* A window's name, changed by the program that owns it.
 *
 * A window called "Editor" says what the program is; a window called
 * "Editor: a note" says which of the three open ones you are looking
 * for - and that is what a title bar is FOR. The name is also what the
 * dock shows over its tile, so one call fixes both at once. */
int desktop_win_title(int winid, int owner, const char *title)
{
    if (winid < 0 || winid >= MAX_WIN || !title)
        return -1;
    struct window *w = &wins[winid];
    if (!w->used || !w->content || w->owner != owner)
        return -1;
    set_title(w, title);
    return 0;
}

/* "I heard, and I am not closing yet."
 *
 * Closing is a REQUEST - it always was, and the countdown after it was
 * there for programs that do not answer. What was missing was the
 * other answer: a program that HAS heard and has a reason to stay, an
 * editor with a file that was never saved being the whole of the
 * point. So it says so, and the countdown stops; what it does with the
 * moment it bought - a question in its own window, saving the file -
 * is its own business.
 *
 * A program that goes on declining goes on being asked, exactly as
 * everywhere else: refusing to close is a thing an application may do,
 * and the way past it is to ask again. Silence is still what gets a
 * window taken away. */
int desktop_win_keep(int winid, int owner)
{
    if (winid < 0 || winid >= MAX_WIN)
        return -1;
    struct window *w = &wins[winid];
    if (!w->used || !w->content || w->owner != owner)
        return -1;
    if (!w->closing)
        return 0;                  /* nobody was asking */
    w->closing = 0;
    return 1;
}

/* Checks that winid is a window of application owner */
static struct window *app_window(int winid, int owner)
{
    if (winid < 0 || winid >= MAX_WIN)
        return (void *)0;
    struct window *w = &wins[winid];
    if (!w->used || !w->content || w->owner != owner)
        return (void *)0;
    return w;
}

int desktop_win_rect(int winid, int owner, int x, int y, int w, int h,
                     uint8_t color)
{
    struct window *win = app_window(winid, owner);
    if (!win)
        return -1;
    buf_rect(win, x, y, w, h, buf_colour(win, color));
    return 0;
}

/* "From now on my pixels are colours."
 *
 * A window is born holding palette indices, because that is what every
 * program on this machine drew in and none of them should have to
 * change. One that has better colours to show - a photograph, a page
 * whose author picked them - asks for the other kind, once, and from
 * then on writes 0x00RRGGBB through win_blit32. Its chrome can go on
 * using the index calls: they translate.
 *
 * The request is granted whatever the screen is. On a screen that
 * cannot show a colour the compositor trades each one for the nearest
 * index while composing, which is worse than asking the program to do
 * it - but the program then has one path instead of two, and this
 * machine's eight-bit mode is a fallback for when there is no
 * framebuffer at all, not the case to design for. */
int desktop_win_truecolour(int winid, int owner)
{
    struct window *win = app_window(winid, owner);
    if (!win)
        return -1;
    if (win->truecolour)
        return 0;

    uint32_t bytes = (uint32_t)(win->cw * win->ch) * 4u;
    uint32_t *buf = (uint32_t *)win_buf_alloc(bytes);
    if (!buf)
        return -1;
    /* What was already drawn stays drawn - a program that asks after
       painting its chrome does not lose it. */
    for (int j = 0; j < win->ch; j++)
        for (int i = 0; i < win->cw; i++)
            buf[j * win->cw + i] =
                video_palette_rgb(win->content[j * win->cw + i]);
    win_buf_free(win->content, win->content_bytes);
    win->content = (uint8_t *)buf;
    win->content_bytes = bytes;
    win->truecolour = 1;
    return 0;
}

/* A block of pixels straight into a window's content.
 *
 * Everything a program could draw until now was a rectangle of one
 * colour or a line of text. A picture is neither, and drawing it by
 * calling for one rectangle per pixel would be three hundred thousand
 * system calls for one screenshot. This takes a row - or a whole
 * block - at a time, in the same palette indices the compositor draws
 * in, so there is nothing to interpret on either side. */
int desktop_win_blit(int winid, int owner, int x, int y, int w, int h,
                     const uint8_t *pixels)
{
    struct window *win = app_window(winid, owner);
    if (!win || !pixels || w <= 0 || h <= 0)
        return -1;

    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++)
            buf_px(win, x + col, y + row,
                   buf_colour(win, pixels[row * w + col]));
    return 0;
}

/* The same thing in colours rather than indices.
 *
 * This is the call a picture actually wants: sixteen million colours
 * instead of two hundred and fifty six, and no search for the nearest
 * of them on the way in. It only means anything to a window that asked
 * for true colour - one that did not has nowhere to put the other
 * three bytes, so it is told no rather than shown a quarter of the
 * picture. */
int desktop_win_blit32(int winid, int owner, int x, int y, int w, int h,
                       const uint32_t *pixels)
{
    struct window *win = app_window(winid, owner);
    if (!win || !pixels || w <= 0 || h <= 0)
        return -1;
    if (!win->truecolour)
        return -1;

    for (int row = 0; row < h; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= win->ch)
            continue;
        /* A row that lands whole inside the window is a copy - which
           is the whole point of having asked for this format. */
        const uint32_t *src = pixels + (long)row * w;
        uint32_t *dst = (uint32_t *)win->content + (long)dy * win->cw;
        int from = x < 0 ? -x : 0;
        int to = x + w > win->cw ? win->cw - x : w;
        for (int col = from; col < to; col++)
            dst[x + col] = src[col];
    }
    return 0;
}

int desktop_win_draw(int winid, int owner, const struct win_draw *d,
                     const char *s)
{
    int px = d->px > 0 && d->px <= 64 ? d->px : UI_PX;
    int font = d->font == UI_BOLD || d->font == UI_MONO ? d->font : UI_SANS;
    if (d->op == WD_MEASURE)
        return s ? ui_font_width(font, px, s) : 0;

    struct window *win = app_window(winid, owner);
    if (!win)
        return -1;
    if (!win->truecolour && desktop_win_truecolour(winid, owner) < 0)
        return -1;
    /* The window's own pixels, said explicitly: the compositor may be
       in the middle of a frame on another core, drawing somewhere else */
    struct gfx_surface t = { (uint32_t *)win->content, win->cw, win->ch,
                             win->cw };
    int a = d->a < 0 ? 0 : d->a > 255 ? 255 : d->a;
    int r = d->r < 0 ? 0 : d->r;
    switch (d->op) {
    case WD_FILL:
        gfx_fill_s(&t, d->x, d->y, d->w, d->h, d->rgb, a);
        return 0;
    case WD_RRECT:
        gfx_rrect16_s(&t, d->x * 16, d->y * 16, d->w * 16, d->h * 16, r * 16,
                      d->rgb, a);
        return 0;
    case WD_RING:
        gfx_rring16_s(&t, d->x * 16, d->y * 16, d->w * 16, d->h * 16, r * 16,
                      16, d->rgb, a);
        return 0;
    case WD_GRAD:
        gfx_rrect_grad_s(&t, d->x, d->y, d->w, d->h, r, d->rgb, d->rgb2, a);
        return 0;
    case WD_DISC:
        gfx_disc16_s(&t, d->x * 16 + d->w * 8, d->y * 16 + d->w * 8,
                     d->w * 8, d->rgb, a);
        return 0;
    case WD_LINE:
        gfx_line16_s(&t, d->x * 16 + 8, d->y * 16 + 8, d->w * 16 + 8,
                     d->h * 16 + 8, r ? r : 16, d->rgb, a);
        return 0;
    case WD_TEXT:
        return s ? ui_font_text_s(&t, font, px, d->x, d->y, s, d->rgb) : d->x;
    case WD_CELLS: {
        int i = 0;
        for (; s && s[i]; i++) {
            if (s[i] == ' ')
                continue;
            char one[2] = { s[i], 0 };
            ui_font_text_s(&t, font, px, d->x + i * d->w, d->y, one, d->rgb);
        }
        return d->x + i * d->w;
    }
    default:
        return -1;
    }
}

void desktop_ui_look(int which, uint32_t *out)
{
    const struct look *lk = which == 1 ? &look_dark : which == 2 ? &look_light
                                                                 : &L;
    int dark = which == 1 ? 1 : which == 2 ? 0 : theme_dark;
    int k = 0;
    out[k++] = (uint32_t)dark;
    out[k++] = lk->win_bg;
    out[k++] = dark ? DARK_APP_FIELD : LIGHT_APP_FIELD;
    out[k++] = dark ? DARK_APP_TEXT : LIGHT_APP_TEXT;
    out[k++] = dark ? DARK_APP_DIM : LIGHT_APP_DIM;
    out[k++] = lk->win_line;
    out[k++] = dark ? DARK_APP_ROW : LIGHT_APP_ROW;
    out[k++] = lk->accent;
    out[k++] = lk->accent_text;
    out[k++] = dark ? DARK_APP_DANGER : LIGHT_APP_DANGER;
    out[k++] = dark ? DARK_APP_GOOD : LIGHT_APP_GOOD;
}

int desktop_win_text(int winid, int owner, int x, int y, const char *s,
                     uint8_t color)
{
    struct window *win = app_window(winid, owner);
    if (!win)
        return -1;
    buf_text(win, x, y, s, buf_colour(win, color));
    return 0;
}

/* Takes the next window event into *out. Returns 1 when there was one,
   otherwise 0. */
int desktop_win_poll(int winid, int owner, void *out)
{
    struct window *win = app_window(winid, owner);
    if (!win)
        return -1;
    uint32_t flags = irq_save();
    int got = 0;
    if (win->ev_tail != win->ev_head) {
        *(struct wevent *)out = win->evq[win->ev_tail];
        win->ev_tail = (win->ev_tail + 1) % WEV_QLEN;
        got = 1;
    }
    irq_restore(flags);
    return got;
}


/* --- one frame of the kiosk ----------------------------------------
 *
 * Everything the ordinary desktop draws around a window - the bar, the
 * icons, the menu, the other windows - is missing here, and that is
 * the whole difference. What is left is a name for the screen, the one
 * window in the middle of it, and the single thing the environment
 * itself offers: restarting the machine.
 */
static int kiosk_pid = -1;
static int kiosk_uptime;           /* frames the program has been alive */

static void kiosk_frame(int mx, int my)
{
    /* The one program. Not running - never started, or ended - means
       starting it again: a kiosk with nothing in it is a machine that
       looks broken. A program that dies at once is a different matter,
       and after a few tries we stop and say so rather than flicker
       for ever. */
    if (kiosk_pid >= 0 && task_alive(kiosk_pid)) {
        if (++kiosk_uptime > 100)
            kiosk_launches = 0;    /* it ran; whatever it does now is fine */
    } else if (!kiosk_gave_up) {
        kiosk_uptime = 0;
        if (kiosk_wait > 0) {
            kiosk_wait--;
        } else if (kiosk_launches >= KIOSK_TRIES) {
            kiosk_gave_up = 1;
        } else {
            kiosk_launches++;
            kiosk_pid = exec_spawn(kiosk_app);
            kiosk_wait = 33;                   /* a second before asking again */
        }
    }

    ui_font_text(UI_BOLD, BAR_PX + 2, 14,
                 ui_font_baseline_in(BAR_PX + 2, kiosk_btn_y(), KIOSK_BTN_H),
                 "ThreadOS - installation", 0xFFFFFF);

    int bx = kiosk_btn_x(), by = kiosk_btn_y();
    int over = in_rect(mouse_x, mouse_y, bx, by, KIOSK_BTN_W, KIOSK_BTN_H);
    gfx_frost(bx, by, KIOSK_BTN_W, KIOSK_BTN_H, KIOSK_BTN_H / 2, 6,
              over ? L.accent : 0xFFFFFF, over ? 230 : 50);
    gfx_rring16(bx * 16, by * 16, KIOSK_BTN_W * 16, KIOSK_BTN_H * 16,
                KIOSK_BTN_H * 8, 16, 0xFFFFFF, 110);
    int rw = ui_font_width(UI_SANS, UI_PX, "Restart");
    ui_font_text(UI_SANS, UI_PX, bx + (KIOSK_BTN_W - rw) / 2,
                 ui_baseline_in(by, KIOSK_BTN_H), "Restart", 0xFFFFFF);

    /* The window, in the middle. It is put there every frame because
       in this mode nobody may move it - there is nowhere to move it
       to. */
    for (int i = 0; i < MAX_WIN; i++) {
        struct window *w = &wins[i];
        if (!w->used || !w->content)
            continue;
        w->x = (video_w() - w->w) / 2;
        w->y = (video_h() - w->h) / 2;
        if (w->y < 30)
            w->y = 30;
        w->has_button = 0;         /* nothing to close it with */
        w->minimized = 0;
        w->desk = cur_desk;
        focus_idx = i;
        draw_window(w);
    }

    if (kiosk_gave_up) {
        const char *a = "The installer will not start";
        const char *b = "Restart the machine";
        int cx = video_w() / 2, cy = video_h() / 2;
        ui_font_text(UI_BOLD, TITLE_PX, cx - ui_font_width(UI_BOLD, TITLE_PX, a) / 2,
                     cy, a, 0xFFFFFF);
        ui_font_text(UI_SANS, UI_PX, cx - ui_text_width(b) / 2, cy + 18, b,
                     0xFFFFFF);
    }

    draw_hint();
    draw_cursor(mx, my);
    video_present();
    frames++;
    task_sleep(30);
}

/* --- The beat --------------------------------------------------------------
 *
 * Fifty frames a second, kept by the clock. Sleeping a fixed time after
 * every frame made the beat as slow as the drawing plus the sleep, and
 * as uneven as the drawing: a heavy frame pushed every frame after it.
 * Now each frame has its moment, two ticks after the one before, and
 * sleeps only what is left of it; one that ran past its moment does
 * not try to catch up - it gives the others one tick and the beat
 * starts again from there.
 *
 * What it costs is counted over each second: how many frames, and how
 * much of the second went on drawing them ('screen' says both).
 */
#define FRAME_TICKS (100 / FRAME_HZ)   /* 20 ms at 100 ticks a second */

static uint32_t frame_due;
static uint32_t stat_from, stat_frames, stat_busy;
static int fps_last = -1, busy_last;

static void frame_done(uint32_t began)
{
    uint32_t now = pit_ticks();
    stat_frames++;
    stat_busy += now - began;
    if (now - stat_from >= 100) {
        fps_last = (int)(stat_frames * 100 / (now - stat_from));
        busy_last = (int)(stat_busy * 100 / (now - stat_from));
        stat_from = now;
        stat_frames = 0;
        stat_busy = 0;
    }
    frame_due += FRAME_TICKS;
    if ((int32_t)(frame_due - now) <= 0)
        frame_due = now + 1;
    task_sleep((frame_due - now) * 10);
}

int desktop_frame_stats(int *fps, int *busy)
{
    if (fps_last < 0)
        return 0;
    *fps = fps_last;
    *busy = busy_last;
    return 1;
}

static void desktop_thread(void *arg)
{
    (void)arg;
    /* How large a logical pixel is. Zero - the default - means the
       screen decides: a laptop's own resolution gets two, everything
       smaller gets one. A person who disagrees says so once and it is
       remembered, like the background and the wallpaper. */
    video_set_scale((int)config_get_num("scale", 0));

    /* Try the high VESA resolution (640x480x32); when there is no
       framebuffer - fall back to mode 13h (320x200) */
    if (video_init_vesa(640, 480) < 0)
        video_init();

    /* The palette, for the programs' windows and for a screen of 256
       colours. What each entry means has not changed since the first
       window was drawn - programs count on it. */
    video_set_palette(COL_BAR, 211, 211, 211);   /* light grey */
    video_set_palette(COL_WIN, 227, 227, 227);   /* the grey window background */
    video_set_palette(COL_TBBTN, 105, 105, 105); /* dark grey */
    video_set_palette(COL_ICON, 255, 162, 0);    /* orange */
    video_set_palette(COL_LINE, 150, 150, 150);
    video_set_palette(COL_WIN_HI, 247, 247, 247);
    video_set_palette(COL_TITLE_OFF, 178, 178, 178);
    video_set_palette(COL_TBBTN_OFF, 146, 146, 146);
    video_set_palette(COL_DIM, 73, 73, 73);
    video_set_palette(COL_ICON_LO, 186, 113, 0);

    icons_read_list();         /* which programs the dock holds, if the disk says */
    wallpaper_load();          /* and the wallpaper, if the settings name one */
    look_refresh(1);           /* the theme, and the background drawn once */

    if (!kiosk) {
        /* Two kernel windows (no owner, no content buffer) */
        wins[0] = (struct window){.x = 40, .y = 60, .w = 120, .h = 70,
                                  .used = 1, .has_button = 1, .owner = -1};
        set_title(&wins[0], "Terminal");
        raise_win(0);
        wins[1] = (struct window){.x = 180, .y = 95, .w = 120, .h = 70,
                                  .used = 1, .has_button = 0, .owner = -1};
        set_title(&wins[1], "Files");
        raise_win(1);
    }

    for (;;) {
        uint32_t began = pit_ticks();
        reap_windows();
        look_refresh(0);
        anim_settle();

        int mx, my, buttons;
        mouse_get(&mx, &my, &buttons);
        handle_input(mx, my, buttons);

        draw_background();

        if (kiosk) {
            kiosk_frame(mx, my);
            continue;
        }

        int ord[MAX_WIN];
        int n = zorder(ord);
        for (int k = 0; k < n; k++)      /* from the bottom up (Z order) */
            draw_window(&wins[ord[k]]);

        /* The place a dragged window would take: a pane of the accent
           colour, faint, with its edge drawn - the question, before
           letting go answers it */
        if (snap_hint != SNAP_NONE) {
            int hx, hy, hw, hh;
            snap_geom(snap_hint, &hx, &hy, &hw, &hh);
            hx += 4;
            hy += 4;
            hw -= 8;
            hh -= 8;
            gfx_rrect(hx, hy, hw, hh, WIN_R, L.accent, 60);
            gfx_rring16(hx * 16, hy * 16, hw * 16, hh * 16, WIN_R * 16, 32,
                        L.accent, 220);
        }

        draw_bar();
        draw_dock();
        draw_menu();
        draw_notes();
        draw_search();
        /* What was just saved stays on the screen for a moment - the
           hint itself lives one frame, so it is asked for again until
           the moment is up. */
        if (shot_frames > 0) {
            shot_frames--;
            if (shot_note)
                hint_at(8, BAR_H + 4, shot_note);
        }
        draw_hint();               /* above everything, including the menu */
        draw_cursor(mx, my);

        video_present();                      /* the finished frame -> the screen */

        if (shot_wanted) {                    /* a whole frame, nothing half drawn */
            shot_wanted = 0;
            save_screenshot();
        }
        frames++;
        frame_done(began);
    }
}

static int compositor_running;

int desktop_start_kiosk(const char *program)
{
    /* Only before the compositor is up: a desktop already on the
       screen cannot become a kiosk, because the windows that are
       already open would have nowhere to go. */
    if (compositor_running || !program)
        return 0;
    kiosk = 1;
    kiosk_app = program;
    desktop_start();
    return 1;
}

void desktop_start(void)
{
    if (compositor_running)                      /* one compositor is enough */
        return;
    compositor_running = 1;

    /* From here the screen is the compositor's. Whatever used to write
       into the corner of the text screen - the mouse's position, the
       uptime - would now be drawing on top of the desktop. */
    vga_screen_taken(1);

    prev_key_handler = keyboard_console_handler();  /* the shell of the visible console */
    keyboard_set_override(desktop_on_key);          /* keys -> the focused window */
    keyboard_set_fkey_override(desktop_on_fkey);    /* ...and Alt+F4 */
    task_create("desktop", desktop_thread, (void *)0);
}
