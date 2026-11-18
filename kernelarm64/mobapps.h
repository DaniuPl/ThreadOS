#ifndef THREADOS_ARM64_MOBAPPS_H
#define THREADOS_ARM64_MOBAPPS_H

#include <stdint.h>

/* What the phone's settings say (Settings and the pulled-down shade
   change them; mobile.c applies them) */
struct mob_prefs {
    int dark;                   /* the apps' dark look */
    int airplane;               /* the network card off */
    int dnd;                    /* do not disturb: notifications kept,
                                   no banner */
    int brightness;             /* 10..100, per cent */
    int accent;                 /* MOB_ACCENTS colours, 0 the first */
    int wallpaper;              /* 0 the aurora, then gradients */
    int round_icons;            /* circles, not rounded squares */
    int silent;                 /* the bell crossed out */
    int volume;                 /* 0..100 */
    int banners;                /* new notifications as banners */
    int lock_notes;             /* the latest on the lock screen */
    int saver;                  /* battery saver */
    int show_pct;               /* the battery's per cent in the bar */
    int nav_buttons;            /* Android's three buttons, not the
                                   iPhone's gestures */
    int clock24;                /* 13:05, not 1:05 PM */
    int size;                   /* the display size: MOB_SIZES, 0 the
                                   largest */
    int theme;                  /* the panels: MOB_THEME_* */
    int flashlight;             /* the torch: no LED on QEMU's virt, a
                                   driver's on a phone that has one */
    int language;               /* what programs are told (the PC's
                                   "language" setting): 0 en, 1 pl */
    int landscape;              /* the screen on its side: a tablet's
                                   from the start, a phone's when
                                   turned */
};

#define MOB_THEME_MATTE  0      /* a tint over what is under them */
#define MOB_THEME_GLASS  1      /* Liquid Glass */
#define MOB_THEME_SOLID  2      /* opaque surfaces */
extern struct mob_prefs mob_prefs;

#define MOB_ACCENTS    5
#define MOB_SIZES      3        /* 200%, 175%, 150% */
#define MOB_WALLPAPERS 4
uint32_t mob_accent(void);
uint32_t mob_accent_n(int i);
extern const char *const mob_wallpaper_names[MOB_WALLPAPERS];

/* The apps' colours in the look the settings choose */
struct mob_theme {
    uint32_t bg, fg, muted, card, line, key;
};
void mob_theme(struct mob_theme *t);
void mob_theme_for(struct mob_theme *t, int dark);  /* light or dark */

/* mobile.c's pieces, for the apps */
void mob_status_bar(uint32_t colour);
void mob_title(const char *s, uint32_t colour);
void mob_pill(uint32_t colour);
int mob_glass_backdrop(void);
int mob_glass_card(int x, int y, int w, int h, int r);
void mob_centred(int font, int px, int cx, int baseline, const char *s,
                 uint32_t colour);
void mob_set_airplane(int on);
void mob_set_brightness(int pct);
/* Brightness and the refresh again, after battery saver changed */
void mob_apply_saver(void);
/* The home and lock screens' picture again */
void mob_wallpaper_changed(void);
/* The display size: everything drawn larger or smaller */
void mob_set_size(int which);
/* Programs' windows to the room the navigation leaves them */
void mob_windows_fit(void);
/* The screen on its side (1) or upright (0) */
void mob_set_orientation(int landscape);
/* The battery as the shell knows it: per cent, or -1 on mains power
   with no battery; *charging set */
int mob_battery(int *charging);
/* The network as the status bar shows it: 0 no card, 1 a card with no
   address yet, 2 connected, 3 off (airplane mode); the address */
int mob_network(char *addr, int len);

/* mobapps.c: each app drawn whole, and a tap at a logical point - 1
   when it changed and is to be drawn again */
void notes_draw(void);
int notes_tap(int x, int y);
void calc_draw(void);
int calc_tap(int x, int y);
void settings_draw(void);
#define SETTINGS_ABOUT 2         /* settings_tap: open About */
int settings_tap(int x, int y);
/* Settings' pages: back to the one above (Android's back button) - 0
   when it is on its first already; and to the first, as it opens */
int settings_back(void);
void settings_reset(void);

/* mobsound.c: the shell's sound - the notification chime (unless the
   phone is silent or not to be disturbed) and the Recorder; what the
   Recorder shows has moved on when rec_changes() has */
void mobsound_init(void);
void mob_chime(void);
int rec_changes(void);
void recorder_draw(void);
int recorder_tap(int x, int y);

/* A program's panel (WIN_DRAW's WD_GLASS) in the look Settings choose,
   on its window's pixels: how light it came out, 0..255 */
struct gfx_surface;
int mob_panel_on(const struct gfx_surface *t, int x, int y, int w, int h,
                 int r);

/* The keyboard on the screen, Notes' and a program's: its top, drawn
   across the bottom, and a tap on it - each key's text to 'emit', "\n"
   for enter and "\b" for backspace; 1 when a key was hit */
int mob_kb_top(void);
void mob_kb_draw(void);
int mob_kb_tap(int x, int y, void (*emit)(const char *s));

/* Text helpers both use */
char *mob_put(char *p, const char *s);
char *mob_put_num(char *p, uint32_t v);

#endif
