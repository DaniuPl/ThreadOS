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
 */

#include "udraw.h"
#include "ulocale.h"
#include "pciinfo.h"
#include "ubt.h"
#include "uwifi.h"
#include "udevices.h"

/* Small enough to open in the cascade's third place on a 640x480
   screen, between the bar and the dock */
#define W0 500
#define H0 362

#define SIDE_W 150            /* the sections, down the left */
#define SIDE_Y0 44            /* the first section's top */
#define SIDE_H 21             /* fifteen of them have to fit in 362 */
#define ICON 16               /* a section's coloured square */
#define SIDE_TEXT_X 36        /* a section's name, right of its square */
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
#define WIFI_LIST_Y 166       /* Wi-Fi: the first network's row (PANE_Y0,
                                 three lines, the button and a gap) */
#define WIFI_ROW_H 20         /* ...and each one's height */
#define WIFI_PICK_W 296       /* a row as a click and a pick see it */
#define WIFI_JOIN_H 40        /* the password and Join under the list */
#define WIFI_JOIN_W 70
#define BT_ROW_H 18           /* Bluetooth: a device's row */
#define BT_PAIRED_X 280       /* ...where it says Paired */
#define BT_PICK_W 336         /* ...and its width, picked */
#define BT_ASK_H 40           /* Pair, or Yes and No, under the list */
#define BT_ASK_YES_X 110      /* Yes, after the number */
#define DEV_MAX 64            /* Devices: as many as are listed */
#define DEV_LIST_Y 108        /* the first device's row: under two lines
                                 of figures (PANE_Y0 + 2 * INFO_H + 8) */
#define DEV_ROW_H 20
#define DEV_STATE_W 120       /* its state, at the row's right end */
#define DEV_AMBER 0xFF9F0A    /* what needs a person: no driver, no firmware */
#define LOCK_WAY_W 84         /* Screen Lock: a way to unlock, as a chip */
#define LOCK_PIN_W 92         /* ...the PIN field */
#define LOCK_PW_W 140         /* ...and the password beside it */
#define PASS_W 300            /* the password panel */
#define PASS_H 96

/* The clipboard's keys, Ctrl+Insert and Shift+Insert, as the kernel
   delivers them (kernel/include/keyboard.h) */
#define KEY_COPY  0x1A
#define KEY_PASTE 0x1B

/* In groups, the way a Mac lays them out: the radios and the network,
   sound, how the machine looks and locks, the pointer and the files,
   and the machine itself. Updates comes last because only the 64-bit
   system has it, so every other section is in the same place in both. */
#define SEC_WIFI    0
#define SEC_BT      1
#define SEC_NET     2
#define SEC_SOUND   3
#define SEC_GENERAL 4
#define SEC_LOOK    5
#define SEC_DESK    6
#define SEC_DISPLAY 7
#define SEC_LOCK    8
#define SEC_MOUSE   9
#define SEC_FILES   10
#define SEC_REGION  11
#define SEC_ABOUT   12
#define SEC_DEVICES 13
#define SEC_UPDATES 14
#define SEC_N       15

static const char *const sections[] = {
    "Wi-Fi", "Bluetooth", "Network", "Sound", "General", "Appearance",
    "Desktop & Dock", "Display", "Screen Lock", "Mouse", "Files", "Region",
    "About", "Devices", "Updates"
};
/* A line under the last of each group */
static int group_ends(int i)
{
    return i == SEC_NET || i == SEC_SOUND || i == SEC_LOCK ||
           i == SEC_REGION;
}
/* Each section's square, under the picture of it */
static const uint32_t section_colour[] = {
    0x0A84FF, 0x0A84FF, 0x0A84FF, 0xFF375F, 0x8E8E93, 0x3A3A3C, 0x5E5CE6,
    0x0A84FF, 0x3A3A3C, 0x8E8E93, 0x1E8CF0, 0x34A853, 0x8E8E93, 0x30B0C7,
    0xFF9500
};

static int win = -1;
static int cw = W0, ch = H0;

/* A section's picture, white on its ICON-sized square at x, y - drawn
   with the window's own shapes, as the desktop draws the programs'.
   'bg' is the square's colour, for the holes. */
static void section_icon(int i, int x, int y, uint32_t bg)
{
    const uint32_t w = 0xFFFFFF;
    switch (i) {
    case SEC_WIFI:                      /* two arcs over a dot */
        ud_disc(win, x + 6, y + 10, 4, w);
        ud_clip(win, x + 1, y + 1, 14, 9);
        ud_ring(win, x + 4, y + 7, 8, 8, 4, w);
        ud_ring(win, x + 1, y + 4, 14, 14, 7, w);
        ud_ring(win, x + 2, y + 5, 12, 12, 6, w);
        ud_unclip(win);
        break;
    case SEC_BT:                        /* the rune */
        ud_line(win, x + 5, y + 5, x + 11, y + 10, 24, w);
        ud_line(win, x + 11, y + 10, x + 8, y + 13, 24, w);
        ud_line(win, x + 8, y + 13, x + 8, y + 2, 24, w);
        ud_line(win, x + 8, y + 2, x + 11, y + 5, 24, w);
        ud_line(win, x + 11, y + 5, x + 5, y + 10, 24, w);
        break;
    case SEC_NET:                       /* three machines, joined */
        ud_line(win, x + 8, y + 4, x + 4, y + 12, 20, w);
        ud_line(win, x + 8, y + 4, x + 12, y + 12, 20, w);
        ud_line(win, x + 4, y + 12, x + 12, y + 12, 20, w);
        ud_disc(win, x + 6, y + 2, 5, w);
        ud_disc(win, x + 2, y + 10, 5, w);
        ud_disc(win, x + 10, y + 10, 5, w);
        break;
    case SEC_SOUND:                     /* a loudspeaker, and its sound */
        ud_fill(win, x + 2, y + 6, 3, 4, w);
        for (int k = 0; k < 4; k++)
            ud_line(win, x + 5 + k, y + 6 - k, x + 5 + k, y + 9 + k, 16, w);
        ud_clip(win, x + 10, y + 1, 6, 14);
        ud_ring(win, x + 6, y + 5, 6, 6, 3, w);
        ud_ring(win, x + 4, y + 3, 10, 10, 5, w);
        ud_unclip(win);
        break;
    case SEC_GENERAL:                   /* a cogwheel */
        ud_line(win, x + 8, y + 2, x + 8, y + 14, 40, w);
        ud_line(win, x + 2, y + 8, x + 14, y + 8, 40, w);
        ud_line(win, x + 4, y + 4, x + 12, y + 12, 40, w);
        ud_line(win, x + 12, y + 4, x + 4, y + 12, 40, w);
        ud_disc(win, x + 3, y + 3, 11, w);
        ud_disc(win, x + 6, y + 6, 5, bg);
        break;
    case SEC_LOOK:                      /* light and dark, half and half */
        ud_ring(win, x + 2, y + 2, 12, 12, 6, w);
        ud_clip(win, x + 2, y + 2, 6, 12);
        ud_disc(win, x + 2, y + 2, 12, w);
        ud_unclip(win);
        break;
    case SEC_DESK:                      /* a screen with its dock */
        ud_round(win, x + 1, y + 2, 14, 12, 2, w);
        ud_round(win, x + 2, y + 3, 12, 10, 1, bg);
        ud_round(win, x + 4, y + 10, 8, 2, 1, w);
        break;
    case SEC_DISPLAY:                   /* a screen on its foot */
        ud_round(win, x + 1, y + 3, 14, 9, 2, w);
        ud_round(win, x + 2, y + 4, 12, 7, 1, bg);
        ud_fill(win, x + 7, y + 12, 2, 2, w);
        ud_fill(win, x + 4, y + 13, 8, 1, w);
        break;
    case SEC_LOCK:                      /* a padlock */
        ud_clip(win, x, y, 16, 8);
        ud_ring(win, x + 4, y + 2, 8, 12, 4, w);
        ud_ring(win, x + 5, y + 3, 6, 10, 3, w);
        ud_unclip(win);
        ud_round(win, x + 3, y + 7, 10, 7, 2, w);
        ud_disc(win, x + 7, y + 9, 2, bg);
        break;
    case SEC_MOUSE:                     /* a mouse, its button split */
        ud_ring(win, x + 4, y + 1, 8, 14, 4, w);
        ud_ring(win, x + 5, y + 2, 6, 12, 3, w);
        ud_line(win, x + 8, y + 2, x + 8, y + 6, 16, w);
        break;
    case SEC_FILES:                     /* a folder */
        ud_round(win, x + 2, y + 3, 6, 3, 1, w);
        ud_round(win, x + 2, y + 5, 12, 8, 1, w);
        break;
    case SEC_REGION:                    /* a globe */
        ud_ring(win, x + 2, y + 2, 12, 12, 6, w);
        ud_ring(win, x + 5, y + 2, 6, 12, 3, w);
        ud_line(win, x + 2, y + 8, x + 14, y + 8, 16, w);
        ud_line(win, x + 4, y + 5, x + 12, y + 5, 12, w);
        ud_line(win, x + 4, y + 11, x + 12, y + 11, 12, w);
        break;
    case SEC_ABOUT:                     /* information, in a ring */
        ud_ring(win, x + 1, y + 1, 14, 14, 7, w);
        ud_ring(win, x + 2, y + 2, 12, 12, 6, w);
        ud_disc(win, x + 7, y + 4, 2, w);
        ud_fill(win, x + 7, y + 7, 2, 5, w);
        break;
    case SEC_DEVICES:                   /* a chip on its pins */
        for (int k = 0; k < 3; k++) {
            ud_fill(win, x + 4 + 3 * k, y + 1, 2, 14, w);
            ud_fill(win, x + 1, y + 4 + 3 * k, 14, 2, w);
        }
        ud_round(win, x + 3, y + 3, 10, 10, 2, w);
        ud_round(win, x + 5, y + 5, 6, 6, 1, bg);
        break;
    default:                            /* Updates: down, into the tray */
        ud_line(win, x + 8, y + 2, x + 8, y + 10, 24, w);
        ud_line(win, x + 4, y + 7, x + 8, y + 11, 24, w);
        ud_line(win, x + 12, y + 7, x + 8, y + 11, 24, w);
        ud_line(win, x + 3, y + 13, x + 13, y + 13, 24, w);
        break;
    }
}
static int section = SEC_GENERAL;       /* where it opens */
static struct ud_look l;

/* --- the settings, as the kernel has them ---------------------------- */

/* Room for every setting there is: with sixteen, the seventeenth -
   the language - was never read, and Region showed no language chosen */
#define MAXCFG 32
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

/* Three strings one after another, cut to the room */
static void cat3(char *d, int max, const char *a, const char *b,
                 const char *c)
{
    const char *part[3] = { a, b, c };
    int n = 0;
    for (int k = 0; k < 3; k++)
        for (int i = 0; part[k][i] && n < max - 1; i++)
            d[n++] = part[k][i];
    d[n] = 0;
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
static char pass[USER_PASSWORD_MAX + 1];
static int plen;
static char want_name[CFG_NAME_MAX], want_value[CFG_VAL_MAX];
/* 1 saved, 2 changed but there is no disk to keep it on, -1 refused
   (a wrong password), -2 refused (not a value it takes), 0 nothing */
static int said;

#define UPDATE_OUTPUT_MAX 2048
#define UPDATE_CHECK 0
#define UPDATE_INSTALL 1
#define UPDATE_ROLLBACK 2
static int update_allowed;
static int update_pid = -1;
static int update_fd = -1;
static int update_operation;
static int update_available;
static int update_auth_pending;
static char update_message[80] = "Check for signed releases from ThreadOS.";
static char update_output[UPDATE_OUTPUT_MAX];
static int update_output_len;

static int contains(const char *text, const char *part)
{
    if (!*part)
        return 1;
    for (; *text; text++) {
        int i = 0;
        while (part[i] && text[i] == part[i])
            i++;
        if (!part[i])
            return 1;
    }
    return 0;
}

static int starts_with(const char *text, const char *prefix)
{
    while (*prefix && *text == *prefix) {
        text++;
        prefix++;
    }
    return !*prefix;
}

static void update_start(int operation)
{
    int fd[2];
    if (update_pid >= 0)
        return;
    if (sys_pipe(fd) < 0) {
        copy(update_message, "Could not start the update process.",
             sizeof(update_message));
        update_available = 0;
        return;
    }

    int64_t pid = sys_fork();
    if (pid < 0) {
        sys_close(fd[0]);
        sys_close(fd[1]);
        copy(update_message, "Could not start the update process.",
             sizeof(update_message));
        update_available = 0;
        return;
    }
    if (pid == 0) {
        sys_close(fd[0]);
        if (sys_dup2(fd[1], 1) < 0 || sys_dup2(fd[1], 2) < 0)
            sys_exit(126);
        sys_close(fd[1]);
        if (operation == UPDATE_INSTALL) {
            char *const args[] = { "update", (char *)0 };
            sys_exec("update", args);
        } else if (operation == UPDATE_ROLLBACK) {
            char *const args[] = { "update", "rollback", (char *)0 };
            sys_exec("update", args);
        } else {
            char *const args[] = { "update", "check", (char *)0 };
            sys_exec("update", args);
        }
        sys_exit(127);
    }

    sys_close(fd[1]);
    update_fd = fd[0];
    update_pid = (int)pid;
    update_operation = operation;
    update_output_len = 0;
    update_output[0] = 0;
    update_available = 0;
    if (operation == UPDATE_INSTALL)
        copy(update_message, "Installing the signed update...",
             sizeof(update_message));
    else if (operation == UPDATE_ROLLBACK)
        copy(update_message, "Restoring the previous system...",
             sizeof(update_message));
    else
        copy(update_message, "Checking for signed updates...",
             sizeof(update_message));
}

static int update_poll(void)
{
    if (update_pid < 0)
        return 0;
    int code = 0;
    int rc = sys_trywait(update_pid, &code);
    if (rc == -3)
        return 0;
    if (rc != 0) {
        if (update_fd >= 0)
            sys_close(update_fd);
        update_fd = -1;
        update_pid = -1;
        update_operation = UPDATE_CHECK;
        copy(update_message, "Could not read the update process result.",
             sizeof(update_message));
        return 1;
    }

    int read_failed = 0;
    if (update_fd >= 0) {
        char chunk[256];
        for (;;) {
            int64_t n = sys_read(update_fd, chunk, sizeof(chunk));
            if (n < 0) {
                read_failed = 1;
                break;
            }
            if (!n)
                break;
            int room = UPDATE_OUTPUT_MAX - 1 - update_output_len;
            int take = n > room ? room : (int)n;
            for (int i = 0; i < take; i++)
                update_output[update_output_len++] = chunk[i];
            update_output[update_output_len] = 0;
            if (!room)
                break;
        }
        sys_close(update_fd);
    }
    update_fd = -1;
    update_pid = -1;

    if (read_failed) {
        copy(update_message, "Could not read the update process output.",
             sizeof(update_message));
    } else if (code == 0 && update_operation == UPDATE_INSTALL &&
        contains(update_output, "updated to release sequence")) {
        copy(update_message, "Installed. Restart to apply the update.",
             sizeof(update_message));
    } else if (code == 0 && update_operation == UPDATE_ROLLBACK &&
               contains(update_output, "the previous system was restored")) {
        copy(update_message, "Previous system restored. Restart to use it.",
             sizeof(update_message));
    } else if (code == 0 && contains(update_output, "a newer system is available")) {
        update_available = 1;
        copy(update_message, "A newer signed update is available.",
             sizeof(update_message));
    } else if (code == 0 && contains(update_output, "this system is up to date")) {
        copy(update_message, "This system is up to date.",
             sizeof(update_message));
    } else if (code == 0) {
        copy(update_message, "The update check completed.",
             sizeof(update_message));
    } else {
        const char *last = 0;
        for (int i = 0; i + 9 <= update_output_len; i++) {
            if (starts_with(update_output + i, "[update] "))
                last = update_output + i + 9;
        }
        if (last) {
            int n = 0;
            while (last[n] && last[n] != '\n' &&
                   n < (int)sizeof(update_message) - 1)
                n++;
            for (int i = 0; i < n; i++)
                update_message[i] = last[i];
            update_message[n] = 0;
        } else {
            copy(update_message, "The update failed. Try again later.",
                 sizeof(update_message));
        }
    }
    update_operation = UPDATE_CHECK;
    return 1;
}

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
    int for_update = update_auth_pending;
    update_auth_pending = 0;
    pass[plen] = 0;
    asking = 0;
    if (!plen || sys_auth(pass) != 0) {
        said = plen ? -1 : 0;
    } else {
        if (for_update) {
            update_start(for_update);
        } else {
            said = outcome(sys_cfg_set(want_name, want_value), want_name,
                           want_value);
            sys_drop();
        }
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
#define F_WALL 3
#define F_PIN  4                  /* the lock's: a new PIN */
#define F_LOCKPW 5                /* ...and one's own password, to change it */
#define F_WIFIPW 6                /* a network's password, to join it */
#define F_N    7
static const char *const field_setting[F_N] = {
    "name", "resolution", "dns", "wallpaper", (const char *)0, (const char *)0,
    (const char *)0
};
/* room for a WPA2 password (63) as well as a setting's value */
#define FIELD_MAX 64
static char fields[F_N][FIELD_MAX];
static int focus = -1;            /* the field that has the keys */

/* A field shows the setting until it is typed into */
static void fields_from_settings(void)
{
    for (int i = 0; i < F_N; i++)
        if (i != focus && field_setting[i])
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
#define A_BT_SEARCH 12
#define A_WIFI_SEARCH 15
#define A_WIFI_PICK 17            /* a network in the list */
#define A_BT_PICK 20              /* a Bluetooth device in the list */
#define A_BT_PAIR 21
#define A_BT_YES 22               /* the numbers match */
#define A_BT_NO 23
#define A_BT_FORGET 24
#define A_WIFI_JOIN 18            /* ...joined, with the password typed */
#define A_WIFI_LEAVE 19
#define A_DEVICE 16               /* a device opened, or -1: the list */
#define A_UNLOCK_PICK 13          /* the way the lock opens, chosen */
#define A_UNLOCK_SAVE 14          /* ...and saved, with the password */
#define A_UPDATE_CHECK   9
#define A_UPDATE_INSTALL 10
#define A_UPDATE_ROLLBACK 11

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
    ud_text_l(win, PANE_X, ud_baseline(y, 18, UD_PX), text, l.text, UD_SANS,
              UD_PX);
}

/* One choice out of a few: lit when it is the one in force */
static void chip(int x, int y, const char *text, int on, const char *name,
                 const char *value)
{
    ud_round(win, x, y, CHIP_W, CHIP_H, 8, on ? l.accent : l.row);
    ud_text_mid_l(win, x, y, CHIP_W, CHIP_H, text,
                  on ? l.on_accent : l.text, UD_SANS, UD_PX);
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
    ud_text_mid_l(win, x, y, w, CHIP_H, text, l.on_accent, UD_SANS, UD_PX);
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
    int end = ud_text_l(win, PANE_X + b + 8,
                        ud_baseline(y, CHIP_H, UD_PX), text, l.text, UD_SANS,
                        UD_PX);
    hot(PANE_X, y, end - PANE_X, CHIP_H, A_TOGGLE, 0, name, (const char *)0);
}

static void note(int y, const char *text)
{
    ud_text_l(win, PANE_X, ud_baseline(y, 16, 11), text, l.dim, UD_SANS, 11);
}

/* A line of figures: what it is, and its value */
static void info(int i, const char *name, const char *value)
{
    int y = PANE_Y0 + i * INFO_H;
    int base = ud_baseline(y, INFO_H, UD_PX);
    ud_text_l(win, PANE_X, base, name, l.dim, UD_SANS, UD_PX);
    ud_text(win, PANE_X + INFO_X, base, value, l.text, UD_SANS, UD_PX);
}

/* ...and one whose value is a word of ours, to be translated, rather
   than a figure or a name the machine gave */
static void info_l(int i, const char *name, const char *value)
{
    int y = PANE_Y0 + i * INFO_H;
    int base = ud_baseline(y, INFO_H, UD_PX);
    ud_text_l(win, PANE_X, base, name, l.dim, UD_SANS, UD_PX);
    ud_text_l(win, PANE_X + INFO_X, base, value, l.text, UD_SANS, UD_PX);
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

    y += ROW_H;
    row_name(y, "Wallpaper file");
    field(y + CTRL_DY, F_WALL);

    note(y + ROW_H, "Paint's Wall button applies a drawing as wallpaper.");
    note(y + ROW_H + 14, "Wallpaper files are read from the system root.");
}

/* Where the dock stands, whether it magnifies, whether the bar stays -
   the way a Mac's "Desktop & Dock" has them */
static void draw_desk(void)
{
    static const char *const sides[3] = { "Left", "Bottom", "Right" };
    static const char *const side_values[3] = { "left", "bottom", "right" };
    static const char *const styles[2] = { "macOS", "Windows" };
    static const char *const style_values[2] = { "mac", "windows" };
    int y = PANE_Y0;
    /* The whole desktop's way: a bar at the top and a dock, or a
       taskbar along the bottom with Start */
    row_name(y, "Interface style");
    const char *style = setting("layout");
    if (!style[0])
        style = "mac";
    for (int i = 0; i < 2; i++) {
        int x = PANE_X + i * (LOCK_WAY_W + CHIP_GAP);
        int on = same(style, style_values[i]);
        ud_round(win, x, y + CTRL_DY, LOCK_WAY_W, CHIP_H, 8,
                 on ? l.accent : l.row);
        ud_text_mid_l(win, x, y + CTRL_DY, LOCK_WAY_W, CHIP_H, styles[i],
                      on ? l.on_accent : l.text, UD_SANS, UD_PX);
        hot(x, y + CTRL_DY, LOCK_WAY_W, CHIP_H, A_SET, 0, "layout",
            style_values[i]);
    }
    y += ROW_H;
    if (same(style, "windows")) {
        /* The rest is the dock's, and the taskbar is not a dock: it
           stays at the bottom, the same size, and does not hide */
        note(y, "In the Windows style the taskbar stands at the bottom.");
        note(y + 14, "The dock's settings are for the macOS style.");
        return;
    }
    row_name(y, "Position on screen");
    const char *now = setting("dockpos");
    if (!now[0])
        now = "bottom";
    for (int i = 0; i < 3; i++)
        chip(PANE_X + i * (CHIP_W + CHIP_GAP), y + CTRL_DY, sides[i],
             same(now, side_values[i]), "dockpos", side_values[i]);
    y += ROW_H;
    check_box(y, "Hide the dock until the pointer reaches it",
              same(setting("dockhide"), "1"), "dockhide");
    y += CHIP_H + 8;
    check_box(y, "Magnify the icons under the pointer",
              same(setting("dockmag"), "1"), "dockmag");
    y += CHIP_H + 8;
    check_box(y, "Hide the menu bar when it is not used",
              same(setting("barhide"), "1"), "barhide");
    y += CHIP_H + 16;
    note(y, "The size of the dock is in Appearance.");
    note(y + 14, "With the bar hidden, the pointer at the top brings it.");
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
    int end = ud_text_l(win, PANE_X + b + 8,
                        ud_baseline(y, CHIP_H, UD_PX), "Mute", l.text,
                        UD_SANS, UD_PX);
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
    struct sysinfo64 si;
    sys_sysinfo(&si);

    info(i++, "System", "ThreadOS");
    info(i++, "Kernel", "64-bit, long mode");
    /* The release number travels in the initrd (the update program
       reads it from there too) */
    char v[16];
    int64_t n = sys_initrd_read("version", v, sizeof(v) - 1);
    if (n > 0) {
        v[n] = 0;
        for (int k = 0; v[k]; k++)
            if (v[k] == '\n')
                v[k] = 0;
        info(i++, "Version", v);
    }
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

static void draw_region(void)
{
    static const char *const dates[] = { "D/M/Y", "M/D/Y", "ISO" };
    static const char *const date_values[] = { "dmy", "mdy", "iso" };
    static const char *const times[] = { "24-hour", "12-hour" };
    static const char *const time_values[] = { "24h", "12h" };
    static const char *const numbers[] = { "1,234.5", "1 234,5" };
    static const char *const number_values[] = { "dot", "comma" };
    static const char *const languages[] = { "English", "Polski" };
    static const char *const language_values[] = { "en", "pl" };
    int y = PANE_Y0;

    row_name(y, "Date order");
    chips(y + CTRL_DY, 3, dates, date_values, "datefmt");
    y += ROW_H;
    row_name(y, "Time");
    chips(y + CTRL_DY, 2, times, time_values, "timefmt");
    y += ROW_H;
    row_name(y, "Numbers");
    chips(y + CTRL_DY, 2, numbers, number_values, "numfmt");
    y += ROW_H;
    row_name(y, "Language");
    chips(y + CTRL_DY, 2, languages, language_values, "language");
    note(y + ROW_H, "Clock, calculator and file sizes follow these formats.");
}

/* What wireless cards are called, for the ones a laptop of the last
   ten years is likely to have. Anything else is shown by its numbers. */
static const struct { uint16_t vendor, device; const char *name; } wifi_names[] = {
    { 0x8086, 0x08B1, "Intel Wireless 7260" },
    { 0x8086, 0x08B2, "Intel Wireless 7260" },
    { 0x8086, 0x08B3, "Intel Wireless 3160" },
    { 0x8086, 0x08B4, "Intel Wireless 3160" },
    { 0x8086, 0x095A, "Intel Wireless 7265" },
    { 0x8086, 0x095B, "Intel Wireless 7265" },
    { 0x8086, 0x3165, "Intel Wireless 3165" },
    { 0x8086, 0x24F3, "Intel Wireless 8260" },
    { 0x8086, 0x24FD, "Intel Wireless 8265" },
    { 0x8086, 0x2526, "Intel Wireless-AC 9260" },
    { 0x8086, 0x2723, "Intel Wi-Fi 6 AX200" },
    { 0x8086, 0x2725, "Intel Wi-Fi 6E AX210" },
    { 0x10EC, 0xB822, "Realtek RTL8822BE" },
    { 0x10EC, 0xC821, "Realtek RTL8821CE" },
    { 0x10EC, 0x8179, "Realtek RTL8188EE" },
    { 0x168C, 0x003E, "Qualcomm Atheros QCA6174" },
    { 0x168C, 0x0042, "Qualcomm Atheros QCA9377" },
    { 0x14E4, 0x43A0, "Broadcom BCM4360" },
    { 0x14E4, 0x43B1, "Broadcom BCM4352" },
};

static void put_hex4(char *out, int at, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        out[at + i] = "0123456789abcdef"[(v >> (12 - 4 * i)) & 0xF];
    out[at + 4] = 0;
}

static struct wifi_info wfi;
static int wifi_pick = -1;          /* the network picked to join */
static void secret_field(int x, int y, int w, int f, const char *empty);

/* Joins the picked network with the password typed; the driver does it
   in its own time, and the page says how it goes (wfi.link) */
static void wifi_join(void)
{
    if (wifi_pick < 0 || wifi_pick >= WIFI_INFO_NETS)
        return;
    struct wifi_join j;
    copy(j.ssid, wfi.net[wifi_pick].ssid, sizeof(j.ssid));
    copy(j.password, fields[F_WIFIPW], sizeof(j.password));
    sys_wifi_join(&j);
    for (int i = 0; i < (int)sizeof(j.password); i++)
        j.password[i] = 0;
    for (int i = 0; i < FIELD_MAX; i++)
        fields[F_WIFIPW][i] = 0;
    wifi_pick = -1;
}

/* How strong, in four bars: the steps phones use, near enough */
static void wifi_bars(int x, int y, int rssi)
{
    int lit = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
    for (int i = 0; i < 4; i++) {
        int h = 4 + 3 * i;
        ud_round(win, x + 5 * i, y + 15 - h, 3, h, 1,
                 i < lit ? l.text : l.line);
    }
}

/* The card the driver has (the Intel Wireless 3160), what it is doing,
   and what the last scan heard - or what 'wifi test' read from its
   recorded frames on a machine with no card. 0 = not this page's
   business: no driver has a card. */
static int draw_wifi_driver(void)
{
    if (sys_wifi(WIFI_INFO, &wfi) != 0 || (!wfi.present && !wfi.recorded))
        return 0;
    static const char *const kind[4] = { "open", "WEP", "WPA", "WPA2" };
    char status[64];
    if (wfi.link == WIFI_LINK_UP)
        cat3(status, sizeof(status), "Joined ", wfi.joined, "");
    else if (wfi.link != WIFI_LINK_DOWN)
        cat3(status, sizeof(status), "Joining ", wfi.joined, "...");
    else
        copy(status, wfi.ready ? "on" : wfi.state, sizeof(status));
    if (wfi.present) {
        info(0, "Card", wfi.card);
        info(1, "Status", status);
        info(2, "Address", wfi.mac[0] ? wfi.mac : "-");
    } else {
        info(0, "Card", "none in this machine");
        info(1, "Networks", "recorded ('wifi test')");
        info(2, "Address", "-");
    }
    int y = PANE_Y0 + 3 * INFO_H + 8;
    if (wfi.scanning)
        ud_text_l(win, PANE_X, ud_baseline(y, CHIP_H, UD_PX),
                  "Searching for networks...", l.dim, UD_SANS, UD_PX);
    else if (wfi.ready)
        button(PANE_X, y, 150, "Search for networks", A_WIFI_SEARCH, 0);
    else if (wfi.present)
        note(y + 4, wfi.error[0] ? wfi.error : "The card is not ready.");
    y = WIFI_LIST_Y;
    if (wfi.found < 0 && !wfi.scanning) {
        note(y, "Search to see the networks around.");
    } else if (wfi.found == 0 && !wfi.scanning) {
        note(y, "No networks found.");
    } else {
        int n = wfi.found < WIFI_INFO_NETS ? wfi.found : WIFI_INFO_NETS;
        int end = ch - NOTE_DY - 14 - (wifi_pick >= 0 ? WIFI_JOIN_H : 0);
        if (wifi_pick >= n)
            wifi_pick = -1;
        for (int i = 0; i < n && y + WIFI_ROW_H < end; i++) {
            const struct wifi_net *w = &wfi.net[i];
            int base = ud_baseline(y, WIFI_ROW_H, UD_PX);
            if (i == wifi_pick)
                ud_ring(win, PANE_X - 6, y, WIFI_PICK_W, WIFI_ROW_H, 4,
                        l.accent);
            hot(PANE_X - 6, y, WIFI_PICK_W, WIFI_ROW_H, A_WIFI_PICK, i,
                (const char *)0, (const char *)0);
            if (w->ssid[0])
                ud_text(win, PANE_X, base, w->ssid, l.text, UD_SANS, UD_PX);
            else
                ud_text_l(win, PANE_X, base, "Hidden network", l.dim,
                          UD_SANS, UD_PX);
            ud_text_l(win, PANE_X + 200, base, kind[w->security & 3], l.dim,
                      UD_SANS, UD_PX);
            wifi_bars(PANE_X + 250, y + 2, w->rssi);
            y += WIFI_ROW_H;
        }
    }
    /* The picked network: its password and Join; a joined one: Leave */
    int jy = ch - NOTE_DY - 14 - WIFI_JOIN_H + 6;
    if (wifi_pick >= 0) {
        int x = PANE_X;
        if (wfi.net[wifi_pick].security != WIFI_SEC_OPEN) {
            secret_field(x, jy, FIELD_W, F_WIFIPW, "Password");
            x += FIELD_W + 10;
        }
        button(x, jy, WIFI_JOIN_W, "Join", A_WIFI_JOIN, 0);
    } else if (wfi.link == WIFI_LINK_UP) {
        button(PANE_X, jy, WIFI_JOIN_W, "Leave", A_WIFI_LEAVE, 0);
    }
    char said[160];
    if (wfi.join_error[0])
        cat3(said, sizeof(said), "Not joined: ", wfi.join_error, "");
    else if (wfi.link == WIFI_LINK_UP)
        cat3(said, sizeof(said), "Joined ", wfi.joined, ".");
    else if (wfi.link != WIFI_LINK_DOWN)
        cat3(said, sizeof(said), "Joining ", wfi.joined, "...");
    else
        copy(said, wifi_pick >= 0 ? "Type its password, then Join."
                                  : "Pick a network to join it.", sizeof(said));
    note(ch - NOTE_DY - 14, said);
    return 1;
}

static void draw_wifi(void)
{
    if (draw_wifi_driver())
        return;
    /* A wireless card is a network controller of the "other" kind,
       class 02, subclass 80 - every one of them, whoever made it */
    struct pci_info p;
    int found = 0;
    for (int i = 0; sys_pci_info(i, &p) == 1 && i < 256; i++) {
        if (((p.class_code >> 24) & 0xFF) != 0x02 ||
            ((p.class_code >> 16) & 0xFF) != 0x80)
            continue;
        found = 1;
        break;
    }
    if (!found) {
        row_name(PANE_Y0, "No Wi-Fi card was found in this machine.");
        note(PANE_Y0 + 22, "A cable in the network port works (Network).");
        return;
    }
    const char *name = "A wireless network card";
    for (unsigned k = 0; k < sizeof(wifi_names) / sizeof(wifi_names[0]); k++)
        if (wifi_names[k].vendor == p.vendor &&
            wifi_names[k].device == p.device)
            name = wifi_names[k].name;
    char id[12];
    put_hex4(id, 0, p.vendor);
    id[4] = ':';
    put_hex4(id, 5, p.device);
    info(0, "Card", name);
    info(1, "PCI ID", id);
    info(2, "Status", "no driver for this card yet");
    int y = PANE_Y0 + 3 * INFO_H + 10;
    note(y, "Wi-Fi cannot be turned on until there is one: the card");
    note(y + 14, "needs its maker's firmware and a driver of its own.");
    note(y + 36, "Until then: the network port, or a USB network adapter.");
}

/* --- Devices ------------------------------------------------------------

   The Device Manager: every device in the machine and what became of
   it, the ones that need something first - a person opens it to find
   out why something does not work, and that is the answer they are
   looking for. A row opens the device on its own: the numbers a driver
   is found by, where it sits, and what its driver said. */

static struct device_info devs[DEV_MAX];
static int ndevs;
static int dev_top;                     /* the first row shown */
static int dev_open = -1;               /* the device on its own, or -1 */

static void load_devices(void)
{
    struct device_info d;
    ndevs = 0;
    for (int i = 0; ndevs < DEV_MAX && i < 512 && sys_device(i, &d) == 1; i++)
        devs[ndevs++] = d;
    /* The most urgent first, and the bus's order within each kind:
       an insertion sort, because it keeps that order */
    for (int i = 1; i < ndevs; i++) {
        struct device_info t = devs[i];
        int j = i;
        while (j > 0 && devs[j - 1].state > t.state) {
            devs[j] = devs[j - 1];
            j--;
        }
        devs[j] = t;
    }
}

static const char *dev_state_text(int s)
{
    switch (s) {
    case DEV_FAILED:    return "Failed";
    case DEV_FIRMWARE:  return "Needs firmware";
    case DEV_NO_DRIVER: return "No driver";
    case DEV_SPARE:     return "Not in use";
    case DEV_STARTING:  return "Starting";
    case DEV_WORKING:   return "Working";
    default:            return "Part of the machine";
    }
}

static uint32_t dev_state_colour(int s)
{
    switch (s) {
    case DEV_FAILED:    return l.danger;
    case DEV_FIRMWARE:
    case DEV_NO_DRIVER: return DEV_AMBER;
    case DEV_STARTING:  return l.accent;
    case DEV_WORKING:   return l.good;
    default:            return l.line;
    }
}

static void put_id(char *out, const struct device_info *d)
{
    put_hex4(out, 0, d->vendor);
    out[4] = ':';
    put_hex4(out, 5, d->device);
    out[9] = 0;
}

static void put_where(char *out, const struct device_info *d)
{
    int n = put_str(out, 0, d->bus == DEV_BUS_PCI ? "PCI " : "USB socket ");
    n = put_num(out, n, d->where[0]);
    if (d->bus == DEV_BUS_PCI) {
        out[n++] = ':';
        n = put_num(out, n, d->where[1]);
        out[n++] = '.';
        n = put_num(out, n, d->where[2]);
    }
}

static void draw_device(const struct device_info *d)
{
    char id[12], where[24];
    put_id(id, d);
    put_where(where, d);
    info(0, "Device", d->name);
    info(1, "Maker:model", id);
    info(2, "Where", where);
    if (d->driver[0])
        info(3, "Driver", d->driver);
    else
        info_l(3, "Driver", "none");
    int base = ud_baseline(PANE_Y0 + 4 * INFO_H, INFO_H, UD_PX);
    ud_text_l(win, PANE_X, base, "Status", l.dim, UD_SANS, UD_PX);
    ud_text_l(win, PANE_X + INFO_X, base, dev_state_text(d->state),
              dev_state_colour(d->state) == l.line ? l.text
                                                   : dev_state_colour(d->state),
              UD_SANS, UD_PX);
    int y = PANE_Y0 + 5 * INFO_H + 6;
    if (d->why[0]) {
        note(y, d->why);
        y += 18;
    }
    if (d->state == DEV_NO_DRIVER) {
        note(y, "ThreadOS has no driver for it yet. The numbers above");
        note(y + 14, "are what a driver would be found by.");
    } else if (d->state == DEV_FIRMWARE) {
        note(y, "Its driver is there; the maker's firmware is not.");
    }
    button(PANE_X, ch - NOTE_DY - CHIP_H - 22, SAVE_W, "Back", A_DEVICE, -1);
}

static void draw_devices(void)
{
    load_devices();
    if (dev_open >= ndevs)
        dev_open = -1;
    if (dev_open >= 0) {
        draw_device(&devs[dev_open]);
        return;
    }
    int need = 0;
    for (int i = 0; i < ndevs; i++)
        if (devs[i].state <= DEV_NO_DRIVER)
            need++;
    /* Two lines of figures, rather than "21 devices": a number
       before its noun takes three forms in Polish */
    char num[12], urgent[12];
    put_num(num, 0, (uint32_t)ndevs);
    put_num(urgent, 0, (uint32_t)need);
    info(0, "Devices", num);
    info(1, "Need attention", urgent);

    int rows = (ch - NOTE_DY - 22 - DEV_LIST_Y) / DEV_ROW_H;
    if (dev_top > ndevs - rows)
        dev_top = ndevs - rows;
    if (dev_top < 0)
        dev_top = 0;
    int w = cw - PANE_X - 16;
    for (int k = 0; k < rows && dev_top + k < ndevs; k++) {
        const struct device_info *d = &devs[dev_top + k];
        int y = DEV_LIST_Y + k * DEV_ROW_H;
        int base = ud_baseline(y, DEV_ROW_H, UD_PX);
        ud_round(win, PANE_X, y + (DEV_ROW_H - 8) / 2, 8, 8, 4,
                 dev_state_colour(d->state));
        ud_clip(win, PANE_X + 14, y, w - 14 - DEV_STATE_W, DEV_ROW_H);
        ud_text(win, PANE_X + 14, base, d->name,
                d->state == DEV_PLUMBING ? l.dim : l.text, UD_SANS, UD_PX);
        ud_unclip(win);
        ud_text_l(win, PANE_X + w - DEV_STATE_W + 4, base,
                  dev_state_text(d->state), l.dim, UD_SANS, UD_PX);
        hot(PANE_X, y, w, DEV_ROW_H, A_DEVICE, dev_top + k, (const char *)0,
            (const char *)0);
    }
    if (ndevs > rows)
        note(DEV_LIST_Y + rows * DEV_ROW_H + 2,
             "The wheel shows the rest.");
}

static struct bt_info bti;
static int bt_pick = -1;           /* the device picked */
static char bt_pick_addr[18];

static void draw_bluetooth(void)
{
    if (sys_bt(BT_INFO, &bti) != 0) {
        row_name(PANE_Y0, "Bluetooth is there in the 64-bit system only.");
        return;
    }
    if (!bti.present) {
        info(0, "Bluetooth", "no adapter");
        /* The reason is a sentence, longer than the value column */
        note(PANE_Y0 + INFO_H + 6, bti.why);
        note(PANE_Y0 + INFO_H + 24,
             "'hardware' in the terminal lists every USB device.");
        return;
    }
    info(0, "Bluetooth", "on");
    info(1, "Address", bti.address);
    info(2, "Name", bti.name[0] ? bti.name : "(none)");
    info(3, "Version", bti.version);
    int y = PANE_Y0 + 4 * INFO_H + 8;
    if (bti.scanning) {
        ud_text_l(win, PANE_X, ud_baseline(y, CHIP_H, UD_PX),
                  "Searching for devices nearby...", l.dim, UD_SANS, UD_PX);
    } else {
        button(PANE_X, y, 150, "Search for devices", A_BT_SEARCH, 0);
    }
    y += CHIP_H + 10;
    if (!bti.found && !bti.scanning) {
        note(y, "No devices found yet.");
        return;
    }
    if (bt_pick >= bti.found)
        bt_pick = -1;
    int end = ch - NOTE_DY - 14 - BT_ASK_H;
    for (int i = 0; i < bti.found && y + BT_ROW_H < end; i++) {
        char rssi[12];
        int v = bti.dev_rssi[i], at = 0;
        if (v < 0) {
            rssi[at++] = '-';
            v = -v;
        }
        put_str(rssi, put_num(rssi, at, (uint32_t)v), " dBm");
        int base = ud_baseline(y, BT_ROW_H, UD_PX);
        if (i == bt_pick)
            ud_ring(win, PANE_X - 6, y, BT_PICK_W, BT_ROW_H, 4, l.accent);
        ud_text(win, PANE_X, base,
                bti.dev_name[i][0] ? bti.dev_name[i] : bti.dev_addr[i],
                l.text, UD_SANS, UD_PX);
        ud_text(win, PANE_X + 220, base, rssi, l.dim, UD_SANS, UD_PX);
        if (bti.dev_paired[i])
            ud_text_l(win, PANE_X + BT_PAIRED_X, base, "Paired", l.dim,
                      UD_SANS, UD_PX);
        hot(PANE_X - 6, y, BT_PICK_W, BT_ROW_H, A_BT_PICK, i,
            (const char *)0, (const char *)0);
        y += BT_ROW_H;
    }
    /* Under the list: the pairing's question, or what can be done with
       the device picked; and a line on how it went */
    int jy = ch - NOTE_DY - 14 - BT_ASK_H + 6;
    char said[160];
    said[0] = 0;
    if (bti.pair_state == BT_PAIR_CONFIRM) {
        char six[8];
        for (int i = 5, v = (int)bti.pair_number; i >= 0; i--, v /= 10)
            six[i] = (char)('0' + v % 10);
        six[6] = 0;
        ud_text(win, PANE_X, ud_baseline(jy, CHIP_H, UD_PX + 6), six, l.text,
                UD_BOLD, UD_PX + 6);
        button(PANE_X + BT_ASK_YES_X, jy, 60, "Yes", A_BT_YES, 0);
        button(PANE_X + BT_ASK_YES_X + 70, jy, 60, "No", A_BT_NO, 0);
        put_str(said, put_str(said, put_str(said, 0, "Does "),
                              bti.pair_name), " show this number?");
    } else if (bti.pair_state == BT_PAIR_CONNECTING ||
               bti.pair_state == BT_PAIR_BONDING) {
        put_str(said, put_str(said, put_str(said, 0, "Pairing with "),
                              bti.pair_name), "...");
    } else {
        if (bt_pick >= 0 && !bti.scanning) {
            if (bti.dev_paired[bt_pick])
                button(PANE_X, jy, 80, "Forget", A_BT_FORGET, 0);
            else
                button(PANE_X, jy, 80, "Pair", A_BT_PAIR, 0);
        }
        if (bti.pair_state == BT_PAIR_PAIRED)
            put_str(said, put_str(said, put_str(said, 0, "Paired with "),
                                  bti.pair_name), ".");
        else if (bti.pair_state == BT_PAIR_FAILED)
            put_str(said, put_str(said, 0, "Not paired: "), bti.pair_why);
    }
    if (said[0])
        note(ch - NOTE_DY - 14, said);
}

/* How the lock opens is the account's own, kept with its password in
   the account file: read from the kernel, changed only with the
   password (USER_CTL_UNLOCK). What is picked waits for Save. */
static int unlock_mode, unlock_has_pin, unlock_pick = -1;
static int unlock_said;            /* 1 saved, -2 wrong password, -3 no PIN */

static void unlock_load(void)
{
    struct user_ctl c = { 0 };
    c.op = USER_CTL_UNLOCK_INFO;
    if (sys_user(&c) == 0) {
        unlock_mode = c.level;
        unlock_has_pin = c.index;
    }
    if (unlock_pick < 0)
        unlock_pick = unlock_mode;
}

static void unlock_save(void)
{
    struct user_ctl c = { 0 };
    c.op = USER_CTL_UNLOCK;
    c.level = unlock_pick;
    copy(c.password, fields[F_PIN], sizeof(c.password));
    copy(c.admin_password, fields[F_LOCKPW], sizeof(c.admin_password));
    int32_t rc = sys_user(&c);
    unlock_said = rc == 0 ? 1 : rc == -2 ? -2 : rc == -3 ? -3 : -1;
    for (int i = 0; i < (int)sizeof(c.password); i++)
        c.password[i] = c.admin_password[i] = 0;
    for (int i = 0; i < CFG_VAL_MAX; i++)
        fields[F_PIN][i] = fields[F_LOCKPW][i] = 0;
    focus = -1;
    unlock_load();
    if (rc == 0)
        unlock_pick = unlock_mode;
}

/* A field whose letters are not shown: a dot for each, or what it is
   for, dimmed, while it is empty */
static void secret_field(int x, int y, int w, int f, const char *empty)
{
    ud_round(win, x, y, w, CHIP_H, 6, l.field);
    ud_ring(win, x, y, w, CHIP_H, 6, focus == f ? l.accent : l.line);
    int n = len(fields[f]), cx = x + 10;
    if (!n && focus != f)
        ud_text_l(win, x + 8, ud_baseline(y, CHIP_H, UD_PX), empty, l.dim,
                  UD_SANS, UD_PX);
    for (int i = 0; i < n && cx < x + w - 10; i++, cx += 9)
        ud_disc(win, cx, y + CHIP_H / 2, 3, l.text);
    if (focus == f)
        ud_fill(win, cx - 2, y + 5, 1, CHIP_H - 10, l.text);
    hot(x, y, w, CHIP_H, A_FOCUS, f, (const char *)0, (const char *)0);
}

static void draw_lockscreen(void)
{
    static const char *const offs[5] = { "Never", "1 min", "5 min", "10 min",
                                         "30 min" };
    static const char *const off_values[5] = { "0", "60", "300", "600",
                                               "1800" };
    int y = PANE_Y0;
    row_name(y, "Turn the screen off when inactive for");
    chips(y + CTRL_DY, 5, offs, off_values, "screenoff");

    y += ROW_H - 2;
    check_box(y, "Dim the screen before it turns off",
              same(setting("screendim"), "1"), "screendim");

    /* When the lock comes: never, as the screen goes dark, or a while
       after. 'lockafter' says it; a machine that only ever had the
       older 'lockoff' switch reads as "never" or "at once". */
    static const char *const after[5] = { "Never", "At once", "1 min",
                                          "5 min", "15 min" };
    static const char *const after_values[5] = { "never", "0", "60", "300",
                                                 "900" };
    const char *now = setting("lockafter");
    if (!now[0])
        now = same(setting("lockoff"), "1") ? "0" : "never";
    y += CHIP_H + 10;
    row_name(y, "Lock the screen after it turns off");
    for (int i = 0; i < 5; i++)
        chip(PANE_X + i * (CHIP_W + CHIP_GAP), y + CTRL_DY, after[i],
             same(now, after_values[i]), "lockafter", after_values[i]);

    unlock_load();
    static const char *const ways[3] = { "Password", "PIN", "Swipe" };
    y += ROW_H - 2;
    row_name(y, "Unlock with");
    for (int i = 0; i < 3; i++) {
        int x = PANE_X + i * (LOCK_WAY_W + CHIP_GAP), cy = y + CTRL_DY;
        ud_round(win, x, cy, LOCK_WAY_W, CHIP_H, 8,
                 unlock_pick == i ? l.accent : l.row);
        ud_text_mid_l(win, x, cy, LOCK_WAY_W, CHIP_H, ways[i],
                      unlock_pick == i ? l.on_accent : l.text, UD_SANS, UD_PX);
        hot(x, cy, LOCK_WAY_W, CHIP_H, A_UNLOCK_PICK, i, (const char *)0,
            (const char *)0);
    }

    y += ROW_H - 2;
    secret_field(PANE_X, y, LOCK_PIN_W, F_PIN,
                 unlock_has_pin ? "New PIN" : "PIN");
    secret_field(PANE_X + LOCK_PIN_W + 8, y, LOCK_PW_W, F_LOCKPW,
                 "Your password");
    button(PANE_X + LOCK_PIN_W + LOCK_PW_W + 16, y, SAVE_W, "Save",
           A_UNLOCK_SAVE, 0);

    y += CHIP_H + 6;
    if (unlock_said == 1)
        note(y, "Saved - the lock opens the new way");
    else if (unlock_said == -2)
        note(y, "NOT changed (wrong password)");
    else if (unlock_said == -3)
        note(y, "NOT changed: a PIN is 4 to 8 digits");
    else if (unlock_said == -1)
        note(y, "NOT changed");
    else if (unlock_pick == 2)
        note(y, "Swipe: anyone at the machine can open it");
    else
        note(y, unlock_has_pin ? "The PIN or the password opens the lock"
                               : "A PIN is 4 to 8 digits; the password still works");
    note(y + 14, "Ctrl+Alt+L locks the screen at once.");
}

static void draw_mouse(void)
{
    static const char *const speeds[5] = { "Slow", "Normal", "Fast", "Faster",
                                           "Fastest" };
    static const char *const speed_values[5] = { "50", "100", "150", "200",
                                                 "300" };
    int y = PANE_Y0;
    row_name(y, "Pointer speed");
    chips(y + CTRL_DY, 5, speeds, speed_values, "pointer");

    y += ROW_H;
    check_box(y, "Natural scrolling", same(setting("natural"), "1"),
              "natural");
    note(y + CHIP_H + 4, "The content follows the fingers, as on a phone;");
    note(y + CHIP_H + 18, "without it, the wheel moves the view.");
    note(y + CHIP_H + 40, "A touchpad moves the same pointer as a mouse.");
}

static void draw_updates(void)
{
    char version[32] = "unknown";
    int64_t n = sys_initrd_read("version", version, sizeof(version) - 1);
    if (n > 0) {
        version[n] = 0;
        for (int i = 0; version[i]; i++)
            if (version[i] == '\n')
                version[i] = 0;
    }
    info(0, "Installed version", version);
    button(PANE_X, PANE_Y0 + INFO_H + 6, 152, "Check for updates",
           A_UPDATE_CHECK, 0);
    note(PANE_Y0 + INFO_H + 38, update_message);
    if (update_available && update_allowed) {
        button(PANE_X, PANE_Y0 + INFO_H + 70, 130, "Install update",
               A_UPDATE_INSTALL, 0);
        note(PANE_Y0 + INFO_H + 102,
             "You will be asked for the admin password.");
        button(PANE_X, PANE_Y0 + INFO_H + 140, 190,
               "Restore previous system", A_UPDATE_ROLLBACK, 0);
        note(PANE_Y0 + INFO_H + 172,
             "The previous system is kept after an update.");
    } else if (update_allowed) {
        button(PANE_X, PANE_Y0 + INFO_H + 70, 190,
               "Restore previous system", A_UPDATE_ROLLBACK, 0);
        note(PANE_Y0 + INFO_H + 102,
             "Available after an update.");
    } else if (update_available) {
        note(PANE_Y0 + INFO_H + 70,
             "Boot the installed system to enable installation.");
    } else if (!update_allowed) {
        note(PANE_Y0 + INFO_H + 70,
             "Install only after booting the installed system.");
    }
}

/* --- the window -------------------------------------------------------- */

static void draw(void)
{
    ud_look(&l);
    load();
    fields_from_settings();
    nhot = 0;
    ulocale_window_title(win, "Settings", "Ustawienia");

    /* One background for both halves, a line between them: the window
       stays the theme's window colour, which is how other programs and
       the desktop's tests tell a window's body from its title bar */
    ud_fill(win, 0, 0, cw, ch, l.bg);
    ud_fill(win, SIDE_W, 12, 1, ch - 24, l.line);
    ud_text_l(win, 16, HEAD_BASE, "Settings", l.text, UD_BOLD, 16);
    for (int i = 0; i < SEC_N; i++) {
        int y = SIDE_Y0 + i * SIDE_H;
        if (i == section)
            ud_round(win, 6, y + 1, SIDE_W - 12, SIDE_H - 2, 6, l.accent);
        /* The square with its picture, the way a Mac marks each one */
        int iy = y + (SIDE_H - ICON) / 2;
        ud_round(win, 12, iy, ICON, ICON, 4, section_colour[i]);
        section_icon(i, 12, iy, section_colour[i]);
        ud_text_l(win, SIDE_TEXT_X, ud_baseline(y, SIDE_H, UD_PX),
                  sections[i], i == section ? l.on_accent : l.text, UD_SANS,
                  UD_PX);
        if (group_ends(i) && i < SEC_N - 1)
            ud_fill(win, 14, y + SIDE_H, SIDE_W - 28, 1, l.line);
        hot(0, y, SIDE_W, SIDE_H, A_SECTION, i, (const char *)0,
            (const char *)0);
    }

    ud_text_l(win, PANE_X, HEAD_BASE, sections[section], l.text, UD_BOLD, 16);
    switch (section) {
    case SEC_WIFI:    draw_wifi(); break;
    case SEC_BT:      draw_bluetooth(); break;
    case SEC_NET:     draw_network(); break;
    case SEC_SOUND:   draw_sound(); break;
    case SEC_GENERAL: draw_general(); break;
    case SEC_LOOK:    draw_look(); break;
    case SEC_DESK:    draw_desk(); break;
    case SEC_DISPLAY: draw_display(); break;
    case SEC_LOCK:    draw_lockscreen(); break;
    case SEC_MOUSE:   draw_mouse(); break;
    case SEC_FILES:   draw_files(); break;
    case SEC_REGION:  draw_region(); break;
    case SEC_DEVICES: draw_devices(); break;
    case SEC_UPDATES: draw_updates(); break;
    default:          draw_about(); break;
    }

    /* What became of the last change - always in the same place, so it
       is not looked for */
    int nb = ch - NOTE_DY;
    if (said == 1)
        ud_text_l(win, PANE_X, nb, "Saved", l.good, UD_BOLD, UD_PX);
    else if (said == 2)
        ud_text_l(win, PANE_X, nb,
                  "Changed until the machine stops (no disk)", l.dim, UD_BOLD,
                  UD_PX);
    else if (said == -1)
        ud_text_l(win, PANE_X, nb, "NOT changed (wrong password)", l.danger,
                  UD_BOLD, UD_PX);
    else if (said == -2)
        ud_text_l(win, PANE_X, nb, "NOT changed (not a value it takes)",
                  l.danger, UD_BOLD, UD_PX);

    if (asking) {
        /* The password: dots, not what was typed - the person needs to
           see that the keys arrive, and nothing more */
        int px = PANE_X + (cw - PANE_X - PASS_W) / 2 - 10;
        int py = (ch - PASS_H) / 2;
        ud_fill_a(win, 0, 0, cw, ch, 0x000000, l.dark ? 110 : 70);
        ud_round(win, px, py, PASS_W, PASS_H, 10, l.field);
        ud_ring(win, px, py, PASS_W, PASS_H, 10, l.line);
        const char *password_message = "Changing settings needs admin";
        if (update_auth_pending == UPDATE_ROLLBACK)
            password_message = "Restoring the previous system needs admin";
        else if (update_auth_pending == UPDATE_INSTALL)
            password_message = "Installing an update needs admin";
        ud_text_l(win, px + 14, py + 24, password_message,
                  l.text, UD_BOLD, UD_PX);
        ud_round(win, px + 14, py + 36, PASS_W - 28, CHIP_H, 6, l.bg);
        ud_ring(win, px + 14, py + 36, PASS_W - 28, CHIP_H, 6, l.accent);
        int x = ud_text_l(win, px + 22,
                          ud_baseline(py + 36, CHIP_H, UD_PX), "Password:",
                          l.dim, UD_SANS, UD_PX) + 8;
        for (int i = 0; i < plen && x < px + PASS_W - 24; i++, x += 9)
            ud_disc(win, x, py + 45, 6, l.text);
        ud_text_l(win, px + 14, py + PASS_H - 14,
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
            dev_open = -1;
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
        case A_BT_SEARCH:
            sys_bt(BT_SEARCH, (struct bt_info *)0);
            break;
        case A_BT_PICK:
            bt_pick = t->arg;
            copy(bt_pick_addr, bti.dev_addr[t->arg], sizeof(bt_pick_addr));
            break;
        case A_BT_PAIR:
            sys_bt_arg(BT_PAIR, (int64_t)(uintptr_t)bt_pick_addr);
            break;
        case A_BT_YES:
        case A_BT_NO:
            sys_bt_arg(BT_ANSWER, t->act == A_BT_YES);
            break;
        case A_BT_FORGET:
            sys_bt_arg(BT_FORGET, (int64_t)(uintptr_t)bt_pick_addr);
            break;
        case A_WIFI_SEARCH:
            sys_wifi(WIFI_SEARCH, (struct wifi_info *)0);
            break;
        case A_WIFI_PICK:
            wifi_pick = t->arg;
            fields[F_WIFIPW][0] = 0;
            if (wfi.net[t->arg].security != WIFI_SEC_OPEN)
                focus = F_WIFIPW;       /* the password is what comes next */
            break;
        case A_WIFI_JOIN:
            wifi_join();
            break;
        case A_WIFI_LEAVE:
            sys_wifi(WIFI_LEAVE, (struct wifi_info *)0);
            break;
        case A_DEVICE:
            dev_open = t->arg;
            break;
        case A_UNLOCK_PICK:
            unlock_pick = t->arg;
            unlock_said = 0;
            break;
        case A_UNLOCK_SAVE:
            unlock_save();
            break;
        case A_UPDATE_CHECK:
            update_start(UPDATE_CHECK);
            break;
        case A_UPDATE_INSTALL:
            if (update_allowed && update_available) {
                if (sys_level() == LEVEL_ADMIN) {
                    update_start(UPDATE_INSTALL);
                } else {
                    update_auth_pending = UPDATE_INSTALL;
                    asking = 1;
                    plen = 0;
                    said = 0;
                }
            }
            break;
        case A_UPDATE_ROLLBACK:
            if (update_allowed) {
                if (sys_level() == LEVEL_ADMIN) {
                    update_start(UPDATE_ROLLBACK);
                } else {
                    update_auth_pending = UPDATE_ROLLBACK;
                    asking = 1;
                    plen = 0;
                    said = 0;
                }
            }
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
            update_auth_pending = 0;
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
        /* a setting's value fits the configuration; a password, 63 */
        int room = focus == F_WIFIPW ? FIELD_MAX : CFG_VAL_MAX;
        if (c == '\n' || c == '\r') {
            int was = focus;
            focus = -1;
            if (was == F_WIFIPW)
                wifi_join();           /* Enter is Join */
            else if (!field_setting[was])
                unlock_save();         /* the lock's two: Enter is Save */
            else
                apply(field_setting[was], fields[was]);
        } else if (c == KEY_COPY) {
            if (n && field_setting[focus])   /* never a secret */
                sys_clip_set(f, (uint32_t)n);
        } else if (c == KEY_PASTE) {
            char got[CLIP_MAX + 1];
            int k = (int)sys_clip_get(got, sizeof(got));
            for (int i = 0; i < k && n < room - 1; i++)
                if (got[i] >= 0x20 && got[i] < 0x7F)
                    f[n++] = got[i];
            f[n] = 0;
        } else if (c == '\b') {
            if (n > 0)
                f[n - 1] = 0;
        } else if (c >= 0x20 && c < 0x7F && n < room - 1) {
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
    update_allowed = sys_update_allowed() == 1;
    win = (int)win_create(W0, H0, ulocale_language_polish() ? "Ustawienia"
                                                           : "Settings");
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
            } else if (ev.type == WEV_SCROLL && section == SEC_DEVICES &&
                       dev_open < 0) {
                dev_top -= ev.val;      /* draw_devices() keeps it in range */
            }
        }
        if (update_poll())
            again = 1;
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
