/* gtapp - an application package (.tapp), opened from the Files app
 *
 *   gtapp FILE
 *
 * What the package is - its name, version, the program it runs, its
 * size - and whether the key that signs ThreadOS signed it, read and
 * checked as 'tapp verify' does (user/utapp.h). A package that is
 * signed by it has an Install button; one that is not has only Close,
 * and the window says why. Installing is an administrator's to do: the
 * kernel says no to anybody else, and the window then asks for the
 * administrator's password, as the installer's does (user/ginstal.c),
 * and gives the right back when it is done.
 *
 * Every line sits on a grid of ROW_H from ROW_Y0, so that a test reads
 * the window the way a person does (scripts/tests/t_appsinst.py).
 */

#include "udraw.h"
#include "ulocale.h"
#include "utapp.h"

#define W0 420
#define H0 236
#define PAD 14
#define ROW_H 16
#define ROW_Y0 10
#define BTN_W 150
#define BTN_H 26
#define BTN_GAP 10

static int rowy(int r) { return ROW_Y0 + r * ROW_H; }

#define S_LOOK 0          /* what the package is */
#define S_PASS 1          /* the kernel said no - the password */
#define S_DONE 2          /* installed, or why not */

static struct ud_look L;
static struct tapp_pkg pkg;
static uint8_t *data;             /* the whole package, when it is one */
static int checked = -1;          /* TAPP_*, or -1: not a package */
static const char *why = "";
static const char *file;
static int state = S_LOOK;
static char pass[USER_PASSWORD_MAX + 1];
static int plen;
static const char *result = "";
static int result_bad;
static int we_authed;
static int cw = W0, ch = H0;
static int nbuttons;

static const char *lang(const char *english, const char *polish)
{
    return ulocale_language_polish() ? polish : english;
}

static void say(int win, int x, int r, const char *s, uint32_t rgb, int font)
{
    ud_text(win, x, ud_baseline(rowy(r), ROW_H, UD_PX), s, rgb, font, UD_PX);
}

static int put(char *d, int at, const char *s)
{
    while (*s && at < 95)
        d[at++] = *s++;
    d[at] = 0;
    return at;
}

static int put_num(char *d, int at, uint32_t v)
{
    char t[12];
    int n = 0;
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n && at < 95)
        d[at++] = t[--n];
    d[at] = 0;
    return at;
}

static int btn_y(void) { return ch - BTN_H - 12; }

static int btn_x(int which)
{
    int span = nbuttons * BTN_W + (nbuttons - 1) * BTN_GAP;
    return (cw - span) / 2 + which * (BTN_W + BTN_GAP);
}

static void buttons(int win, const char *first, const char *second)
{
    nbuttons = second ? 2 : 1;
    const char *label[2] = { first, second };
    for (int i = 0; i < nbuttons; i++) {
        ud_round(win, btn_x(i), btn_y(), BTN_W, BTN_H, 7, L.accent);
        ud_text_mid(win, btn_x(i), btn_y(), BTN_W, BTN_H, label[i],
                    L.on_accent, UD_BOLD, UD_PX);
    }
}

/* 0 = no button, 1 = the first, 2 = the second */
static int in_button(int x, int y)
{
    if (y < btn_y() || y >= btn_y() + BTN_H)
        return 0;
    for (int i = 0; i < nbuttons; i++)
        if (x >= btn_x(i) && x < btn_x(i) + BTN_W)
            return i + 1;
    return 0;
}

static void draw(int win)
{
    char line[96];
    ud_look(&L);
    ud_fill(win, 0, 0, cw, ch, L.bg);
    say(win, PAD, 0, lang("Application package", "Paczka aplikacji"),
        L.text, UD_BOLD);

    if (checked < 0) {
        say(win, PAD, 2, file, L.text, UD_SANS);
        put(line, put(line, 0, lang("Not a package: ", "To nie paczka: ")),
            why);
        say(win, PAD, 3, line, L.danger, UD_SANS);
        buttons(win, lang("Close", "Zamknij"), 0);
        return;
    }

    int at = put(line, 0, ulocale_language_polish() && pkg.name_pl[0]
                          ? pkg.name_pl : pkg.name);
    at = put(line, at, lang(", version ", ", wersja "));
    put_num(line, at, pkg.version);
    say(win, PAD, 2, line, L.text, UD_BOLD);
    at = put(line, 0, pkg.id);
    at = put(line, at, lang(" - runs ", " - uruchamia "));
    at = put(line, at, pkg.exec);
    at = put(line, at, ", ");
    at = put_num(line, at, (pkg.length + 1023) / 1024);
    put(line, at, " KiB");
    say(win, PAD, 3, line, L.dim, UD_SANS);

    if (checked == TAPP_SIGNED)
        say(win, PAD, 5, lang("Signed by the system's key",
                              "Podpisana kluczem systemu"),
            L.text, UD_SANS);
    else
        say(win, PAD, 5, checked == TAPP_NOT_SIGNED
            ? lang("NOT signed by the system's key",
                   "NIE podpisana kluczem systemu")
            : lang("Not signed", "Bez podpisu"), L.danger, UD_SANS);

    if (state == S_DONE) {
        say(win, PAD, 7, result, result_bad ? L.danger : L.text, UD_BOLD);
        buttons(win, lang("Close", "Zamknij"), 0);
        return;
    }
    if (state == S_PASS) {
        say(win, PAD, 7, lang("The administrator's password:",
                              "Hasło administratora:"), L.text, UD_SANS);
        char dots[USER_PASSWORD_MAX + 1];
        int n = 0;
        for (; n < plen; n++)
            dots[n] = '*';
        dots[n] = 0;
        ud_round(win, PAD, rowy(8) - 3, cw - 2 * PAD, ROW_H + 6, 6, L.field);
        int end = ud_text(win, PAD + 8, ud_baseline(rowy(8), ROW_H, UD_PX),
                          dots, L.text, UD_SANS, UD_PX);
        ud_fill(win, end + 1, rowy(8) + 1, 1, ROW_H - 2, L.text);
        if (result[0])
            say(win, PAD, 10, result, L.danger, UD_SANS);
        buttons(win, lang("Install", "Zainstaluj"), lang("Cancel", "Anuluj"));
        return;
    }
    if (checked == TAPP_SIGNED) {
        say(win, PAD, 7, lang("It goes into /EFI/ThreadOS/apps,",
                              "Trafi do /EFI/ThreadOS/apps,"),
            L.text, UD_SANS);
        say(win, PAD, 8, pkg.dock
            ? lang("and into the dock.", "i do docka.")
            : lang("and runs by its name.", "i uruchomi się po nazwie."),
            L.text, UD_SANS);
        buttons(win, lang("Install", "Zainstaluj"), lang("Cancel", "Anuluj"));
    } else {
        say(win, PAD, 7, lang("Only a package signed by the system's key",
                              "Zainstalować można tylko paczkę"),
            L.text, UD_SANS);
        say(win, PAD, 8, lang("can be installed.",
                              "podpisaną kluczem systemu."),
            L.text, UD_SANS);
        buttons(win, lang("Close", "Zamknij"), 0);
    }
}

static void done(const char *english, const char *polish, int bad)
{
    result = lang(english, polish);
    result_bad = bad;
    state = S_DONE;
}

static void install(void)
{
    int rc = sys_package_install(data, pkg.length);
    switch (rc) {
    case 0:
        done("Installed - it runs now", "Zainstalowana - działa od razu", 0);
        break;
    case 1:
        done("Installed - from the next start",
             "Zainstalowana - od następnego startu", 0);
        break;
    case -2:
        state = S_PASS;
        plen = 0;
        result = "";
        break;
    case -3:
        done("Only an installed system takes packages",
             "Paczki przyjmuje tylko zainstalowany system", 1);
        break;
    case -4:
        done("Not a package for this machine",
             "To nie paczka dla tego komputera", 1);
        break;
    case -5:
        done("No room for it on the EFI partition",
             "Brak miejsca na partycji EFI", 1);
        break;
    case -6:
        done("That version is installed already",
             "Ta wersja jest już zainstalowana", 1);
        break;
    default:
        done("Writing it failed", "Zapis się nie udał", 1);
        break;
    }
}

static void try_password(void)
{
    pass[plen] = 0;
    int ok = plen && sys_auth(pass) == 0;
    for (int i = 0; i < plen; i++)
        pass[i] = 0;
    plen = 0;
    if (!ok) {
        result = lang("wrong password", "złe hasło");
        return;
    }
    we_authed = 1;
    install();
}

static void leave(void)
{
    /* Only a right this window asked for is a right to give back */
    if (we_authed)
        sys_drop();
    sys_exit(0);
}

void _start(int argc, char **argv)
{
    file = argc > 1 ? argv[1] : "";
    data = tapp_load(file, &pkg, &why);
    if (data) {
        struct tapp_src m = { -1, -1, data, pkg.length };
        int from;
        checked = tapp_verify(&m, &pkg, &from);
    }
    int win = (int)win_create(W0, H0, "Package");
    if (win < 0)
        sys_exit(1);
    ulocale_window_title(win, "Package", "Paczka");
    draw(win);

    for (;;) {
        struct win_event ev;
        int again = 0;
        while (win_poll(win, &ev) == 1) {
            again = 1;
            if (ev.type == WEV_CLOSE)
                leave();
            if (ev.type == WEV_SIZE) {
                cw = ev.x;
                ch = ev.y;
                continue;
            }
            if (ev.type == WEV_CLICK) {
                int b = in_button(ev.x, ev.y);
                if (!b)
                    continue;
                if (state == S_LOOK && b == 1 && checked == TAPP_SIGNED)
                    install();
                else if (state == S_PASS && b == 1)
                    try_password();
                else
                    leave();
                continue;
            }
            if (ev.type == WEV_KEY && state == S_PASS) {
                char c = (char)ev.val;
                if (c == '\n' || c == '\r')
                    try_password();
                else if (c == '\b' && plen)
                    plen--;
                else if (c >= 32 && c < 127 && plen < USER_PASSWORD_MAX)
                    pass[plen++] = c;
            }
        }
        if (again)
            draw(win);
        sys_sleep(60);
    }
}
