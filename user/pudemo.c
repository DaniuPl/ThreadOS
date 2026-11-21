/* pudemo: every control of ProtonUI v2 in one window
 *
 * docs/protonui-plan.md stage 4: the buttons of the four kinds, two
 * switches, segments, knots, two boxes, a field, a list, a slider and
 * the two kinds of progress - put together with pu_start, so that Tab
 * moves the keys between them and the one with the keys wears the
 * thread round it. What happens is said on the serial port:
 *
 *   [pudemo] drawn
 *   [pudemo] pressed NAME
 *   [pudemo] changed NAME VALUE
 *   [pudemo] focus NAME
 *   [pudemo] typed NAME TEXT
 *
 * t_protonui reads the controls off the screen and drives them. The
 * places are plain numbers, which the test reads from this file.
 */

#include "uapi.h"
#include "ugui.h"

#define W 420
#define H 330
#define BTN_Y 16
#define BTN_H 34
#define B0_X 16
#define B0_W 90
#define B1_X 114
#define B1_W 90
#define B2_X 212
#define B2_W 84
#define B3_X 304
#define B3_W 90
#define SW_X 16
#define SW_Y 66
#define SW_W 200
#define SW2_Y 96
#define SEG_X 232
#define SEG_Y 66
#define SEG_W 172
#define SEG_H 34
#define KN_X 232
#define KN_Y 110
#define KN_W 172
#define CK_X 16
#define CK_Y 130
#define CK2_Y 154
#define FLD_X 16
#define FLD_Y 186
#define FLD_W 200
#define SL_X 16
#define SL_Y 222
#define SL_W 200
#define SL_H 20
#define LST_X 232
#define LST_Y 192
#define LST_W 172
#define LST_H 72
#define PR_X 16
#define PR_Y 280
#define PR_W 388
#define PR_VALUE 62
#define PU_Y 300
#define PU_PHASE 340

static struct ug u;
static struct ug_item room[32];
static char name_buf[32] = "threados-pc";

static const char *const views[3] = { "List", "Icons", "Tree" };
static const char *const sizes[3] = { "Small loom", "Medium loom",
                                      "Large loom" };
static const char *const rows[5] = { "kernel", "docs", "AGENTS.md",
                                     "Makefile", "README.md" };

static const char *view_of(void *ctx, int i) { (void)ctx; return views[i]; }
static const char *size_of(void *ctx, int i) { (void)ctx; return sizes[i]; }
static const char *row_of(void *ctx, int i) { (void)ctx; return rows[i]; }

static void build(void)
{
    pu_start(&u, u.win, W, H, room, 32);
    int b0 = pu_button(&u, B0_X, BTN_Y, B0_W, BTN_H, "Install");
    pu_kind(&u, b0, PU_PRIMARY);
    int b1 = pu_button(&u, B1_X, BTN_Y, B1_W, BTN_H, "Cancel");
    pu_kind(&u, b1, PU_SECONDARY);
    int b2 = pu_button(&u, B2_X, BTN_Y, B2_W, BTN_H, "Details");
    pu_kind(&u, b2, PU_GHOST);
    int b3 = pu_button(&u, B3_X, BTN_Y, B3_W, BTN_H, "Delete");
    pu_kind(&u, b3, PU_DANGER);
    pu_switch(&u, SW_X, SW_Y, SW_W, "Bluetooth", 1);
    pu_switch(&u, SW_X, SW2_Y, SW_W, "Dark theme at night", 0);
    int seg = pu_segments(&u, SEG_X, SEG_Y, SEG_W, SEG_H, view_of, 0, 3, 0);
    pu_name(&u, seg, "View");
    int kn = pu_knots(&u, KN_X, KN_Y, KN_W, size_of, 0, 3, 1);
    pu_name(&u, kn, "Loom size");
    pu_check(&u, CK_X, CK_Y, "Start at login", 1);
    pu_check(&u, CK_X, CK2_Y, "Send reports", 0);
    int f = pu_field(&u, FLD_X, FLD_Y, FLD_W, name_buf, sizeof(name_buf));
    pu_name(&u, f, "Computer name");
    int sl = pu_slider(&u, SL_X, SL_Y, SL_W, SL_H, 40, 100);
    pu_name(&u, sl, "Volume");
    int ls = pu_list(&u, LST_X, LST_Y, LST_W, LST_H, row_of, 0, 5);
    pu_name(&u, ls, "Files");
    pu_set_value(&u, ls, 1);
    int p0 = pu_progress(&u, PR_X, PR_Y, PR_W, PR_VALUE, 100);
    pu_name(&u, p0, "Installing");
    int p1 = pu_progress(&u, PR_X, PU_Y, PR_W, -1, 100);
    pu_name(&u, p1, "Downloading");
    pu_set_phase(&u, p1, PU_PHASE);
}

static void say_num(int v)
{
    char d[12];
    int n = 0;
    if (v < 0) {
        print("-");
        v = -v;
    }
    do {
        d[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    char out[12];
    for (int i = 0; i < n; i++)
        out[i] = d[n - 1 - i];
    out[n] = 0;
    print(out);
}

static void said(int what, int id)
{
    print(what == UG_PRESSED ? "[pudemo] pressed "
          : what == UG_CHANGED ? "[pudemo] changed "
          : what == UG_FOCUSED ? "[pudemo] focus " : "[pudemo] typed ");
    print(pu_name_of(&u, id));
    if (what == UG_CHANGED) {
        print(" ");
        say_num(pu_value(&u, id));
    } else if (what == UG_TYPED) {
        print(" ");
        print(name_buf);
    }
    print("\n");
}

void _start(void)
{
    u.win = (int)win_create(W, H, "Controls");
    if (u.win < 0)
        sys_exit(1);
    build();
    ug_draw(&u);
    print("[pudemo] drawn\n");
    for (;;) {
        struct win_event ev;
        int redraw = 0;
        while (win_poll(u.win, &ev) == 1) {
            if (ev.type == WEV_LOOK) {
                redraw = 1;
                continue;
            }
            int which;
            int what = ug_event(&u, &ev, &which);
            if (what == UG_CLOSED)
                sys_exit(0);
            if (what == UG_PRESSED || what == UG_CHANGED ||
                what == UG_FOCUSED || what == UG_TYPED)
                said(what, which);
            redraw = 1;
        }
        if (redraw)
            ug_draw(&u);
        sys_sleep(50);
    }
}
