/* Account manager: account records and their private home directories
   are changed through the kernel's account service. */

#include "udraw.h"
#include "ugui.h"
#include "ulocale.h"

#define W 520
#define H 340
#define MAX_USERS 24

struct account_row {
    char name[16];
    int uid, level;
};

static struct account_row rows[MAX_USERS];
static int nrows, selected, focus, make_admin, top;
static int win, cw = W, ch = H;
static struct ud_look look;
static char name[16];
static char password[USER_PASSWORD_MAX + 1];
static char confirm[USER_PASSWORD_MAX + 1];
static char admin_password[USER_PASSWORD_MAX + 1];
static char status[72];

static int length(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int rows_visible(void)
{
    int n = (ch - 154) / 22;
    if (n < 1) n = 1;
    if (n > 10) n = 10;
    return n;
}

static void copy(char *d, const char *s, int cap)
{
    int i = 0;
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static void erase(char *s, int cap)
{
    for (int i = 0; i < cap; i++) s[i] = 0;
}

static void load(void)
{
    nrows = 0;
    while (nrows < MAX_USERS) {
        struct user_ctl c = { 0 };
        c.op = USER_CTL_LIST;
        c.index = nrows;
        if (sys_user(&c) < 0) break;
        copy(rows[nrows].name, c.name, sizeof(rows[nrows].name));
        rows[nrows].uid = c.uid;
        rows[nrows].level = c.level;
        nrows++;
    }
    if (selected >= nrows) selected = nrows ? nrows - 1 : -1;
    if (selected >= 0 && selected < top) top = selected;
    if (nrows <= rows_visible()) top = 0;
    else if (top > nrows - rows_visible()) top = nrows - rows_visible();
}

static void say(int x, int y, const char *s, uint32_t color)
{
    ud_text(win, x, ud_baseline(y, 20, UD_PX), s, color, UD_SANS, UD_PX);
}

static void say_l(int x, int y, const char *s, uint32_t color)
{
    ud_text_l(win, x, ud_baseline(y, 20, UD_PX), s, color, UD_SANS, UD_PX);
}

static void field(int x, int y, int w, const char *label, const char *value,
                  int selected_field, int secret)
{
    say_l(x, y, label, look.dim);
    ud_round(win, x, y + 20, w, 28, 5, look.field);
    ud_ring(win, x, y + 20, w, 28, 5,
            focus == selected_field ? look.accent : look.line);
    if (secret) {
        char stars[32];
        int n = length(value);
        if (n > (int)sizeof(stars) - 1) n = sizeof(stars) - 1;
        for (int i = 0; i < n; i++) stars[i] = '*';
        stars[n] = 0;
        say(x + 7, y + 24, stars, look.text);
    } else {
        say(x + 7, y + 24, value, look.text);
    }
}

static void button(int x, int y, int w, const char *label, int danger)
{
    uint32_t color = danger ? look.danger : look.accent;
    ud_round(win, x, y, w, 28, 5, color);
    ud_text_mid_l(win, x, y, w, 28, label, look.on_accent, UD_SANS, UD_PX);
}

static void draw(void)
{
    ud_look(&look);
    ulocale_window_title(win, "User Accounts", "Konta użytkowników");
    ud_fill(win, 0, 0, cw, ch, look.bg);
    say_l(16, 12, "User Accounts", look.text);
    say_l(16, 38, "Accounts and home folders are managed by the system",
          look.dim);
    ud_fill(win, 16, 62, cw - 32, 1, look.line);

    say_l(18, 72, "Accounts", look.dim);
    int visible = rows_visible();
    for (int i = 0; i < visible && top + i < nrows; i++) {
        int row = top + i;
        int y = 96 + i * 22;
        if (row == selected)
            ud_round(win, 16, y - 2, 210, 21, 4, look.row);
        char role[24];
        copy(role, rows[row].level == 0
                       ? (ulocale_language_polish() ? "administrator" : "admin")
                       : (ulocale_language_polish() ? "użytkownik" : "user"),
             sizeof(role));
        ud_text(win, 24, ud_baseline(y, 18, UD_PX), rows[row].name,
                look.text, UD_SANS, UD_PX);
        ud_text_right(win, 215, ud_baseline(y, 18, UD_PX), role,
                      look.dim, UD_SANS, UD_PX);
    }

    int x = 246;
    field(x, 72, cw - x - 18, "Account name", name, 0, 0);
    field(x, 124, cw - x - 18, "New password", password, 1, 1);
    field(x, 176, cw - x - 18, "Confirm password", confirm, 2, 1);
    field(x, 228, cw - x - 18, "Admin or current password",
          admin_password, 3, 1);
    ud_round(win, x, 286, 16, 16, 3, make_admin ? look.accent : look.field);
    ud_ring(win, x, 286, 16, 16, 3, look.line);
    if (make_admin)
        ud_text(win, x + 3, ud_baseline(285, 18, UD_PX), "x",
                look.on_accent, UD_BOLD, UD_PX);
    say_l(x + 24, 284, "Create as administrator", look.text);

    button(16, ch - 42, 112, "Add account", 0);
    button(138, ch - 42, 112, "Set password", 0);
    button(260, ch - 42, 112, "Change role", 0);
    button(382, ch - 42, 112, "Remove", 1);
    if (status[0]) say_l(16, ch - 70, status, look.dim);
}

static void submit(int op)
{
    struct user_ctl c = { 0 };
    c.op = op;
    if (op == USER_CTL_ADD) {
        if (!name[0] || !password[0] || !confirm[0] ||
            length(password) != length(confirm)) {
            copy(status, "Enter a name and matching passwords", sizeof(status));
            return;
        }
        for (int i = 0; password[i]; i++)
            if (password[i] != confirm[i]) {
                copy(status, "Passwords do not match", sizeof(status));
                return;
            }
        copy(c.name, name, sizeof(c.name));
        copy(c.password, password, sizeof(c.password));
        copy(c.admin_password, admin_password, sizeof(c.admin_password));
        c.level = make_admin ? 0 : 1;
    } else {
        if (selected < 0 || selected >= nrows) {
            copy(status, "Select an account first", sizeof(status));
            return;
        }
        copy(c.name, rows[selected].name, sizeof(c.name));
        copy(c.admin_password, admin_password, sizeof(c.admin_password));
        if (op == USER_CTL_PASSWORD) {
            if (!password[0] || length(password) != length(confirm)) {
                copy(status, "Enter matching new passwords", sizeof(status));
                return;
            }
            for (int i = 0; password[i]; i++)
                if (password[i] != confirm[i]) {
                    copy(status, "Passwords do not match", sizeof(status));
                    return;
                }
            copy(c.password, password, sizeof(c.password));
        } else if (op == USER_CTL_ROLE) {
            c.level = rows[selected].level == 0 ? 1 : 0;
        }
    }
    int rc = sys_user(&c);
    erase((char *)&c, sizeof(c));
    erase(password, sizeof(password));
    erase(confirm, sizeof(confirm));
    erase(admin_password, sizeof(admin_password));
    copy(status, rc == 0 ? "Account updated" :
         "Could not update account; check admin password", sizeof(status));
    load();
}

static char *focused(void)
{
    if (focus == 0) return name;
    if (focus == 1) return password;
    if (focus == 2) return confirm;
    if (focus == 3) return admin_password;
    return (char *)0;
}

static void key(char c)
{
    char *b = focused();
    int cap = focus == 0 ? sizeof(name) : sizeof(password);
    if (focus == 2) cap = sizeof(confirm);
    if (focus == 3) cap = sizeof(admin_password);
    if (c == '\t') focus = (focus + 1) % 4;
    else if (c == '\n' || c == '\r') submit(USER_CTL_ADD);
    else if (b && c == '\b') {
        int n = length(b); if (n) b[n - 1] = 0;
    } else if (b && c >= 0x20 && c < 0x7f && length(b) < cap - 1) {
        int n = length(b); b[n] = c; b[n + 1] = 0;
    }
}

void _start(void)
{
    win = (int)win_create(W, H, ulocale_language_polish()
                                   ? "Konta użytkowników" : "User Accounts");
    if (win < 0) sys_exit(1);
    load();
    draw();
    int language = ulocale_language_polish();
    for (;;) {
        struct win_event ev;
        int redraw = 0;
        int current_language = ulocale_language_polish();
        if (current_language != language) {
            language = current_language;
            redraw = 1;
        }
        while (win_poll(win, &ev) == 1) {
            redraw = 1;
            if (ev.type == WEV_CLOSE) sys_exit(0);
            if (ev.type == WEV_SIZE) { cw = ev.x; ch = ev.y; }
            else if (ev.type == WEV_KEY) key((char)ev.val);
            else if (ev.type == WEV_SCROLL && ev.x < 230) {
                top -= ev.val;
                if (top < 0) top = 0;
                if (nrows > rows_visible() && top > nrows - rows_visible())
                    top = nrows - rows_visible();
            }
            else if (ev.type == WEV_CLICK) {
                int visible = rows_visible();
                if (ev.y >= 94 && ev.y < 94 + visible * 22 && ev.x < 230) {
                    selected = top + (ev.y - 94) / 22;
                    if (selected >= nrows) selected = -1;
                } else if (ev.x >= 246 && ev.y >= 286 && ev.y < 304) {
                    make_admin = !make_admin;
                } else if (ev.y >= ch - 48 && ev.y < ch - 12) {
                    if (ev.x < 132) submit(USER_CTL_ADD);
                    else if (ev.x < 254) submit(USER_CTL_PASSWORD);
                    else if (ev.x < 376) submit(USER_CTL_ROLE);
                    else if (ev.x < 504) submit(USER_CTL_DELETE);
                } else if (ev.x >= 246 && ev.y >= 72 && ev.y < 120) focus = 0;
                else if (ev.x >= 246 && ev.y >= 124 && ev.y < 172) focus = 1;
                else if (ev.x >= 246 && ev.y >= 176 && ev.y < 224) focus = 2;
                else if (ev.x >= 246 && ev.y >= 228 && ev.y < 276) focus = 3;
            }
        }
        if (redraw) draw();
        sys_sleep(80);
    }
}
