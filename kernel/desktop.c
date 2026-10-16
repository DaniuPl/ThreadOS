/* The ThreadOS graphical desktop - a simple compositor
 *
 * It runs as a kernel thread: in a loop it redraws the whole frame
 * into the framebuffer (background -> cursor) and gives up the CPU for
 * ~30 ms. At 320x200 a full redraw is 64 KB - cheap enough that we do
 * not bother saving the background under the cursor.
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

/* Desktop palette indices (set in desktop_thread)
 *
 * The screen is eight bits per pixel with a palette of our own, so a
 * shade costs an index and nothing else. That is what the depth here
 * is made of: an edge lit from the top left and shaded at the bottom
 * right, a title bar that is not one flat colour, a background that
 * darkens towards the floor. Every one of them is a rectangle in a
 * slightly different entry of the same table.
 */
#define COL_BLACK  0
#define COL_TITLE  3      /* the window title bar (blue) */
#define COL_WHITE  4
#define COL_DESK   6      /* the desktop background (the middle shade) */
#define COL_BAR    7      /* the top bar (light grey) */
#define COL_WIN    8      /* the window background (grey) */
#define COL_TBBTN  9      /* a window button on the taskbar (dark grey) */
#define COL_ICON   10     /* a desktop icon (orange) */

/* --- the few tones a flat surface needs -----------------------------
 *
 * No lit and shaded edges, no shadows, no gradient on a button: a
 * surface is one colour, and what separates two surfaces is a single
 * line a shade darker. What is left to say with colour is then the
 * only thing worth saying with it - which window is listening.
 */
#define COL_LINE     11   /* the line between one surface and the next */
#define COL_WIN_HI   12   /* a lighter surface: fields, the menu */
#define COL_TITLE_OFF 13  /* the title bar of a window without focus */
#define COL_TBBTN_OFF 14  /* a taskbar button that is not the focused one */
#define COL_DIM      15   /* text on a surface that is not the accent */
#define COL_ICON_LO  16   /* the icon tile, a shade down */

/* The background is a gradient: sixteen bands, sixteen entries,
   darkening towards the floor. Sixteen rather than six because six can
   be counted with the eye - the steps have to be small enough that
   what is seen is a slope and not a staircase. */
#define COL_DESK0    20
#define COL_DESK_N   16

#define TITLE_H 10

/* Desktop icons: clicking one launches an application */
#define ICON_W 32
#define ICON_H 24
/* An icon: where it sits, what it says, what it starts, and which of
   the pictures below it wears.
 *
 * The list of them used to be this array and nothing else - seven
 * programs decided when the kernel was built. It is now a file on the
 * disk (see icons_read_list), and this array is what a machine
 * without that file gets. */
#define ICON_MAX 12
struct icon {
    int x, y;
    const char *label;
    const char *app;
    int art;                   /* which picture, -1 = by position */
    char store[2][24];         /* room for a label and a name read from
                                  the disk - the built-in ones point at
                                  their own literals */
};
static struct icon icons[ICON_MAX] = {
    {8, 20, "App", "guiapp", 0, {{0}}},
    {8, 60, "Clock", "gclock", 1, {{0}}},
    {8, 100, "Files", "gfiles", 2, {{0}}},
    {8, 140, "Monitor", "gmon", 3, {{0}}},
    {8, 180, "Term", "gterm", 4, {{0}}},
    {8, 220, "Web", "gweb", 5, {{0}}},
    {8, 260, "Config", "gsettings", 6, {{0}}},
    {8, 300, "Editor", "gedit", 7, {{0}}},
    {8, 340, "Tasks", "gtasks", 8, {{0}}},
};
static int icon_n = 9;         /* how many of them there are */

/* What each icon has drawn on it: twelve rows of twelve pixels, in the
 * order of the table above.
 *
 * A coloured rectangle with a word under it is a label, not an icon -
 * it says nothing until it is read. A picture is recognised before it
 * is read, which is the whole reason icons exist. Twelve by twelve is
 * not much room, so each of these is one idea and no detail: a window,
 * a clock face, a folder, bars of a chart, a prompt, a globe, a
 * cogwheel.
 *
 * The bits are read from the top: 0x800 is the leftmost pixel.
 */
static const uint16_t icon_art[][12] = {
    {   /* App - a little window */
        0xFFF, 0x801, 0xFFF, 0x801, 0x801, 0x801,
        0x801, 0x801, 0x801, 0x801, 0x801, 0xFFF,
    },
    {   /* Clock - a clock face with hands */
        0x1F8, 0x30C, 0x606, 0xC23, 0xC23, 0xC23,
        0xC3B, 0xC03, 0x606, 0x30C, 0x1F8, 0x000,
    },
    {   /* Files - a folder */
        0x000, 0xF00, 0xFFE, 0x802, 0x802, 0x802,
        0x802, 0x802, 0x802, 0x802, 0xFFE, 0x000,
    },
    {   /* Monitor - bars of a chart */
        0x000, 0x030, 0x030, 0x0F0, 0x0F0, 0x3F0,
        0x3FC, 0xFFC, 0xFFC, 0xFFF, 0xFFF, 0x000,
    },
    {   /* Term - a prompt and a place to type */
        0x000, 0xC00, 0x600, 0x300, 0x180, 0x0C0,
        0x180, 0x300, 0x600, 0xC00, 0x000, 0x0FC,
    },
    {   /* Web - a globe with meridians */
        0x1F8, 0x3FC, 0x6DE, 0xCDB, 0xCDB, 0xFFF,
        0xCDB, 0xCDB, 0x6DE, 0x3FC, 0x1F8, 0x000,
    },
    {   /* Ustaw - a cogwheel */
        0x0F0, 0x2F4, 0x7FE, 0xFFF, 0xF0F, 0xE07,
        0xE07, 0xF0F, 0xFFF, 0x7FE, 0x2F4, 0x0F0,
    },
    {   /* Edytor - a sheet of paper with writing on it */
        0xFFC, 0x804, 0x9E4, 0x804, 0x9F4, 0x804,
        0x9E4, 0x804, 0x9F4, 0x804, 0xFFC, 0x000,
    },
    {   /* Zadania - a list of rows, one of them picked out */
        0x000, 0xFFF, 0x000, 0xEEE, 0x000, 0xFFF,
        0x000, 0xEEE, 0x000, 0xFFF, 0x000, 0x000,
    },
};
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

/* The disk file with the remembered icon positions (x,y pairs in order) */
#define ICON_POS_FILE "icons.pos"

/* Which picture belongs to which running program.
 *
 * A window knows the process that owns it and nothing else - not what
 * was clicked to start it. So the desktop remembers, when it launches
 * something, which icon it came from; the window created a moment
 * later by that process takes the same picture. Nothing else can
 * connect the two: the compositor sees a pid, and the icons are the
 * only place where a program has a face.
 */
#define LAUNCH_MAX 16
static struct { int pid; int icon; } launched[LAUNCH_MAX];
static int launch_next;

static void remember_launch(int pid, int icon)
{
    if (pid < 0 || icon < 0)
        return;
    launched[launch_next].pid = pid;
    launched[launch_next].icon = icon;
    launch_next = (launch_next + 1) % LAUNCH_MAX;
}

/* The icon a program was started from, or -1 */
static int icon_of_pid(int pid)
{
    for (int i = 0; i < LAUNCH_MAX; i++)
        if (launched[i].pid == pid)
            return launched[i].icon;
    return -1;
}

static int icon_drag = -1;      /* the icon being dragged (-1 = none) */
static int icon_dx, icon_dy;    /* the grab: the cursor's offset within the icon */
static int icon_press_x, icon_press_y;   /* the press position (click vs drag) */

/* The file that says which icons there are: one line per icon,
 *
 *     program  napis  numer-obrazka
 *
 * and nothing else. Lines that do not parse are passed over, an empty
 * or missing file leaves the built-in seven - a desktop with no icons
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
        if (art >= (int)(sizeof(icon_art) / sizeof(icon_art[0])))
            art = 0;

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
        ic->x = 8;
        ic->y = 20 + count * 40;
        count++;
    }

    if (!count)
        return 0;                      /* nothing usable - keep the seven */
    icon_n = count;
    printk("[desktop] icons from the file '%s': %d\n", ICON_LIST_FILE, count);
    return 1;
}

/* Reads the icon positions from disk (if the file exists and its size
   matches the number of icons there are) */
static void icons_load(void)
{
    if (!fs_ready())
        return;
    int32_t buf[ICON_MAX * 2];
    int want = ICON_N * 2 * (int)sizeof(int32_t);
    if (fs_read(ICON_POS_FILE, buf, (uint32_t)want) < want)
        return;
    for (int k = 0; k < ICON_N; k++) {
        icons[k].x = buf[k * 2];
        icons[k].y = buf[k * 2 + 1];
    }
}

/* Writes the current icon positions to disk */
static void icons_save(void)
{
    if (!fs_ready())
        return;
    int32_t buf[ICON_MAX * 2];
    for (int k = 0; k < ICON_N; k++) {
        buf[k * 2] = icons[k].x;
        buf[k * 2 + 1] = icons[k].y;
    }
    fs_write(ICON_POS_FILE, buf, (uint32_t)(ICON_N * 2 * sizeof(int32_t)));
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
    /* Maximised: the window fills the desktop down to the taskbar. The
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
    struct wevent evq[WEV_QLEN];  /* the event queue for the application */
    int ev_head, ev_tail;
};

/* How long an application has to end after being asked. The desktop
   redraws about twenty times a second, so this is roughly two seconds -
   long enough for a program that is doing something else, short enough
   that a hung one does not hold the screen. */
#define CLOSE_FRAMES 40

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

static int drag_idx = -1;
static int resize_idx = -1;
/* A drag INSIDE a window's content: the slot whose application is
   being dragged in, or -1. Kept apart from drag_idx, which moves the
   window itself - here the window stays put and the application is
   told where the pointer goes. */
static int content_drag = -1;
static int drag_dx, drag_dy;
static int prev_pressed;    /* the previous left-button state - to detect a click */
/* Where the pointer was on the last pass. Drawing needs it for one
   thing only: the entry the cursor is over is drawn differently, which
   is what tells a list from a menu. */
static int mouse_x, mouse_y;
static int focus_idx = -1;  /* the window with keyboard focus */

/* The taskbar and the launch menu */
/* The taskbar.
 *
 * Eighteen pixels rather than twelve, because what is on it now is
 * pictures and not words: a twelve-pixel picture needs room around it,
 * and a bar the height of one line of text was only ever the height of
 * the text on it.
 *
 * How WIDE it is, and where the buttons sit on it, are settings - see
 * bar_x0() below. A bar that fills the screen is what a system has when
 * nobody has thought about it; half a screen, centred, is a dock. */
#define TB_H 18                       /* the taskbar height */
#define TB_ICON 12                    /* the picture on a button */
#define MENU_BW 40                    /* the width of the "Menu" button */
/* An entry is a picture and a name, so it is as tall as the picture
   with air around it - not as tall as one line of text. */
#define MENU_ROW 16                   /* the height of a menu entry */
#define MENU_W 110                    /* the width of the opened menu */
static int menu_open;
/* Where the menu hangs. Opened from the Menu button it stands over it;
   opened with the right button on the desktop it stands where the
   pointer is - the same list either way, because a second list would
   be a second thing to learn. */
static int menu_px, menu_py;
/* The installer is deliberately NOT here. It takes the whole screen
   and everything else has to be out of reach while it runs, so it is
   started as a kiosk ('install') and not as one more window among
   somebody's other windows. */
static const char *menu_items[] = {"guiapp", "gclock", "gfiles", "gweb",
                                  "gedit", "gtasks", "gmikser"};

/* --- The kiosk -----------------------------------------------------
 *
 * One program, the whole screen, and no way to anything else. This is
 * what an installer is: while a machine is being installed onto,
 * having a taskbar and a menu and somebody's other windows within
 * reach is not a convenience, it is a way to click something else by
 * mistake in the middle of writing to a disk.
 *
 * So in this mode the compositor draws no bar, no icons, no menu and
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
#define KIOSK_BTN_H 14
#define KIOSK_TRIES 3

static int kiosk_btn_x(void) { return video_w() - KIOSK_BTN_W - 6; }
static int kiosk_btn_y(void) { return 6; }
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
 * Which one is showing is drawn at the right-hand end of the top bar,
 * because that is where a thing that is always true belongs - a person
 * has to be able to see WHERE they are without pressing anything. */
#define DESK_N 4
static int cur_desk;

static int taskbar_y(void) { return video_h() - TB_H; }

/* How wide the bar is, from the settings: 100, 75 or 50 percent of the
   screen. Anything else is read as the whole width - a setting that
   cannot be understood is not a reason to draw nothing. */
static int bar_w(void)
{
    uint32_t p = config_get_num("taskbar", 100);
    if (p != 75 && p != 50)
        p = 100;
    int w = (int)((uint32_t)video_w() * p / 100);
    return w < 160 ? 160 : w;
}

/* Where it starts. A bar narrower than the screen is centred on it:
   that is what makes it a dock rather than a bar with a gap. */
static int bar_x0(void)
{
    return (video_w() - bar_w()) / 2;
}

static int menu_item_y(int i)
{
    return menu_py + i * MENU_ROW;
}

/* Puts the menu somewhere it fits: never off the right-hand edge,
   never over the taskbar. */
static void menu_at(int x, int y)
{
    int hgt = MENU_N * MENU_ROW;
    if (x + MENU_W > video_w())
        x = video_w() - MENU_W;
    if (x < 0)
        x = 0;
    if (y + hgt > taskbar_y())
        y = taskbar_y() - hgt;
    if (y < 0)
        y = 0;
    menu_px = x;
    menu_py = y;
    menu_open = 1;
}

/* The list of window buttons on the taskbar (after the Menu button).
   The buttons share out what is left of the bar, so a twelfth window
   makes them narrower instead of putting them past the right edge -
   where they would be drawn nowhere and clickable nowhere. */
/* The clock at the right-hand end of the bar: "12:34:56", eight
   characters of an 8-pixel font plus a little air on each side. It is
   taken out of what the window buttons have to share, because a clock
   drawn over a button would be a clock that hides one. */
#define TB_CLOCK_CH 8
#define TB_CLOCK_W (TB_CLOCK_CH * 8 + 6)
static int clock_x(void)
{
    return bar_x0() + bar_w() - TB_CLOCK_W + 3;
}

/* How much of the machine is in use.
 *
 * A number would say "4128 KiB", which is a thing to read; a bar
 * filled to a third is a thing to SEE. The exact figures - how much
 * memory, of how much, and how many tasks are behind it - are one
 * hover away, so the strip stays quiet until it is asked.
 *
 * It goes on the top bar, next to the desktop switch, because that
 * strip is where things that are ALWAYS true live: which desktop this
 * is, and how much of the machine is spoken for. The taskbar below is
 * for windows, and a meter there would be one more thing squeezing the
 * buttons. */
#define TB_LOAD_BAR 40

/* "Show the desktop": everything out of the way, and then everything
   back. The second click is the whole of the idea - a button that only
   hides windows is a button that costs you your work arrangement, and
   nobody presses it twice. So which windows were hidden BY IT is
   remembered, and only those come back; a window minimised by hand
   stays minimised, because that was somebody's decision. */
#define SHOW_W 12
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
    load_wait = 30;                    /* ~ once a second at 33 fps */

    uint32_t ram_kib = pmm_ram_kib();
    uint32_t used_kib = pmm_used_frames() * 4;
    if (used_kib > ram_kib)
        used_kib = ram_kib;
    load_percent = ram_kib ? (int)((used_kib * 100) / ram_kib) : 0;
    load_used_mib = (int)(used_kib / 1024);
    load_ram_mib = (int)(ram_kib / 1024);
    load_tasks = task_count();
}

#define TBL_W_MAX 22                  /* a picture and air on both sides */
#define TBL_W_MIN 14                  /* narrower than this loses the picture */

/* Where the first window button goes: after the Menu button, or
   wherever the setting says. 0 = from the left, 1 = in the middle of
   what is left, 2 = up against the clock. */
static int win_count(void);
static int tbl_width(void);

static int tbl_x0(void)
{
    int left = bar_x0() + 2 + MENU_BW + 4;
    int right = bar_x0() + bar_w() - TB_CLOCK_W - 2;
    int strip = win_count() * tbl_width();
    uint32_t how = config_get_num("icons", 0);

    if (how == 1) {                   /* centred between menu and clock */
        int x = left + (right - left - strip) / 2;
        return x < left ? left : x;
    }
    if (how == 2) {                   /* pushed up against the clock */
        int x = right - strip;
        return x < left ? left : x;
    }
    return left;                      /* from the left, as it always was */
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

/* The width of one button, the gap between them included. Everything
   that draws a button or looks for one under the cursor asks here, so
   a button is clickable exactly where it is drawn. */
static int tbl_width(void)
{
    int n = win_count();
    if (n < 1)
        n = 1;
    int space = bar_w() - (2 + MENU_BW + 4) - 2 - TB_CLOCK_W;
    int w = space / n;
    if (w > TBL_W_MAX)
        w = TBL_W_MAX;
    if (w < TBL_W_MIN)
        w = TBL_W_MIN;
    return w;
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

#define CLOSE_SZ 8          /* the bar buttons [_] [x] */
#define GRIP_SZ  8          /* the resize grip (bottom right corner) */
#define BTN_W 40
#define BTN_H 12
#define WIN_MIN_W 60
#define WIN_MIN_H 40

static int in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static int close_x(const struct window *w) { return w->x + w->w - CLOSE_SZ - 1; }
static int close_y(const struct window *w) { return w->y + 2; }
static int min_x(const struct window *w)   { return w->x + w->w - 2 * CLOSE_SZ - 2; }
static int max_x(const struct window *w)   { return w->x + w->w - 3 * CLOSE_SZ - 3; }
static int grip_x(const struct window *w)  { return w->x + w->w - GRIP_SZ; }
static int grip_y(const struct window *w)  { return w->y + w->h - GRIP_SZ; }
/* Every window may be resized. An application window costs more than a
   kernel one - its content buffer has to be made anew - so that is done
   once, when the drag ends, and not on every frame of it. */
static int resizable(const struct window *w) { (void)w; return 1; }
static int btn_x(const struct window *w)   { return w->x + 8; }
static int btn_y(const struct window *w)    { return w->y + TITLE_H + 8; }

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

/* Draws a decimal number as text */
static void draw_int(int x, int y, int v, uint8_t col)
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
    video_draw_string(x, y, s, col);
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

static void draw_window(const struct window *win)
{
    if (win->minimized)            /* minimised - visible only on the bar */
        return;

    /* A flat surface with one line around it. No shadow underneath and
       no lit edge: what tells a window from the desktop is that it is
       a different colour, and one line is enough to say where it
       ends. */
    video_fill_rect(win->x, win->y, win->w, win->h, COL_WIN);
    video_fill_rect(win->x, win->y, win->w, 1, COL_LINE);
    video_fill_rect(win->x, win->y + win->h - 1, win->w, 1, COL_LINE);
    video_fill_rect(win->x, win->y, 1, win->h, COL_LINE);
    video_fill_rect(win->x + win->w - 1, win->y, 1, win->h, COL_LINE);

    /* The title bar: the accent colour when the window is listening,
       plain grey when it is not. One flat colour either way - the
       difference is the whole message, so nothing else should be
       competing with it. */
    int focused = focus_idx >= 0 && win == &wins[focus_idx];
    video_fill_rect(win->x + 1, win->y + 1, win->w - 2, TITLE_H,
                    focused ? COL_TITLE : COL_TITLE_OFF);
    video_fill_rect(win->x + 1, win->y + 1 + TITLE_H, win->w - 2, 1,
                    COL_LINE);
    video_draw_string(win->x + 4, win->y + 2, win->title,
                      focused ? COL_WHITE : COL_DIM);
    /* The maximise button, and the minimise one next to the close.
       'O' means it will fill the screen, 'o' that it will go back to
       the size it had - the same button, and which of the two it is
       has to be visible without clicking it. */
    /* The buttons are the glyphs themselves - no boxes drawn round
       them. What they do is said by the sign; a frame adds nothing but
       ink. The one exception is the close button, which is red,
       because that one had better be unmistakable. */
    /* Not in the kiosk. There is nothing to minimise the window to,
       nowhere for it to grow into and nothing behind it to close it
       back to - three buttons that would all be lies. */
    if (!kiosk) {
        uint8_t sign = focused ? COL_WHITE : COL_DIM;
        video_draw_char(max_x(win), close_y(win), win->maxed ? 'o' : 'O',
                        sign);
        video_draw_char(min_x(win), close_y(win), '_', sign);
        video_fill_rect(close_x(win), close_y(win), CLOSE_SZ, CLOSE_SZ,
                        COL_RED);
        video_draw_char(close_x(win), close_y(win), 'x', COL_WHITE);
    }
    /* The grip: three plain lines running the way the corner is
       pulled. One colour, no shading - it is a hint, not a handle to
       be modelled. */
    if (!kiosk && resizable(win)) {
        int gx = grip_x(win), gy = grip_y(win);
        for (int d = 2; d < GRIP_SZ; d += 3)
            for (int k = 0; k <= GRIP_SZ - 2 - d; k++)
                video_putpixel(gx + d + k, gy + GRIP_SZ - 2 - k, COL_LINE);
    }

    /* An optional action button plus a click counter */
    if (win->has_button) {
        video_fill_rect(btn_x(win), btn_y(win), BTN_W, BTN_H, COL_WIN_HI);
        video_fill_rect(btn_x(win), btn_y(win), BTN_W, 1, COL_LINE);
        video_fill_rect(btn_x(win), btn_y(win) + BTN_H - 1, BTN_W, 1,
                        COL_LINE);
        video_fill_rect(btn_x(win), btn_y(win), 1, BTN_H, COL_LINE);
        video_fill_rect(btn_x(win) + BTN_W - 1, btn_y(win), 1, BTN_H,
                        COL_LINE);
        video_draw_string(btn_x(win) + 4, btn_y(win) + 2, "Click", COL_BLACK);
        /* The counter stands on the window's grey, not on the button -
           so it is dark, like everything else there. */
        draw_int(btn_x(win) + BTN_W + 6, btn_y(win) + 2, win->counter,
                 COL_BLACK);
    }

    /* An application window: compose its content buffer into the work
       area. A true-colour window goes a row at a time - its pixels are
       already what the framebuffer wants - and that is the faster of
       the two, not the slower. */
    if (win->content) {
        int bx = win->x + 1;
        int by = win->y + TITLE_H + 1;
        if (win->truecolour) {
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
    icon_drag = -1;
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

/* The switch drawn at the right-hand end of the top bar: one cell per
   desktop, the one being shown in the accent colour, and a desktop
   that holds nothing written in the quiet grey - so that "where am I"
   and "is there anything over there" are one glance, not two. */
#define DESK_CELL 12
static int desk_x0(void) { return video_w() - DESK_N * DESK_CELL - 3; }

static int show_x(void)
{
    return desk_x0() - TB_LOAD_BAR - 8 - 6 - SHOW_W;
}

static void toggle_show_desktop(void)
{
    if (hidden_by_show) {                 /* bring back what it hid */
        for (int i = 0; i < MAX_WIN; i++)
            if (hidden_by_show & (1u << i)) {
                if (wins[i].used)
                    wins[i].minimized = 0;
            }
        hidden_by_show = 0;
        return;
    }
    for (int i = 0; i < MAX_WIN; i++) {
        if (!here(i) || wins[i].minimized)
            continue;
        wins[i].minimized = 1;
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

/* What the window would become. The halves stop at the taskbar, like
   a maximised window does - the bar is not somewhere a window may go. */
static void snap_geom(int where, int *x, int *y, int *w, int *h)
{
    *x = 0;
    *y = 0;
    *w = video_w();
    *h = taskbar_y();
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
    if (my < sh / 4 && win->y <= TITLE_H + SNAP_EDGE)
        return SNAP_FULL;
    return SNAP_NONE;
}

/* Mouse handling: clicks (close, buttons) and window dragging.
   Clicks take precedence over dragging; both are counted on the
   left-button press edge, so that one click = one action. */
static int prev_right;      /* the previous right-button state */

/* Frames since the desktop started - the compositor's own clock, and
   the only one it needs: everything here happens in frames. */
static uint32_t frames;

/* Two clicks on the same title bar, close together in time, are one
   gesture: fill the screen, or give the window its size back. Every
   system does this, and a person who has used one tries it here within
   the first minute.
 *
 * "Close together" is twenty frames - about six tenths of a second at
 * the thirty-three this desktop draws. That is the value every system
 * settles near: shorter and a hand misses it on a machine that is
 * busy, longer and two separate clicks (which is how a window is
 * raised, then raised again) start counting as one gesture. */
#define DOUBLE_FRAMES 20
static uint32_t last_title_click;
static int last_title_win = -1;

static void handle_input(int mx, int my, int buttons)
{
    int pressed = buttons & 0x01;
    int right = buttons & 0x02;

    mouse_x = mx;
    mouse_y = my;

    /* In the kiosk there is nothing to click but the one window and
       the one button the environment itself offers. Everything the
       taskbar, the menu, the icons and the desktop switch would have
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
        if (window_at(mx, my) < 0 && my < taskbar_y())
            menu_at(mx, my);
        else
            menu_open = 0;
    }
    prev_right = right;

    if (pressed && !prev_pressed) {     /* a press edge = a click */
        /* The desktop switch on the top bar, which is above everything
           the way the bar itself is */
        if (my < 10 && mx >= show_x() && mx < show_x() + SHOW_W &&
            window_at(mx, my) < 0) {
            toggle_show_desktop();
            prev_pressed = pressed;
            return;
        }
        if (my < 10 && mx >= desk_x0() && window_at(mx, my) < 0) {
            int d = (mx - desk_x0()) / DESK_CELL;
            if (d >= 0 && d < DESK_N)
                desk_switch(d);
            prev_pressed = pressed;
            return;
        }
        /* The taskbar / launch menu takes precedence (it is on top) */
        if (in_rect(mx, my, bar_x0() + 2, taskbar_y() + 3, MENU_BW,
                    TB_H - 6)) {
            if (menu_open)
                menu_open = 0;
            else
                menu_at(bar_x0() + 2,
                        taskbar_y() - MENU_N * MENU_ROW);
            prev_pressed = pressed;
            return;
        }
        if (menu_open) {
            for (int i = 0; i < MENU_N; i++) {
                if (in_rect(mx, my, menu_px, menu_item_y(i), MENU_W,
                            MENU_ROW)) {
                    /* Started from the menu, which names programs -
                       so its face is looked up by name */
                    remember_launch(exec_spawn(menu_items[i]),
                                    icon_of_app(menu_items[i]));
                    menu_open = 0;
                    prev_pressed = pressed;
                    return;
                }
            }
            menu_open = 0;                /* a click outside the menu - close it */
        }
        /* Window buttons on the taskbar: focus plus bring to top. The
           same two functions the drawing asks, so a button is clickable
           exactly where it is - wherever the settings have put it. */
        if (my >= taskbar_y()) {
            int bw = tbl_width();
            int x0 = tbl_x0();
            int j = 0;
            for (int i = 0; i < MAX_WIN; i++) {
                if (!here(i))
                    continue;
                if (in_rect(mx, my, x0 + j * bw, taskbar_y(),
                            bw - 2, TB_H)) {
                    wins[i].minimized = 0;   /* restore it if minimised */
                    focus_idx = i;
                    raise_win(i);
                    prev_pressed = pressed;
                    return;
                }
                j++;
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
            /* the close button [x]
             *
             * A kernel window owns nothing, so it simply goes. An
             * application window belongs to a running process: dropping
             * it from under that process would leave it drawing into a
             * window that no longer exists. So we ASK - and only insist
             * when the asking goes unanswered. */
            if (in_rect(mx, my, close_x(w), close_y(w), CLOSE_SZ, CLOSE_SZ)) {
                close_window(i);
                hit = 1;
                break;
            }
            /* the maximise button [O] - and the same button puts it
               back, so one click undoes the other */
            if (in_rect(mx, my, max_x(w), close_y(w), CLOSE_SZ, CLOSE_SZ)) {
                toggle_max(i);
                hit = 1;
                break;
            }
            /* the minimise button [_] */
            if (in_rect(mx, my, min_x(w), close_y(w), CLOSE_SZ, CLOSE_SZ)) {
                w->minimized = 1;
                if (focus_idx == i)
                    focus_idx = -1;
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
                    frames - last_title_click <= DOUBLE_FRAMES) {
                    last_title_win = -1;      /* one gesture, once */
                    toggle_max(i);
                    hit = 1;
                    break;
                }
                last_title_win = i;
                last_title_click = frames;
                if (w->maxed) {
                    int along = w->w > 0 ? (mx - w->x) * w->rw / w->w : 0;
                    toggle_max(i);
                    w->x = mx - along;
                    w->y = my - 3;
                    if (w->x < 0)
                        w->x = 0;
                    if (w->x + w->w > video_w())
                        w->x = video_w() - w->w;
                    if (w->y < 0)
                        w->y = 0;
                }
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
        if (!hit) {
            /* A click on the desktop background: grabbing an icon (launch or
               move is decided on release), otherwise a loss of focus */
            focus_idx = -1;
            for (int k = 0; k < ICON_N; k++)
                if (in_rect(mx, my, icons[k].x, icons[k].y, ICON_W, ICON_H)) {
                    icon_drag = k;
                    icon_dx = mx - icons[k].x;
                    icon_dy = my - icons[k].y;
                    icon_press_x = mx;
                    icon_press_y = my;
                    break;
                }
        }
    }

    if (!pressed) {
        /* Let go against an edge: the window takes that edge's place.
           This has to happen before drag_idx is forgotten - it is the
           whole of what the drag was for. */
        if (drag_idx >= 0 && snap_hint != SNAP_NONE)
            snap_window(drag_idx, snap_hint);
        snap_hint = SNAP_NONE;
        drag_idx = -1;
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
        if (icon_drag >= 0) {       /* releasing an icon: a click or a move? */
            int ddx = mx - icon_press_x, ddy = my - icon_press_y;
            if (ddx * ddx + ddy * ddy < 9)
                /* barely moved -> a click, and the window it opens
                   will wear the picture from this very tile */
                remember_launch(exec_spawn(icons[icon_drag].app), icon_drag);
            else
                icons_save();                       /* moved -> save the layout */
            icon_drag = -1;
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

    if (pressed && icon_drag >= 0) {    /* the icon follows the cursor */
        int nx = mx - icon_dx;
        int ny = my - icon_dy;
        if (nx < 0) nx = 0;
        if (ny < 10) ny = 10;                       /* below the top bar */
        if (nx > video_w() - ICON_W) nx = video_w() - ICON_W;
        if (ny > video_h() - TB_H - ICON_H) ny = video_h() - TB_H - ICON_H;
        icons[icon_drag].x = nx;
        icons[icon_drag].y = ny;
    }

    if (pressed && drag_idx >= 0) {     /* in progress: the window follows the cursor */
        int nx = mx - drag_dx;
        int ny = my - drag_dy;
        if (nx < 0) nx = 0;
        if (ny < TITLE_H) ny = TITLE_H;
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
 * Maximised means: from the top left corner of the screen down to the
 * taskbar - not over it. The bar is how a person reaches every other
 * window, so a window that covered it would be a window there was no
 * way out of.
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
        w->y = 0;
        w->w = video_w();
        w->h = taskbar_y();
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

/* Draws the taskbar at the bottom and (if open) the launch menu */
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

/* The background: sixteen bands, darkening towards the floor.
 *
 * A flat colour is what a machine draws when nobody has thought about
 * it. Sixteen rectangles cost sixteen palette entries and one loop,
 * and the screen stops looking like a fill. */
static void draw_background(void)
{
    int h = taskbar_y();

    /* A picture, if there is one: every pixel of it covers a block of
       the screen, so this is one multiply per row and one per column
       rather than a scaling filter nobody would see the benefit of. */
    if (wall) {
        int sw = video_w();
        for (int y = 0; y < h; y++) {
            int sy = y * wall_h / video_h();
            if (wall_rgb) {
                const uint8_t *row = wall + (long)sy * wall_w * 3;
                for (int x = 0; x < sw; x++) {
                    const uint8_t *p = row + (x * wall_w / sw) * 3;
                    video_putpixel_rgb(x, y, ((uint32_t)p[0] << 16) |
                                             ((uint32_t)p[1] << 8) | p[2]);
                }
                continue;
            }
            const uint8_t *row = wall + sy * wall_w;
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
    /* Under the bar there is nothing to see, but the bar is drawn last
       and this keeps the frame whole if it is ever moved */
    video_fill_rect(0, h, video_w(), video_h() - h,
                    (uint8_t)(COL_DESK0 + COL_DESK_N - 1));
}

/* Draws the icons on the desktop background (a tile plus a caption) */
/* One icon's picture, in whatever colour the place it is drawn in
   calls for - white on a tile, white or black on a bar button */
static void draw_icon_art_col(int k, int x, int y, uint8_t col)
{
    if (k < 0 || k >= (int)(sizeof(icon_art) / sizeof(icon_art[0])))
        return;
    for (int row = 0; row < 12; row++)
        for (int c = 0; c < 12; c++)
            if (icon_art[k][row] & (0x800 >> c))
                video_putpixel(x + c, y + row, col);
}

/* One icon's picture, drawn where the tile is */
static void draw_icon_art(int k, int x, int y)
{
    /* White on the tile's colour: one flat mark, nothing behind it and
       nothing under it */
    draw_icon_art_col(k, x, y, COL_WHITE);
}

static void draw_icons(void)
{
    for (int k = 0; k < ICON_N; k++) {
        const struct icon *ic = &icons[k];
        /* The tile: one flat colour with the picture on it. No
           shadow, no edge - a shape in the accent colour is already
           different enough from a desktop to be a thing. */
        video_fill_rect(ic->x, ic->y, ICON_W, ICON_H, COL_ICON);
        draw_icon_art(ic->art, ic->x + (ICON_W - 12) / 2,
                      ic->y + (ICON_H - 12) / 2);
        /* The caption gets a shadow of its own: white on a light
           background is not text, it is a rumour */
        video_draw_string(ic->x + 1, ic->y + ICON_H + 3, ic->label,
                          COL_BLACK);
        video_draw_string(ic->x, ic->y + ICON_H + 2, ic->label, COL_WHITE);
    }
}

/* The time on the bar.
 *
 * Reading the CMOS clock means waiting out an update in progress and
 * then reading it twice until two reads agree, so it is not something
 * to do thirty times a second for a digit that changes once a second.
 * The reading is kept and refreshed about five times a second, which is
 * often enough that no second is ever skipped on screen. */
/* A small unsigned number into a buffer; returns how many characters
   it wrote. The hint on the bar is the only place that needs one. */
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

static char clock_text[TB_CLOCK_CH + 1] = "--:--:--";
static int clock_wait;

static void clock_refresh(void)
{
    if (--clock_wait > 0)
        return;
    clock_wait = 6;

    struct rtc_time t;
    rtc_read(&t);
    clock_text[0] = (char)('0' + (t.hour / 10) % 10);
    clock_text[1] = (char)('0' + t.hour % 10);
    clock_text[2] = ':';
    clock_text[3] = (char)('0' + (t.min / 10) % 10);
    clock_text[4] = (char)('0' + t.min % 10);
    clock_text[5] = ':';
    clock_text[6] = (char)('0' + (t.sec / 10) % 10);
    clock_text[7] = (char)('0' + t.sec % 10);
    clock_text[8] = 0;
}

/* The name of the window under the pointer, shown just above the bar.
 *
 * Remembered rather than drawn on the spot: it belongs on top of
 * everything, and the buttons are drawn before the menu that may open
 * over them. */
static int hint_x, hint_y;
static const char *hint_text;
/* What was just saved, and for how many frames to keep saying it: a
   hint lives one frame, and a message about a file wants a moment. */
static const char *shot_note;
static int shot_frames;

static void hint_for(int x, const char *text)
{
    hint_x = x;
    hint_y = taskbar_y() - 15;        /* just above the bar it belongs to */
    hint_text = text;
}

/* The same hint, for something that is not on the taskbar: the top
   strip has its own things to explain, and a hint that appears at the
   other end of the screen from what it explains is a riddle. */
static void hint_at(int x, int y, const char *text)
{
    hint_x = x;
    hint_y = y;
    hint_text = text;
}

static void draw_hint(void)
{
    if (!hint_text || !hint_text[0])
        return;
    int n = 0;
    while (hint_text[n])
        n++;
    int w = n * 8 + 8;
    int x = hint_x;
    if (x + w > video_w())
        x = video_w() - w;
    if (x < 0)
        x = 0;
    int y = hint_y;
    if (y + 13 > taskbar_y())
        y = taskbar_y() - 15;

    video_fill_rect(x, y, w, 13, COL_WIN);
    video_fill_rect(x, y, w, 1, COL_LINE);
    video_fill_rect(x, y + 12, w, 1, COL_LINE);
    video_fill_rect(x, y, 1, 13, COL_LINE);
    video_fill_rect(x + w - 1, y, 1, 13, COL_LINE);
    video_draw_string(x + 4, y + 3, hint_text, COL_BLACK);
    hint_text = (const char *)0;      /* one frame, one hint */
}

static void draw_taskbar(void)
{
    int ty = taskbar_y();
    int bx0 = bar_x0(), bwid = bar_w();
    video_fill_rect(bx0, ty, bwid, TB_H, COL_BAR);
    video_fill_rect(bx0, ty, bwid, 1, COL_LINE);   /* where the bar starts */
    /* The Menu button: the accent colour, flat */
    video_fill_rect(bx0 + 2, ty + 3, MENU_BW, TB_H - 6, COL_TITLE);
    video_draw_string(bx0 + 6, ty + 5, "Menu", COL_WHITE);

    /* The open windows - one button each, carrying the picture of the
       program the window belongs to.
     *
     * A name trimmed to six letters says 'Ustawi'; a picture says what
     * it is at a glance and takes a third of the room. Which is what a
     * taskbar is for - to be read without being studied. */
    int bw = tbl_width();
    int x0 = tbl_x0();
    int j = 0;
    for (int i = 0; i < MAX_WIN; i++) {
        if (!here(i))
            continue;
        int bx = x0 + j * bw;
        if (bx + bw - 2 > bx0 + bwid)
            break;                    /* the bar has run out - draw no further */
        video_fill_rect(bx, ty + 2, bw - 2, TB_H - 4,
                        i == focus_idx ? COL_TITLE : COL_TBBTN_OFF);
        /* The window's own picture, or - for the kernel's own windows
           and anything started from a shell - the plain window mark */
        int from = wins[i].owner >= 0 ? icon_of_pid(wins[i].owner) : -1;
        int art = from >= 0 ? icons[from].art : 0;
        draw_icon_art_col(art, bx + (bw - 2 - TB_ICON) / 2,
                          ty + (TB_H - TB_ICON) / 2,
                          i == focus_idx ? COL_WHITE : COL_BLACK);
        /* A picture says WHICH program; it does not say which of two
           terminals. The name comes back when the pointer asks for it,
           which is the one moment it is wanted. */
        if (in_rect(mouse_x, mouse_y, bx, ty, bw - 2, TB_H))
            hint_for(bx, wins[i].title);
        j++;
    }

    /* The clock: no field, no frame - just the time, at the end of the
       bar where nothing else is */
    clock_refresh();
    load_refresh();
    video_draw_string(clock_x(), ty + (TB_H - 8) / 2, clock_text, COL_BLACK);
    if (menu_open) {
        /* One flat panel with a line round it. Every entry is the
           program's own picture and its name - the same picture as on
           its icon and on its button, because a program that looks
           like three different things is three things to remember. */
        int top = menu_item_y(0);
        int hgt = MENU_N * MENU_ROW;
        int mx0 = menu_px;
        video_fill_rect(mx0, top, MENU_W, hgt, COL_WIN);
        video_fill_rect(mx0, top, MENU_W, 1, COL_LINE);
        video_fill_rect(mx0, top + hgt - 1, MENU_W, 1, COL_LINE);
        video_fill_rect(mx0, top, 1, hgt, COL_LINE);
        video_fill_rect(mx0 + MENU_W - 1, top, 1, hgt, COL_LINE);
        for (int i = 0; i < MENU_N; i++) {
            int iy = menu_item_y(i);
            int from = icon_of_app(menu_items[i]);
            int art = from >= 0 ? icons[from].art : 0;
            /* The entry under the pointer wears the accent colour.
               Without it a menu is a list of words that happens to be
               clickable; with it, it answers. */
            int over = in_rect(mouse_x, mouse_y, mx0, iy, MENU_W, MENU_ROW);
            if (over)
                video_fill_rect(mx0 + 1, iy, MENU_W - 2, MENU_ROW,
                                COL_TITLE);
            else if (i)
                video_fill_rect(mx0 + 1, iy, MENU_W - 2, 1, COL_LINE);
            draw_icon_art_col(art, mx0 + 4, iy + 2,
                              over ? COL_WHITE : COL_BLACK);
            /* The name a person knows it by, which is the one written
               under its icon - the program's own name is for the
               shell */
            const char *label = from >= 0 ? icons[from].label
                                          : menu_items[i];
            video_draw_string(mx0 + 20, iy + 4, label,
                              over ? COL_WHITE : COL_BLACK);
        }
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
 * person can see on the taskbar, so pressing it twice takes you where
 * looking at the bar says it will.
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

/* Asks the window to close, exactly as its [x] does: an application
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
    shot_frames = 60;             /* about two seconds */
}

static void desktop_on_key(char c)
{
    if (c == KEY_SHOT) {
        save_screenshot();
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
                         30 + k * 12 + w->h > taskbar_y()))
            k -= 3;
        if (k < 0)
            k = 0;
        w->x = 70 + k * 12;
        w->y = 30 + k * 12;
        /* Bigger than the screen even at the start of the cascade - all
           that is left is to push it against the edge */
        if (w->x + w->w > video_w())
            w->x = video_w() > w->w ? video_w() - w->w : 0;
        if (w->y + w->h > taskbar_y())
            w->y = taskbar_y() > w->h ? taskbar_y() - w->h : 0;
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
        return i;
    }
    return -1;
}

/* A window's name, changed by the program that owns it.
 *
 * A window called "Editor" says what the program is; a window called
 * "Editor: a note" says which of the three open ones you are looking
 * for - and that is what a title bar is FOR. The name is also what the
 * taskbar shows under the pointer, so one call fixes both at once. */
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

    video_draw_string(8, 8, "ThreadOS - installation", COL_WHITE);

    int bx = kiosk_btn_x(), by = kiosk_btn_y();
    int over = in_rect(mouse_x, mouse_y, bx, by, KIOSK_BTN_W, KIOSK_BTN_H);
    video_fill_rect(bx, by, KIOSK_BTN_W, KIOSK_BTN_H,
                    over ? COL_TITLE : COL_TBBTN);
    video_fill_rect(bx, by, KIOSK_BTN_W, 1, COL_LINE);
    video_fill_rect(bx, by + KIOSK_BTN_H - 1, KIOSK_BTN_W, 1, COL_LINE);
    video_draw_string(bx + 8, by + 3, "Restart", COL_WHITE);

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
        int cx = video_w() / 2 - 22 * 8 / 2;
        video_draw_string(cx, video_h() / 2 - 4,
                          "The installer will not start", COL_WHITE);
        video_draw_string(cx, video_h() / 2 + 10,
                          "Restart the machine", COL_WHITE);
    }

    draw_hint();
    draw_cursor(mx, my);
    video_present();
    frames++;
    task_sleep(30);
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
    /* The desktop's background comes from the settings, so it survives
       a restart. A small set of choices rather than a free colour: a
       number that means one of six things can be checked, and 'a red
       component of 41' cannot. */
    static const struct { uint8_t r, g, b; } backgrounds[] = {
        {   0, 130, 162 },   /* 0 - teal, as it always was */
        {   0,   0, 130 },   /* 1 - navy */
        {  81,   0, 121 },   /* 2 - purple */
        {   0, 113,   0 },   /* 3 - green */
        { 162,  81,   0 },   /* 4 - brown */
        {  48,  48,  48 },   /* 5 - grey */
    };
    uint32_t bg = config_get_num("background", 0);
    if (bg >= sizeof(backgrounds) / sizeof(backgrounds[0]))
        bg = 0;
    video_set_palette(COL_DESK, backgrounds[bg].r, backgrounds[bg].g,
                      backgrounds[bg].b);
    video_set_palette(COL_BAR, 211, 211, 211);   /* the light grey bars */
    video_set_palette(COL_WIN, 227, 227, 227);   /* the grey window background */
    video_set_palette(COL_TBBTN, 105, 105, 105); /* the dark grey bar button */
    video_set_palette(COL_ICON, 255, 162, 0);   /* the orange icon */

    /* Flat tones. One line colour, one lighter surface, one dim text -
       and the accent blue, which is the only thing that shouts. */
    video_set_palette(COL_LINE, 150, 150, 150);       /* surface boundaries */
    video_set_palette(COL_WIN_HI, 247, 247, 247);     /* a lighter field */
    video_set_palette(COL_TITLE_OFF, 178, 178, 178);  /* no focus - plain grey */
    video_set_palette(COL_TBBTN_OFF, 146, 146, 146);  /* a quiet button */
    video_set_palette(COL_DIM, 73, 73, 73);        /* text without weight */
    video_set_palette(COL_ICON_LO, 186, 113, 0);

    /* The background: from a little brighter at the top to a little
       darker at the floor. A narrow range on purpose - a machine set to
       teal still has to look teal from anywhere on the screen, not just
       in the middle. */
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


    icons_read_list();         /* which icons there are, if the disk says */
    icons_load();              /* restore the remembered icon positions from disk */
    wallpaper_load();          /* and the wallpaper, if the settings name one */

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
        reap_windows();

        int mx, my, buttons;
        mouse_get(&mx, &my, &buttons);
        handle_input(mx, my, buttons);

        draw_background();

        if (kiosk) {
            kiosk_frame(mx, my);
            continue;
        }

        /* The top bar with the system title, lit along its top edge and
           shaded along the bottom - the same light as everything else */
        video_fill_rect(0, 0, video_w(), 10, COL_BAR);
        video_fill_rect(0, 9, video_w(), 1, COL_LINE);
        video_draw_string(2, 1, "ThreadOS", COL_BLACK);

        /* Everything out of the way, and everything back: a screen
           with a window on it, drawn small. */
        int sx = show_x();
        video_fill_rect(sx, 1, SHOW_W, 8, hidden_by_show ? COL_TITLE
                                                         : COL_TBBTN_OFF);
        video_fill_rect(sx, 1, SHOW_W, 1, COL_LINE);
        video_fill_rect(sx, 8, SHOW_W, 1, COL_LINE);
        video_fill_rect(sx, 1, 1, 8, COL_LINE);
        video_fill_rect(sx + SHOW_W - 1, 1, 1, 8, COL_LINE);
        video_fill_rect(sx + 3, 3, SHOW_W - 6, 4,
                        hidden_by_show ? COL_WHITE : COL_LINE);
        if (in_rect(mouse_x, mouse_y, sx, 0, SHOW_W, 10)) {
            const char *what = hidden_by_show ? "Restore windows"
                                              : "Show desktop";
            int n = 0;
            while (what[n])
                n++;
            hint_at(sx - (n * 8 + 8) - 4, 12, what);
        }

        /* How much of the machine is in use, next to which desktop
           this is: both are things that are always true. */
        load_refresh();
        int lx = desk_x0() - TB_LOAD_BAR - 8, ly = 1;
        video_fill_rect(lx, ly, TB_LOAD_BAR, 8, COL_TBBTN_OFF);
        video_fill_rect(lx, ly, TB_LOAD_BAR, 1, COL_LINE);
        video_fill_rect(lx, ly + 7, TB_LOAD_BAR, 1, COL_LINE);
        video_fill_rect(lx, ly, 1, 8, COL_LINE);
        video_fill_rect(lx + TB_LOAD_BAR - 1, ly, 1, 8, COL_LINE);
        /* The filling goes INSIDE the frame - drawn over it, a reading
           of a few percent is one column wide and the frame covers
           exactly that column, so the meter would show nothing until
           the machine was half full. */
        int inner = TB_LOAD_BAR - 2;
        int fill = load_percent * inner / 100;
        if (fill > inner)
            fill = inner;
        if (fill < 1 && load_percent > 0)
            fill = 1;                 /* something in use looks like it */
        video_fill_rect(lx + 1, ly + 1, fill, 6, COL_TITLE);

        /* Asked about, it says everything: how much of how much, and
           how many tasks are behind it. */
        if (in_rect(mouse_x, mouse_y, lx, 0, TB_LOAD_BAR, 10)) {
            static char text[40];
            int n = 0;
            n += put_num(text + n, load_percent);
            text[n++] = '%';
            text[n++] = ' ';
            text[n++] = '(';
            n += put_num(text + n, load_used_mib);
            text[n++] = '/';
            n += put_num(text + n, load_ram_mib);
            text[n++] = ' ';
            text[n++] = 'M';
            text[n++] = 'i';
            text[n++] = 'B';
            text[n++] = ')';
            text[n++] = ',';
            text[n++] = ' ';
            n += put_num(text + n, load_tasks);
            text[n++] = ' ';
            text[n++] = 'z';
            text[n++] = 'a';
            text[n++] = 'd';
            text[n++] = '.';
            text[n] = 0;
            /* To the LEFT of the meter, ending where the meter
               begins: put under the pointer, the box would be read
               with an arrow drawn across the middle of it. */
            hint_at(lx - (n * 8 + 8) - 4, 12, text);
        }

        for (int d = 0; d < DESK_N; d++) {
            int dx = desk_x0() + d * DESK_CELL;
            int busy = 0;
            for (int i = 0; i < MAX_WIN; i++)
                if (wins[i].used && wins[i].desk == d && wins[i].content)
                    busy = 1;
            if (d == cur_desk)
                video_fill_rect(dx, 1, DESK_CELL - 1, 8, COL_TITLE);
            char n[2] = {(char)('1' + d), 0};
            video_draw_string(dx + 2, 1, n,
                              d == cur_desk ? COL_WHITE
                                            : busy ? COL_BLACK : COL_DIM);
        }

        draw_icons();                         /* icons on the background (below the windows) */

        int ord[MAX_WIN];
        int n = zorder(ord);
        for (int k = 0; k < n; k++)      /* from the bottom up (Z order) */
            draw_window(&wins[ord[k]]);

        /* The place a dragged window would take, drawn as an outline
           in the accent colour: the same colour that everywhere else
           means "this is the one". */
        if (snap_hint != SNAP_NONE) {
            int hx, hy, hw, hh;
            snap_geom(snap_hint, &hx, &hy, &hw, &hh);
            video_fill_rect(hx, hy, hw, 2, COL_TITLE);
            video_fill_rect(hx, hy + hh - 2, hw, 2, COL_TITLE);
            video_fill_rect(hx, hy, 2, hh, COL_TITLE);
            video_fill_rect(hx + hw - 2, hy, 2, hh, COL_TITLE);
        }

        draw_taskbar();
        /* What was just saved stays on the screen for a moment - the
           hint itself lives one frame, so it is asked for again until
           the moment is up. */
        if (shot_frames > 0) {
            shot_frames--;
            if (shot_note)
                hint_at(4, 12, shot_note);
        }
        draw_hint();               /* above everything, including the menu */
        draw_cursor(mx, my);

        video_present();                      /* the finished frame -> the screen */
        frames++;
        task_sleep(30);                       /* ~33 frames/s */
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
