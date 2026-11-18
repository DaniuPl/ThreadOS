/* Weft: the web browser on WebKit (WPE WebKit on ThreadOS)
 *
 *   weft [URL]
 *   weft --eval SCRIPT URL
 *   weft --trace URL    (every event from the desktop said too)
 *   weft --proxy http://HOST:PORT URL
 *                       (every page through that proxy: an https://
 *                        page by CONNECT, the rest asked of it by
 *                        their whole address)
 *
 * The UI process of WebKit's three: it opens a window through the
 * ThreadOS platform (apps/webkit/wpethreados.c), and WebKit starts the
 * web process, which lays out and paints pages (wpewebproc), and the
 * network process, which fetches them (wpenetproc), and talks to them
 * over socket pairs.
 *
 * The window is a browser's: tabs across the top, then back, forward,
 * reload and the address field, a thin bar of how far a page has
 * loaded, and the page under them. On a touchscreen - a phone's or a
 * tablet's - it is a mobile browser's instead: the address alone at the
 * top, a toolbar at the bottom (back, forward, home, the tabs, a menu)
 * within the thumb's reach, the tabs side by side as cards, each with
 * its page's picture, behind the toolbar's tabs button, and behind the
 * menu a new tab, the history of the pages visited and the settings -
 * the search engine, the desktop site, dark pages, JavaScript, and
 * clearing the history and the cookies. Each tab is a WebKit view of its
 * own; only the one in front is in the window (wpe_view_set_toplevel),
 * the others are kept, unmapped, as they were. Typing in the address
 * field and pressing Enter goes there - an address without its scheme
 * gets http://, and words are looked up. A link that opens a new window
 * opens a tab. Ctrl+T, Ctrl+W, Ctrl+L and Ctrl+R are a new tab, closing
 * this one, the address field and reload.
 *
 * What happens is said on standard output, a line each, so that the
 * tests can follow it (scripts/tests/t_webkit.py):
 *
 *   [weft] load committed <uri>
 *   [weft] load finished <uri>
 *   [weft] title <title>
 *   [weft] frame shown         (the first frame in the window)
 *   [weft] eval <result>       (--eval: the script's value, as a
 *                               string, once the page has loaded)
 *   [weft] load failed <uri>: <why>
 *   [weft] go <uri>            (from the address field)
 *   [weft] tab <n> of <count>  (the tab in front changed)
 *   [weft] tabs shown, [weft] tabs hidden   (a touchscreen's cards; the
 *                       menu, history and settings the same way)
 *   [weft] history shown, <n> pages
 *   [weft] setting <name> <value>
 *   [weft] user agent <agent>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wpe/webkit.h>
#include "wpethreados.h"
#include "udraw.h"

/* The page's part of the window; small enough, with the band above it,
   for an 800 x 600 screen, which is the tests' (scripts/tests/
   t_webkit.py reads these) */
#define WIDTH  640
#define HEIGHT 400

/* The band: the tabs, the toolbar, the loading bar */
#define TAB_H    26
#define BAR_H    32
#define PROG_H   2
#define CHROME_H 60
#define TAB_W    180         /* a tab at its widest */
#define CLOSE_W  18          /* its close button, at its right end */
#define PLUS_W   26          /* the new-tab button after the tabs */
#define BTN_X    6           /* back, forward, reload: from here, */
#define BTN_W    28          /* this wide, */
#define BTN_STEP 30          /* this far apart */
#define ADDR_X   100         /* the address field */
#define ADDR_H   24
#define TABS_MAX 8

/* On a touchscreen (scripts/tests/t_arm64weft.py reads these) */
#define M_TOP_H    56        /* the address band */
#define M_PILL_X   12        /* the address in a rounded field */
#define M_PILL_Y   8
#define M_PILL_H   40
#define M_TEXT_PX  15
#define M_BOT_H    52        /* the toolbar: */
#define M_BUTTONS  5         /* back, forward, home, tabs, the menu */
#define M_HEAD_H   52        /* the tabs' screen: its heading, */
#define M_GAP      12        /* cards in columns, this far apart, */
#define M_CARD_W   170       /* at least this wide - two on a phone, */
#define M_CARD_H   190       /* this tall, */
#define M_CARD_BAR 30        /* their titles above their pictures */
#define TABLET_W   600       /* a touchscreen this wide is a tablet's */

/* How Weft names itself to sites: ThreadOS, and on a touchscreen what
   a site needs to send its newest pages for one - "Android 10", the
   version Android's browsers all give since they stopped giving their
   own, and "Mobile" on a phone's (a tablet's is without, and is sent
   the pages for a tablet). An agent without them is sent pages for
   phones of twenty years ago. */
#define THREADOS_VERSION "1.0"
#define AGENT_ENGINE "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0"
#if defined(__aarch64__)
#define AGENT_ARCH "aarch64"
#else
#define AGENT_ARCH "x86_64"
#endif

struct tab {
    WebKitWebView *view;
    char title[96];
    double progress;
    gboolean loading;
};

static GMainLoop *loop;
static const char *eval_script;
static int frame_said;

static WPEDisplay *display;
static WPEToplevel *toplevel;
static WebKitSettings *settings;
static WebKitNetworkSession *session;

static struct tab *tabs[TABS_MAX];
static int ntabs, front;

/* The address field, while it has the keys */
static gboolean editing, replace_all;
static char typed[1024];

static gboolean pressed_on_band;

/* A touchscreen, a phone's or a tablet's, and a mobile browser on it;
   what is over the page - the tabs' cards, the menu, the history, the
   settings - scrolled so far */
static gboolean touch, mobile;
enum { SHEET_NONE, SHEET_CARDS, SHEET_MENU, SHEET_HISTORY, SHEET_SETTINGS,
       SHEET_ENGINES };
static int sheet;
#define cards (sheet == SHEET_CARDS)
static int cards_scroll, press_x, press_y, press_scroll;
static gboolean press_moved;

/* The pages visited, newest first: the history (in memory - the phone
   has no disk of its own to keep it on yet) */
#define HISTORY_MAX 100
static struct visit {
    char title[96];
    char uri[512];
} history[HISTORY_MAX];
static int nhistory;

/* The browser's settings */
static const struct engine {
    const char *name, *query;
} engines[] = {
    { "DuckDuckGo", "https://duckduckgo.com/html/?q=" },
    { "Google", "https://www.google.com/search?q=" },
    { "Bing", "https://www.bing.com/search?q=" },
    { "Brave Search", "https://search.brave.com/search?q=" },
    { "Ecosia", "https://www.ecosia.org/search?q=" },
    { "Startpage", "https://www.startpage.com/do/search?q=" },
};
#define ENGINES ((int)(sizeof(engines) / sizeof(engines[0])))
static struct {
    int engine;
    gboolean desktop_site, dark, no_js;
} prefs;

static void draw_band(void);
static struct tab *tab_new(const char *uri, WebKitWebView *related);
static void tab_front(int i);
static void tab_close(int i);

/* --- What the tests follow ----------------------------------------- */

static void say(const char *fmt, const char *a, const char *b)
{
    printf(fmt, a ? a : "", b ? b : "");
    fflush(stdout);
}

static void evaluated(GObject *object, GAsyncResult *result, gpointer data)
{
    (void)data;
    GError *error = NULL;
    JSCValue *value = webkit_web_view_evaluate_javascript_finish(
        WEBKIT_WEB_VIEW(object), result, &error);
    if (!value) {
        say("[weft] eval failed: %s%s\n", error->message, NULL);
        g_error_free(error);
    } else {
        char *s = jsc_value_to_string(value);
        say("[weft] eval %s%s\n", s, NULL);
        g_free(s);
        g_object_unref(value);
    }
}

/* The first frame on the screen is said once: from then on the window
   shows the page */
static gboolean watch_frames(gpointer data)
{
    (void)data;
    if (!frame_said && wpe_threados_frames_shown() > 0) {
        frame_said = 1;
        say("[weft] frame shown%s%s\n", NULL, NULL);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* --- Tabs and their pages ------------------------------------------ */

static int index_of(WebKitWebView *view)
{
    for (int i = 0; i < ntabs; i++)
        if (tabs[i]->view == view)
            return i;
    return -1;
}

/* What the address field shows of an address: nothing for a page that
   has none of its own (a new tab's) */
static const char *shown_uri(WebKitWebView *view)
{
    const char *uri = webkit_web_view_get_uri(view);
    if (!uri || !strcmp(uri, "about:blank"))
        return "";
    return uri;
}

/* A page into the history, at its top: once, however often visited */
static void remember(const char *uri, const char *title)
{
    if (!uri || !*uri || !strncmp(uri, "about:", 6) ||
        !strncmp(uri, "data:", 5))
        return;
    int i = 0;
    while (i < nhistory && strcmp(history[i].uri, uri))
        i++;
    if (i == nhistory && nhistory < HISTORY_MAX)
        nhistory++;
    if (i == HISTORY_MAX)
        i = HISTORY_MAX - 1;
    memmove(&history[1], &history[0], (size_t)i * sizeof(history[0]));
    g_strlcpy(history[0].uri, uri, sizeof(history[0].uri));
    g_strlcpy(history[0].title, title ? title : "", sizeof(history[0].title));
}

static void load_changed(WebKitWebView *view, WebKitLoadEvent event,
                         gpointer data)
{
    struct tab *t = data;
    const char *uri = webkit_web_view_get_uri(view);
    t->loading = event != WEBKIT_LOAD_FINISHED;
    if (event == WEBKIT_LOAD_COMMITTED) {
        say("[weft] load committed %s%s\n", uri, NULL);
    } else if (event == WEBKIT_LOAD_FINISHED) {
        say("[weft] load finished %s%s\n", uri, NULL);
        remember(uri, t->title);
        if (eval_script && index_of(view) == 0)
            webkit_web_view_evaluate_javascript(view, eval_script, -1, NULL,
                                                NULL, NULL, evaluated, NULL);
    }
    draw_band();
}

static gboolean load_failed(WebKitWebView *view, WebKitLoadEvent event,
                            const char *uri, GError *error, gpointer data)
{
    (void)view;
    (void)event;
    (void)data;
    say("[weft] load failed %s: %s\n", uri, error->message);
    return FALSE;                    /* WebKit shows its error page */
}

static void title_changed(WebKitWebView *view, GParamSpec *pspec,
                          gpointer data)
{
    (void)pspec;
    struct tab *t = data;
    const char *title = webkit_web_view_get_title(view);
    g_strlcpy(t->title, title && *title ? title : "", sizeof(t->title));
    /* a page names itself after it has loaded: its visit takes it */
    const char *uri = webkit_web_view_get_uri(view);
    for (int i = 0; uri && i < nhistory; i++)
        if (!strcmp(history[i].uri, uri))
            g_strlcpy(history[i].title, t->title, sizeof(history[i].title));
    if (title && *title)
        say("[weft] title %s%s\n", title, NULL);
    if (index_of(view) == front)
        wpe_toplevel_set_title(toplevel, *t->title ? t->title : "Weft");
    draw_band();
}

static void progress_changed(WebKitWebView *view, GParamSpec *pspec,
                             gpointer data)
{
    (void)pspec;
    struct tab *t = data;
    t->progress = webkit_web_view_get_estimated_load_progress(view);
    if (index_of(view) == front)
        draw_band();
}

static void uri_changed(WebKitWebView *view, GParamSpec *pspec,
                        gpointer data)
{
    (void)pspec;
    (void)data;
    if (index_of(view) == front && !editing)
        draw_band();
}

/* window.close() from the page: its tab goes */
static void page_closed(WebKitWebView *view, gpointer data)
{
    (void)data;
    int i = index_of(view);
    if (i >= 0)
        tab_close(i);
}

/* A link or a script that opens a new window: a new tab, related to
   this one (the same session, and window.opener), which WebKit loads */
static void new_tab_ready(WebKitWebView *view, gpointer data)
{
    (void)data;
    int i = index_of(view);
    if (i >= 0)
        tab_front(i);
}

static WebKitWebView *page_wants_window(WebKitWebView *view,
                                        WebKitNavigationAction *action,
                                        gpointer data)
{
    (void)action;
    (void)data;
    struct tab *t = tab_new(NULL, view);
    if (!t)
        return NULL;
    g_signal_connect(t->view, "ready-to-show", G_CALLBACK(new_tab_ready),
                     NULL);
    return t->view;
}

static const char start_page[] =
    "<!doctype html><html><head><meta charset=utf-8><title>New tab</title>"
    "<style>body{margin:0;height:100vh;display:flex;align-items:center;"
    "justify-content:center;font-family:sans-serif;color:#555;"
    "background:#f4f4f6}h1{font-weight:normal;font-size:28px;margin:0}"
    "p{margin:8px 0 0;font-size:13px;text-align:center}</style></head>"
    "<body><div><h1>Weft</h1><p>Type an address or words above</p>"
    "</div></body></html>";

static struct tab *tab_new(const char *uri, WebKitWebView *related)
{
    if (ntabs == TABS_MAX)
        return NULL;
    struct tab *t = g_new0(struct tab, 1);
    /* A related view shares its parent's session; naming one as well
       would be two answers to one question */
    t->view = WEBKIT_WEB_VIEW(related
        ? g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display,
                       "settings", settings, "related-view", related, NULL)
        : g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display,
                       "settings", settings, "network-session", session,
                       NULL));
    g_signal_connect(t->view, "load-changed", G_CALLBACK(load_changed), t);
    g_signal_connect(t->view, "load-failed", G_CALLBACK(load_failed), t);
    g_signal_connect(t->view, "notify::title", G_CALLBACK(title_changed), t);
    g_signal_connect(t->view, "notify::estimated-load-progress",
                     G_CALLBACK(progress_changed), t);
    g_signal_connect(t->view, "notify::uri", G_CALLBACK(uri_changed), t);
    g_signal_connect(t->view, "close", G_CALLBACK(page_closed), t);
    g_signal_connect(t->view, "create", G_CALLBACK(page_wants_window), t);
    tabs[ntabs++] = t;
    if (uri)
        webkit_web_view_load_uri(t->view, uri);
    else if (!related)
        webkit_web_view_load_html(t->view, start_page, NULL);
    return t;
}

static void tab_front(int i)
{
    if (i < 0 || i >= ntabs)
        return;
    if (front < ntabs && front != i)
        wpe_view_set_toplevel(webkit_web_view_get_wpe_view(tabs[front]->view),
                              NULL);
    front = i;
    wpe_view_set_toplevel(webkit_web_view_get_wpe_view(tabs[i]->view),
                          toplevel);
    wpe_toplevel_set_title(toplevel, *tabs[i]->title ? tabs[i]->title : "Weft");
    editing = FALSE;
    char n[16], count[16];
    g_snprintf(n, sizeof(n), "%d", i + 1);
    g_snprintf(count, sizeof(count), "%d", ntabs);
    say("[weft] tab %s of %s\n", n, count);
    draw_band();
}

static void tab_close(int i)
{
    if (i < 0 || i >= ntabs)
        return;
    if (ntabs == 1) {                /* the last tab: the window goes */
        g_main_loop_quit(loop);
        return;
    }
    struct tab *t = tabs[i];
    wpe_view_set_toplevel(webkit_web_view_get_wpe_view(t->view), NULL);
    memmove(&tabs[i], &tabs[i + 1], (size_t)(ntabs - i - 1) * sizeof(tabs[0]));
    ntabs--;
    g_signal_handlers_disconnect_by_data(t->view, t);
    g_object_unref(t->view);
    g_free(t);
    /* The one in front stays in front; closing it brings its right-hand
       neighbour, or the last tab */
    if (i < front)
        front--;
    else if (i == front) {
        front = MIN(i, ntabs - 1);
        wpe_view_set_toplevel(
            webkit_web_view_get_wpe_view(tabs[front]->view), toplevel);
    }
    tab_front(front);
}

/* --- The address field --------------------------------------------- */

/* What was typed, as an address: one with a scheme as it is, a host
   (it has a dot or a port, and no spaces) with http://, anything else
   looked up */
static char *address_of(const char *s)
{
    while (*s == ' ')
        s++;
    if (!*s)
        return NULL;
    if (strstr(s, "://") || !strncmp(s, "about:", 6) ||
        !strncmp(s, "data:", 5))
        return g_strdup(s);
    if (!strchr(s, ' ') && (strchr(s, '.') || strchr(s, ':') ||
                            !strncmp(s, "localhost", 9)))
        return g_strconcat("http://", s, NULL);
    char *q = g_uri_escape_string(s, NULL, FALSE);
    char *uri = g_strconcat(engines[prefs.engine].query, q, NULL);
    g_free(q);
    return uri;
}

static void address_go(void)
{
    char *uri = address_of(typed);
    editing = FALSE;
    if (uri) {
        say("[weft] go %s%s\n", uri, NULL);
        webkit_web_view_load_uri(tabs[front]->view, uri);
        g_free(uri);
    }
    draw_band();
}

static void address_edit(void)
{
    editing = TRUE;
    replace_all = TRUE;              /* the first key replaces it all */
    g_strlcpy(typed, shown_uri(tabs[front]->view), sizeof(typed));
    draw_band();
}

/* One byte the desktop typed into the field: UTF-8 bytes are kept as
   they come (a Polish letter is two), backspace takes a whole
   character */
static void address_key(unsigned char c)
{
    size_t n = strlen(typed);
    if (c == '\n' || c == '\r') {
        address_go();
        return;
    }
    if (c == 0x1B) {                 /* Esc: as it was */
        editing = FALSE;
        draw_band();
        return;
    }
    if (replace_all && (c == '\b' || c >= 0x20)) {
        typed[0] = 0;
        n = 0;
        replace_all = FALSE;
        if (c == '\b') {
            draw_band();
            return;
        }
    }
    if (c == '\b') {
        while (n > 0 && ((unsigned char)typed[n - 1] & 0xC0) == 0x80)
            n--;
        if (n > 0)
            n--;
        typed[n] = 0;
    } else if (c >= 0x20 && c != 0x7F && n + 1 < sizeof(typed)) {
        typed[n] = (char)c;
        typed[n + 1] = 0;
    }
    draw_band();
}

/* --- The band ------------------------------------------------------ */

static int win_w(void)
{
    int w, h;
    wpe_threados_toplevel_size(toplevel, &w, &h);
    return w;
}

static int tab_width(void)
{
    int room = win_w() - PLUS_W - 8;
    int w = ntabs ? room / ntabs : TAB_W;
    return MIN(w, TAB_W);
}

static void cross(int win, int x, int y, int d, uint32_t rgb)
{
    ud_line(win, x, y, x + d, y + d, 1, rgb);
    ud_line(win, x + d, y, x, y + d, 1, rgb);
}

static void arrow(int win, int x, int y, int left, uint32_t rgb)
{
    /* a chevron in a BTN_W x BAR_H button */
    int cx = x + BTN_W / 2, cy = y + BAR_H / 2, d = 6;
    int tip = left ? cx - d / 2 : cx + d / 2;
    int back = left ? cx + d / 2 : cx - d / 2;
    ud_line(win, back, cy - d, tip, cy, 2, rgb);
    ud_line(win, tip, cy, back, cy + d, 2, rgb);
}

static void draw_mobile(void);

static void draw_band(void)
{
    int win = wpe_threados_toplevel_window(toplevel);
    if (win < 0 || !ntabs)
        return;
    if (mobile) {
        draw_mobile();
        return;
    }
    int w = win_w();
    struct ud_look l;
    ud_look(&l);

    /* The tabs */
    ud_fill(win, 0, 0, w, TAB_H, l.line);
    int tw = tab_width();
    for (int i = 0; i < ntabs; i++) {
        int x = 4 + i * tw;
        uint32_t bg = i == front ? l.bg : l.row;
        ud_round(win, x, 3, tw - 2, TAB_H - 3, 6, bg);
        ud_fill(win, x, TAB_H - 6, tw - 2, 6, bg);
        const char *title = *tabs[i]->title ? tabs[i]->title
                          : *shown_uri(tabs[i]->view) ? shown_uri(tabs[i]->view)
                          : "New tab";
        ud_clip(win, x + 8, 3, tw - 10 - CLOSE_W, TAB_H - 3);
        ud_text(win, x + 8, ud_baseline(3, TAB_H - 3, UD_PX), title,
                i == front ? l.text : l.dim, UD_SANS, UD_PX);
        ud_unclip(win);
        cross(win, x + tw - 2 - CLOSE_W + 5, 3 + (TAB_H - 3) / 2 - 3, 6,
              l.dim);
    }
    int px = 4 + ntabs * tw + 4;             /* the new-tab button */
    ud_line(win, px + 7, TAB_H / 2 + 1, px + 17, TAB_H / 2 + 1, 2, l.text);
    ud_line(win, px + 12, TAB_H / 2 - 4, px + 12, TAB_H / 2 + 6, 2, l.text);

    /* The toolbar */
    WebKitWebView *v = tabs[front]->view;
    ud_fill(win, 0, TAB_H, w, BAR_H, l.bg);
    arrow(win, BTN_X, TAB_H, 1,
          webkit_web_view_can_go_back(v) ? l.text : l.line);
    arrow(win, BTN_X + BTN_STEP, TAB_H, 0,
          webkit_web_view_can_go_forward(v) ? l.text : l.line);
    int rx = BTN_X + 2 * BTN_STEP + BTN_W / 2, ry = TAB_H + BAR_H / 2;
    if (tabs[front]->loading) {
        cross(win, rx - 5, ry - 5, 10, l.text);     /* stop */
    } else {
        ud_ring(win, rx - 7, ry - 7, 14, 14, 7, l.text);   /* reload */
        ud_fill(win, rx + 2, ry - 8, 6, 6, l.bg);
        ud_line(win, rx + 2, ry - 7, rx + 7, ry - 7, 2, l.text);
        ud_line(win, rx + 7, ry - 7, rx + 7, ry - 2, 2, l.text);
    }

    int ax = ADDR_X, ay = TAB_H + (BAR_H - ADDR_H) / 2;
    int aw = w - ADDR_X - 8;
    ud_round(win, ax, ay, aw, ADDR_H, 6, l.field);
    ud_ring(win, ax, ay, aw, ADDR_H, 6, editing ? l.accent : l.line);
    const char *text = editing ? typed : shown_uri(v);
    int tx = ax + 10, room = aw - 20;
    int width = ud_width(text, UD_SANS, UD_PX);
    if (width > room)
        tx -= width - room;              /* the end of it, where one types */
    ud_clip(win, ax + 6, ay, aw - 12, ADDR_H);
    if (editing && replace_all && *text) {
        ud_fill(win, ax + 8, ay + 4, MIN(width, room) + 4, ADDR_H - 8,
                l.accent);
        ud_text(win, tx, ud_baseline(ay, ADDR_H, UD_PX), text, l.on_accent,
                UD_SANS, UD_PX);
    } else {
        int end = ud_text(win, tx, ud_baseline(ay, ADDR_H, UD_PX), text,
                          l.text, UD_SANS, UD_PX);
        if (editing)
            ud_fill(win, *text ? end + 1 : tx, ay + 5, 1, ADDR_H - 10,
                    l.text);
    }
    ud_unclip(win);

    /* How far the page has loaded */
    ud_fill(win, 0, TAB_H + BAR_H, w, PROG_H, l.line);
    if (tabs[front]->loading)
        ud_fill(win, 0, TAB_H + BAR_H, (int)(w * tabs[front]->progress),
                PROG_H, l.accent);
}

static void band_click(int x, int y, int middle)
{
    if (y < TAB_H) {
        int tw = tab_width();
        int i = (x - 4) / tw;
        if (x >= 4 && i < ntabs) {
            if (middle || x >= 4 + i * tw + tw - 2 - CLOSE_W)
                tab_close(i);
            else
                tab_front(i);
            return;
        }
        int px = 4 + ntabs * tw + 4;
        if (!middle && x >= px && x < px + PLUS_W) {
            tab_new(NULL, NULL);
            tab_front(ntabs - 1);
            address_edit();
        }
        return;
    }
    if (middle)
        return;
    WebKitWebView *v = tabs[front]->view;
    if (x >= ADDR_X) {
        address_edit();
        return;
    }
    editing = FALSE;
    if (x >= BTN_X && x < BTN_X + BTN_W)
        webkit_web_view_go_back(v);
    else if (x >= BTN_X + BTN_STEP && x < BTN_X + BTN_STEP + BTN_W)
        webkit_web_view_go_forward(v);
    else if (x >= BTN_X + 2 * BTN_STEP && x < BTN_X + 2 * BTN_STEP + BTN_W) {
        if (tabs[front]->loading)
            webkit_web_view_stop_loading(v);
        else
            webkit_web_view_reload(v);
    }
    draw_band();
}

/* --- On a touchscreen ------------------------------------------------ */

static int win_h(void)
{
    int w, h;
    wpe_threados_toplevel_size(toplevel, &w, &h);
    return h;
}

/* An address as a phone's browser shows it: no scheme, no "www.", no
   lone "/" after the host */
static const char *short_uri(const char *uri, char *out, size_t room)
{
    const char *s = strstr(uri, "://");
    s = s ? s + 3 : uri;
    if (!strncmp(s, "www.", 4))
        s += 4;
    g_strlcpy(out, s, room);
    char *slash = strchr(out, '/');
    if (slash && !slash[1])
        *slash = 0;
    return out;
}

static int button_x(int i)
{
    return win_w() * (2 * i + 1) / (2 * M_BUTTONS);
}

static void chevron(int win, int cx, int cy, int left, uint32_t rgb)
{
    int d = 7;
    int tip = left ? cx - d / 2 : cx + d / 2;
    int back = left ? cx + d / 2 : cx - d / 2;
    ud_line(win, back, cy - d, tip, cy, 32, rgb);
    ud_line(win, tip, cy, back, cy + d, 32, rgb);
}

/* As many columns as the width has room for, two at least */
static int card_cols(void)
{
    return MAX(2, (win_w() - M_GAP) / (M_CARD_W + M_GAP));
}

static void card_box(int i, int *x, int *y, int *w)
{
    int n = card_cols();
    int cw = (win_w() - (n + 1) * M_GAP) / n;
    *x = M_GAP + (i % n) * (cw + M_GAP);
    *y = M_HEAD_H + (i / n) * (M_CARD_H + M_GAP) - cards_scroll;
    *w = cw;
}

static int cards_room(void)
{
    int rows = (ntabs + card_cols() - 1) / card_cols();
    int all = rows * (M_CARD_H + M_GAP);
    int room = win_h() - M_HEAD_H - M_BOT_H;
    return all > room ? all - room : 0;
}

static void draw_cards(int win, const struct ud_look *l)
{
    int w = win_w(), h = win_h();
    ud_fill(win, 0, 0, w, h, l->line);
    for (int i = 0; i < ntabs; i++) {
        int x, y, cw;
        card_box(i, &x, &y, &cw);
        if (y + M_CARD_H <= M_HEAD_H || y >= h - M_BOT_H)
            continue;
        if (i == front)
            ud_round(win, x - 3, y - 3, cw + 6, M_CARD_H + 6, 14, l->accent);
        ud_round(win, x, y, cw, M_CARD_H, 12, l->bg);
        const char *title = *tabs[i]->title ? tabs[i]->title
                          : *shown_uri(tabs[i]->view) ? shown_uri(tabs[i]->view)
                          : "New tab";
        ud_clip(win, x + 10, y, cw - 40, M_CARD_BAR);
        ud_text(win, x + 10, ud_baseline(y, M_CARD_BAR, UD_PX), title,
                l->text, UD_BOLD, UD_PX);
        ud_unclip(win);
        ud_line(win, x + cw - 22, y + M_CARD_BAR / 2 - 5, x + cw - 12,
                y + M_CARD_BAR / 2 + 5, 24, l->dim);
        ud_line(win, x + cw - 12, y + M_CARD_BAR / 2 - 5, x + cw - 22,
                y + M_CARD_BAR / 2 + 5, 24, l->dim);
        int tx = x + 4, ty = y + M_CARD_BAR, tw = cw - 8;
        int th = M_CARD_H - M_CARD_BAR - 4;
        if (!wpe_threados_thumbnail(toplevel,
                                    webkit_web_view_get_wpe_view(tabs[i]->view),
                                    tx, ty, tw, th))
            ud_fill(win, tx, ty, tw, th, l->field);
    }
    /* the heading and the toolbar over whatever scrolled under them */
    ud_fill(win, 0, 0, w, M_HEAD_H, l->line);
    char head[32];
    g_snprintf(head, sizeof(head), ntabs == 1 ? "%d tab" : "%d tabs", ntabs);
    ud_text(win, M_GAP + 4, ud_baseline(0, M_HEAD_H, 17), head, l->text,
            UD_BOLD, 17);
    int y0 = h - M_BOT_H;
    ud_fill(win, 0, y0, w, M_BOT_H, l->bg);
    ud_fill(win, 0, y0, w, 1, l->line);
    int cy = y0 + M_BOT_H / 2;
    ud_line(win, w / 2 - 9, cy, w / 2 + 9, cy, 32, l->text);
    ud_line(win, w / 2, cy - 9, w / 2, cy + 9, 32, l->text);
    int dw = ud_width("Done", UD_BOLD, M_TEXT_PX);
    ud_text(win, w - M_GAP - 8 - dw, ud_baseline(y0, M_BOT_H, M_TEXT_PX),
            "Done", l->accent, UD_BOLD, M_TEXT_PX);
}

static void draw_menu(int win, const struct ud_look *l);
static void draw_history(int win, const struct ud_look *l);
static void draw_settings(int win, const struct ud_look *l);
static void draw_engines(int win, const struct ud_look *l);

static void draw_mobile(void)
{
    int win = wpe_threados_toplevel_window(toplevel);
    struct ud_look l;
    ud_look(&l);
    /* The address being typed: the phone's keyboard comes up for it */
    static gboolean keyboard;
    if (editing != keyboard) {
        keyboard = editing;
        win_keyboard(win, keyboard);
    }
    switch (sheet) {
    case SHEET_CARDS:    draw_cards(win, &l); return;
    case SHEET_MENU:     draw_menu(win, &l); return;
    case SHEET_HISTORY:  draw_history(win, &l); return;
    case SHEET_SETTINGS: draw_settings(win, &l); return;
    case SHEET_ENGINES:  draw_engines(win, &l); return;
    }
    int w = win_w(), h = win_h();
    WebKitWebView *v = tabs[front]->view;

    /* The address, in its field */
    ud_fill(win, 0, 0, w, M_TOP_H, l.bg);
    int pw = w - 2 * M_PILL_X;
    ud_round(win, M_PILL_X, M_PILL_Y, pw, M_PILL_H, M_PILL_H / 2, l.field);
    if (editing)
        ud_ring(win, M_PILL_X, M_PILL_Y, pw, M_PILL_H, M_PILL_H / 2,
                l.accent);
    char shown[256];
    const char *text = editing ? typed
                     : short_uri(shown_uri(v), shown, sizeof(shown));
    int tx = M_PILL_X + 16, room = pw - 16 - 44;
    int width = ud_width(text, UD_SANS, M_TEXT_PX);
    if (editing && width > room)
        tx -= width - room;
    int base = ud_baseline(M_PILL_Y, M_PILL_H, M_TEXT_PX);
    ud_clip(win, M_PILL_X + 12, M_PILL_Y, room + 4, M_PILL_H);
    if (!*text && !editing)
        ud_text(win, tx, base, "Search or type an address", l.dim, UD_SANS,
                M_TEXT_PX);
    int end = ud_text(win, tx, base, text, l.text, UD_SANS, M_TEXT_PX);
    if (editing)
        ud_fill(win, *text ? end + 1 : tx, M_PILL_Y + 10, 2, M_PILL_H - 20,
                l.accent);
    ud_unclip(win);
    /* reload, or stop while it loads, at the field's end */
    int rx = M_PILL_X + pw - 24, ry = M_PILL_Y + M_PILL_H / 2;
    if (tabs[front]->loading) {
        ud_line(win, rx - 6, ry - 6, rx + 6, ry + 6, 28, l.text);
        ud_line(win, rx + 6, ry - 6, rx - 6, ry + 6, 28, l.text);
    } else {
        ud_ring(win, rx - 8, ry - 8, 16, 16, 8, l.text);
        ud_fill(win, rx + 2, ry - 9, 8, 7, l.field);
        ud_line(win, rx + 2, ry - 8, rx + 8, ry - 8, 24, l.text);
        ud_line(win, rx + 8, ry - 8, rx + 8, ry - 2, 24, l.text);
    }
    ud_fill(win, 0, M_TOP_H - 2, w, 2, l.bg);
    if (tabs[front]->loading)
        ud_fill(win, 0, M_TOP_H - 2, (int)(w * tabs[front]->progress), 2,
                l.accent);

    /* The toolbar */
    int y0 = h - M_BOT_H, cy = y0 + M_BOT_H / 2;
    ud_fill(win, 0, y0, w, M_BOT_H, l.bg);
    ud_fill(win, 0, y0, w, 1, l.line);
    chevron(win, button_x(0), cy, 1,
            webkit_web_view_can_go_back(v) ? l.text : l.line);
    chevron(win, button_x(1), cy, 0,
            webkit_web_view_can_go_forward(v) ? l.text : l.line);
    int hx = button_x(2);                         /* home: a house */
    ud_line(win, hx - 9, cy - 1, hx, cy - 9, 28, l.text);
    ud_line(win, hx, cy - 9, hx + 9, cy - 1, 28, l.text);
    ud_line(win, hx - 7, cy - 2, hx - 7, cy + 8, 28, l.text);
    ud_line(win, hx + 7, cy - 2, hx + 7, cy + 8, 28, l.text);
    ud_line(win, hx - 7, cy + 8, hx + 7, cy + 8, 28, l.text);
    int bx = button_x(3);                         /* the tabs, counted */
    ud_ring(win, bx - 11, cy - 11, 22, 22, 5, l.text);
    ud_ring(win, bx - 10, cy - 10, 20, 20, 4, l.text);
    char count[8];
    g_snprintf(count, sizeof(count), "%d", ntabs);
    ud_text_mid(win, bx - 11, cy - 11, 22, 22, count, l.text, UD_BOLD, 12);
    int nx = button_x(4);                         /* the menu: three dots */
    for (int i = -1; i <= 1; i++)
        ud_disc(win, nx - 2, cy + i * 7 - 2, 5, l.text);
}

/* What is over the page: the page is covered - not drawn, not touched
   - while anything is */
static void show_sheet(int which)
{
    static const char *const names[] = { "page", "tabs", "menu", "history",
                                         "settings", "search engines" };
    if (sheet == which)
        return;
    int was = sheet;
    sheet = which;
    cards_scroll = 0;
    editing = FALSE;
    wpe_threados_toplevel_cover(toplevel, which != SHEET_NONE);
    if (was != SHEET_NONE)
        say("[weft] %s hidden%s\n", names[was], NULL);
    if (which == SHEET_HISTORY) {
        char n[16];
        g_snprintf(n, sizeof(n), "%d", nhistory);
        say("[weft] history shown, %s pages%s\n", n, NULL);
    } else if (which != SHEET_NONE) {
        say("[weft] %s shown%s\n", names[which], NULL);
    }
    draw_band();
}

static void cards_show(gboolean on)
{
    show_sheet(on ? SHEET_CARDS : SHEET_NONE);
}

/* --- The menu, the history and the settings ------------------------- */

#define MENU_ITEMS 3         /* a new tab, the history, the settings */
#define LIST_ROW   56        /* the menu's, the history's and the settings'
                                rows, under the heading (t_arm64weft.py
                                reads these) */
#define SETTINGS_ROWS 5

static const char *const menu_items[MENU_ITEMS] = {
    "New tab", "History", "Settings",
};

static void draw_header(int win, const struct ud_look *l, const char *title)
{
    int w = win_w();
    ud_fill(win, 0, 0, w, M_HEAD_H, l->bg);
    ud_fill(win, 0, M_HEAD_H - 1, w, 1, l->line);
    ud_text(win, M_GAP + 4, ud_baseline(0, M_HEAD_H, 17), title, l->text,
            UD_BOLD, 17);
    int dw = ud_width("Done", UD_BOLD, M_TEXT_PX);
    ud_text(win, w - M_GAP - 8 - dw, ud_baseline(0, M_HEAD_H, M_TEXT_PX),
            "Done", l->accent, UD_BOLD, M_TEXT_PX);
}

static void toggle(int win, const struct ud_look *l, int x, int cy, int on)
{
    ud_round(win, x, cy - 13, 44, 26, 13, on ? l->accent : l->line);
    ud_disc(win, on ? x + 20 : x + 2, cy - 11, 22, 0xFFFFFF);
}

/* The menu: the whole window, as the history and the settings are -
   what it leads to, a row each, with the page it was opened from named
   above them */
static void draw_menu(int win, const struct ud_look *l)
{
    int w = win_w(), h = win_h();
    ud_fill(win, 0, 0, w, h, l->bg);
    draw_header(win, l, "Menu");
    for (int i = 0; i < MENU_ITEMS; i++) {
        int y = M_HEAD_H + i * LIST_ROW, cy = y + LIST_ROW / 2;
        int ix = 32;                 /* its icon's middle */
        if (i == 0) {                /* a new tab: + */
            ud_line(win, ix - 8, cy, ix + 8, cy, 28, l->text);
            ud_line(win, ix, cy - 8, ix, cy + 8, 28, l->text);
        } else if (i == 1) {         /* the history: a clock */
            ud_ring(win, ix - 10, cy - 10, 20, 20, 10, l->text);
            ud_line(win, ix, cy, ix, cy - 6, 24, l->text);
            ud_line(win, ix, cy, ix + 5, cy + 3, 24, l->text);
        } else {                     /* the settings: sliders */
            for (int k = -1; k <= 1; k++) {
                ud_line(win, ix - 10, cy + k * 7, ix + 10, cy + k * 7, 20,
                        l->text);
                ud_disc(win, ix - 3 + k * 5, cy + k * 7 - 3, 6, l->text);
            }
        }
        ud_text(win, 60, ud_baseline(y, LIST_ROW, M_TEXT_PX), menu_items[i],
                l->text, UD_SANS, M_TEXT_PX);
        ud_line(win, w - 26, cy - 5, w - 21, cy, 24, l->dim);   /* > */
        ud_line(win, w - 21, cy, w - 26, cy + 5, 24, l->dim);
        ud_fill(win, 20, y + LIST_ROW - 1, w - 40, 1, l->line);
    }
    char shown[256];
    short_uri(shown_uri(tabs[front]->view), shown, sizeof(shown));
    ud_clip(win, 0, M_HEAD_H + MENU_ITEMS * LIST_ROW, w, 40);
    ud_text(win, 20, M_HEAD_H + MENU_ITEMS * LIST_ROW + 28,
            *tabs[front]->title ? tabs[front]->title : shown, l->dim,
            UD_SANS, 12);
    ud_unclip(win);
}

static void draw_history(int win, const struct ud_look *l)
{
    int w = win_w(), h = win_h();
    ud_fill(win, 0, 0, w, h, l->bg);
    for (int i = 0; i < nhistory; i++) {
        int y = M_HEAD_H + i * LIST_ROW - cards_scroll;
        if (y + LIST_ROW <= M_HEAD_H || y >= h - M_BOT_H)
            continue;
        char shown[256];
        short_uri(history[i].uri, shown, sizeof(shown));
        ud_clip(win, 0, MAX(y, M_HEAD_H), w, LIST_ROW);
        ud_text(win, 20, y + 24, *history[i].title ? history[i].title : shown,
                l->text, UD_BOLD, 14);
        ud_text(win, 20, y + 44, shown, l->dim, UD_SANS, 12);
        ud_fill(win, 20, y + LIST_ROW - 1, w - 40, 1, l->line);
        ud_unclip(win);
    }
    if (!nhistory)
        ud_text_mid(win, 0, M_HEAD_H, w, 80, "No pages yet", l->dim,
                    UD_SANS, M_TEXT_PX);
    draw_header(win, l, "History");
    int y0 = h - M_BOT_H;
    ud_fill(win, 0, y0, w, M_BOT_H, l->bg);
    ud_fill(win, 0, y0, w, 1, l->line);
    ud_text_mid(win, 0, y0, w, M_BOT_H, "Clear history", l->danger, UD_BOLD,
                M_TEXT_PX);
}

static void draw_settings(int win, const struct ud_look *l)
{
    int w = win_w(), h = win_h();
    ud_fill(win, 0, 0, w, h, l->bg);
    draw_header(win, l, "Settings");
    static const char *const labels[SETTINGS_ROWS] = {
        "Search engine", "Desktop site", "Dark pages", "JavaScript",
        "Clear history and cookies",
    };
    for (int i = 0; i < SETTINGS_ROWS; i++) {
        int y = M_HEAD_H + i * LIST_ROW, cy = y + LIST_ROW / 2;
        ud_text(win, 20, ud_baseline(y, LIST_ROW, M_TEXT_PX), labels[i],
                i == 4 ? l->danger : l->text, UD_SANS, M_TEXT_PX);
        ud_fill(win, 20, y + LIST_ROW - 1, w - 40, 1, l->line);
        if (i == 0) {
            const char *v = engines[prefs.engine].name;
            int vw = ud_width(v, UD_SANS, M_TEXT_PX);
            ud_text(win, w - 20 - vw, ud_baseline(y, LIST_ROW, M_TEXT_PX), v,
                    l->dim, UD_SANS, M_TEXT_PX);
        } else if (i < 4) {
            int on = i == 1 ? prefs.desktop_site : i == 2 ? prefs.dark
                                                          : !prefs.no_js;
            toggle(win, l, w - 20 - 44, cy, on);
        }
    }
    int y = M_HEAD_H + SETTINGS_ROWS * LIST_ROW + 24;
    ud_text(win, 20, y, "Weft " THREADOS_VERSION " on ThreadOS "
            THREADOS_VERSION, l->dim, UD_SANS, 12);
}

/* The search engines to choose among, the one chosen ticked */
static void draw_engines(int win, const struct ud_look *l)
{
    int w = win_w(), h = win_h();
    ud_fill(win, 0, 0, w, h, l->bg);
    draw_header(win, l, "Search engine");
    for (int i = 0; i < ENGINES; i++) {
        int y = M_HEAD_H + i * LIST_ROW, cy = y + LIST_ROW / 2;
        ud_text(win, 20, ud_baseline(y, LIST_ROW, M_TEXT_PX), engines[i].name,
                l->text, UD_SANS, M_TEXT_PX);
        if (i == prefs.engine) {     /* a tick */
            ud_line(win, w - 40, cy, w - 34, cy + 6, 32, l->accent);
            ud_line(win, w - 34, cy + 6, w - 22, cy - 7, 32, l->accent);
        }
        ud_fill(win, 20, y + LIST_ROW - 1, w - 40, 1, l->line);
    }
}

static void set_agent(WebKitSettings *s);

/* A setting changed: said, kept, put to work */
static void setting(int row)
{
    WebKitWebView *v = tabs[front]->view;
    switch (row) {
    case 0:
        show_sheet(SHEET_ENGINES);   /* the list of them */
        return;
    case 1:
        prefs.desktop_site = !prefs.desktop_site;
        set_agent(settings);
        say("[weft] setting desktop site %s%s\n",
            prefs.desktop_site ? "on" : "off", NULL);
        webkit_web_view_reload(v);
        break;
    case 2:
        prefs.dark = !prefs.dark;
        wpe_settings_set_boolean(wpe_display_get_settings(display),
                                 WPE_SETTING_DARK_MODE, prefs.dark,
                                 WPE_SETTINGS_SOURCE_APPLICATION, NULL);
        say("[weft] setting dark pages %s%s\n", prefs.dark ? "on" : "off",
            NULL);
        webkit_web_view_reload(v);
        break;
    case 3:
        prefs.no_js = !prefs.no_js;
        webkit_settings_set_enable_javascript(settings, !prefs.no_js);
        say("[weft] setting javascript %s%s\n", prefs.no_js ? "off" : "on",
            NULL);
        webkit_web_view_reload(v);
        break;
    case 4:
        nhistory = 0;
        webkit_website_data_manager_clear(
            webkit_network_session_get_website_data_manager(session),
            WEBKIT_WEBSITE_DATA_ALL, 0, NULL, NULL, NULL);
        say("[weft] cleared history and cookies%s%s\n", NULL, NULL);
        break;
    }
    draw_band();
}

static void sheet_tap(int x, int y)
{
    int h = win_h();
    if (sheet == SHEET_MENU && y >= M_HEAD_H) {
        int i = (y - M_HEAD_H) / LIST_ROW;
        if (i >= MENU_ITEMS)
            return;
        if (i == 0) {
            show_sheet(SHEET_NONE);
            if (tab_new(NULL, NULL))
                tab_front(ntabs - 1);
        } else {
            show_sheet(i == 1 ? SHEET_HISTORY : SHEET_SETTINGS);
        }
        return;
    }
    if (y < M_HEAD_H) {
        if (x >= win_w() - 100)              /* Done: from the engines,
                                                back to the settings */
            show_sheet(sheet == SHEET_ENGINES ? SHEET_SETTINGS : SHEET_NONE);
        return;
    }
    if (sheet == SHEET_HISTORY) {
        if (y >= h - M_BOT_H) {
            nhistory = 0;
            say("[weft] history cleared%s%s\n", NULL, NULL);
            draw_band();
            return;
        }
        int i = (y - M_HEAD_H + cards_scroll) / LIST_ROW;
        if (i >= 0 && i < nhistory) {
            char uri[512];
            g_strlcpy(uri, history[i].uri, sizeof(uri));
            show_sheet(SHEET_NONE);
            say("[weft] go %s%s\n", uri, NULL);
            webkit_web_view_load_uri(tabs[front]->view, uri);
        }
        return;
    }
    if (sheet == SHEET_SETTINGS) {
        int i = (y - M_HEAD_H) / LIST_ROW;
        if (i >= 0 && i < SETTINGS_ROWS)
            setting(i);
        return;                      /* (which may have opened another) */
    }
    if (sheet == SHEET_ENGINES) {
        int i = (y - M_HEAD_H) / LIST_ROW;
        if (i >= 0 && i < ENGINES) {
            prefs.engine = i;
            say("[weft] setting search engine %s%s\n", engines[i].name,
                NULL);
            show_sheet(SHEET_SETTINGS);
        }
    }
}

static int sheet_room(void)
{
    if (sheet == SHEET_CARDS)
        return cards_room();
    if (sheet == SHEET_HISTORY) {
        int all = nhistory * LIST_ROW, room = win_h() - M_HEAD_H - M_BOT_H;
        return all > room ? all - room : 0;
    }
    return 0;
}

static void cards_tap(int x, int y)
{
    int w = win_w(), h = win_h();
    if (y >= h - M_BOT_H) {
        if (x >= w / 2 - 30 && x < w / 2 + 30) {
            if (tab_new(NULL, NULL)) {
                cards_show(FALSE);
                tab_front(ntabs - 1);
            }
        } else if (x >= w - 100) {
            cards_show(FALSE);
        }
        return;
    }
    if (y < M_HEAD_H)
        return;
    for (int i = 0; i < ntabs; i++) {
        int cx, cy, cw;
        card_box(i, &cx, &cy, &cw);
        if (x < cx || x >= cx + cw || y < cy || y >= cy + M_CARD_H)
            continue;
        if (y < cy + M_CARD_BAR && x >= cx + cw - 36) {
            tab_close(i);            /* its x */
            draw_band();
        } else {
            cards_show(FALSE);
            tab_front(i);
        }
        return;
    }
}

static void mobile_tap(int x, int y)
{
    WebKitWebView *v = tabs[front]->view;
    if (y < M_TOP_H) {
        int rx = M_PILL_X + (win_w() - 2 * M_PILL_X) - 24;
        if (x >= rx - 20) {
            if (tabs[front]->loading)
                webkit_web_view_stop_loading(v);
            else
                webkit_web_view_reload(v);
        } else {
            address_edit();
        }
        return;
    }
    editing = FALSE;
    int b = x * M_BUTTONS / MAX(win_w(), 1);
    switch (b) {
    case 0: webkit_web_view_go_back(v); break;
    case 1: webkit_web_view_go_forward(v); break;
    case 2: webkit_web_view_load_html(v, start_page, NULL); break;
    case 3: cards_show(TRUE); return;
    case 4: show_sheet(SHEET_MENU); return;
    }
    draw_band();
}

/* A finger on the bands, or anywhere on the cards */
static gboolean mobile_event(int type, int x, int y)
{
    int h = win_h();
    gboolean band = sheet || y < M_TOP_H || y >= h - M_BOT_H;
    switch (type) {
    case WEV_CLICK:
        if (!band && editing) {
            editing = FALSE;
            draw_band();
        }
        return band;
    case WEV_PRESS:
        pressed_on_band = band;
        press_x = x;
        press_y = y;
        press_scroll = cards_scroll;
        press_moved = FALSE;
        return band;
    case WEV_DRAG:
        if (!pressed_on_band)
            return FALSE;
        if (abs(y - press_y) + abs(x - press_x) > 8)
            press_moved = TRUE;
        if (sheet && press_moved) {
            int s = press_scroll - (y - press_y);
            cards_scroll = s < 0 ? 0 : MIN(s, sheet_room());
            draw_band();
        }
        return TRUE;
    case WEV_RELEASE:
        if (!pressed_on_band)
            return FALSE;
        pressed_on_band = FALSE;
        if (!press_moved) {
            if (cards)
                cards_tap(press_x, press_y);
            else if (sheet)
                sheet_tap(press_x, press_y);
            else
                mobile_tap(press_x, press_y);
        }
        return TRUE;
    }
    return band;
}

/* The window's events, before the page has them (wpethreados.h) */
static gboolean band_event(gpointer data, int type, int x, int y, int val)
{
    (void)data;
    if (mobile && (type == WEV_CLICK || type == WEV_PRESS ||
                   type == WEV_DRAG || type == WEV_RELEASE ||
                   type == WEV_SCROLL || type == WEV_ALTCLICK))
        return mobile_event(type, x, y);
    switch (type) {
    case WEV_SIZE:
        draw_band();
        return FALSE;
    case WEV_CLOSE:
        g_main_loop_quit(loop);
        return TRUE;
    case WEV_CLICK:
        if (y < CHROME_H) {
            if ((val & 7) == 1)
                band_click(x, y, 0);
            return TRUE;
        }
        if (editing) {               /* a click on the page leaves it */
            editing = FALSE;
            draw_band();
        }
        return FALSE;
    case WEV_ALTCLICK:
        if (y < CHROME_H) {
            band_click(x, y, (val & 4) != 0);
            return TRUE;
        }
        return FALSE;
    case WEV_PRESS:
        pressed_on_band = y < CHROME_H;
        return pressed_on_band;
    case WEV_DRAG:
    case WEV_RELEASE:
        return pressed_on_band;
    case WEV_SCROLL:
        return y < CHROME_H;
    case WEV_KEY: {
        unsigned char c = (unsigned char)val;
        if (x & WEV_MOD_CTRL) {
            switch (c) {
            case 't':
                if (tab_new(NULL, NULL)) {
                    tab_front(ntabs - 1);
                    address_edit();
                }
                return TRUE;
            case 'w':
                tab_close(front);
                return TRUE;
            case 'l':
                address_edit();
                return TRUE;
            case 'r':
                webkit_web_view_reload(tabs[front]->view);
                return TRUE;
            }
        }
        if (!editing)
            return FALSE;
        address_key(c);
        return TRUE;
    }
    }
    return FALSE;
}

/* The agent: a phone's, a tablet's or a computer's (above) */
static void set_agent(WebKitSettings *s)
{
    char ua[256];
    if (!touch || prefs.desktop_site)
        g_snprintf(ua, sizeof(ua), "Mozilla/5.0 (ThreadOS " THREADOS_VERSION
                   "; Linux " AGENT_ARCH ") " AGENT_ENGINE " Safari/605.1.15 "
                   "Weft/" THREADOS_VERSION);
    else
        g_snprintf(ua, sizeof(ua), "Mozilla/5.0 (Linux; Android 10; "
                   "ThreadOS " THREADOS_VERSION ") " AGENT_ENGINE " %s"
                   "Safari/605.1.15 Weft/" THREADOS_VERSION,
                   win_w() < TABLET_W ? "Mobile " : "");
    webkit_settings_set_user_agent(s, ua);
    say("[weft] user agent %s%s\n", ua, NULL);
}

/* The languages the system is set to (the PC's "language" setting, the
   phone's in Settings) first, English after: Accept-Language, which is
   what a site writes its pages in */
static void set_languages(void)
{
    struct cfg_entry e;
    const char *lang = "en";
    for (uint32_t i = 0; sys_cfg_get(i, &e) == 0; i++)
        if (!strcmp(e.name, "language")) {
            lang = !strcmp(e.value, "pl") ? "pl" : "en";
            break;
        }
    static const char *const pl[] = { "pl-PL", "pl", "en-US", "en", NULL };
    static const char *const en[] = { "en-US", "en", NULL };
    webkit_web_context_set_preferred_languages(
        webkit_web_context_get_default(),
        (const char *const *)(!strcmp(lang, "pl") ? pl : en));
    say("[weft] languages %s%s\n", !strcmp(lang, "pl") ? "pl-PL, pl, en-US, en"
                                                    : "en-US, en", NULL);
}

/* --- The program ---------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *uri = NULL, *proxy = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--eval") && i + 1 < argc)
            eval_script = argv[++i];
        else if (!strcmp(argv[i], "--proxy") && i + 1 < argc)
            proxy = argv[++i];
        else if (!strcmp(argv[i], "--trace"))
            wpe_threados_trace(TRUE);
        else
            uri = argv[i];
    }

    display = wpe_display_threados_new(WIDTH, HEIGHT + CHROME_H, "Weft");
    if (!display) {
        fprintf(stderr, "weft: no display\n");
        return 1;
    }
    /* One window for all the tabs: WebKit is not to make one for each
       view; the tab in front is put into it */
    wpe_settings_set_boolean(wpe_display_get_settings(display),
                             WPE_SETTING_CREATE_VIEWS_WITH_A_TOPLEVEL, FALSE,
                             WPE_SETTINGS_SOURCE_APPLICATION, NULL);
    toplevel = wpe_display_create_toplevel(display, 1);
    if (!toplevel || wpe_threados_toplevel_window(toplevel) < 0) {
        fprintf(stderr, "weft: no window\n");
        return 1;
    }
    /* On a touchscreen, a mobile browser's bands; and pages in the
       system's dark look when it has it (prefers-color-scheme) */
    touch = wpe_threados_toplevel_touch(toplevel);
    mobile = touch;
    struct ud_look look;
    ud_look(&look);
    prefs.dark = look.dark != 0;
    wpe_settings_set_boolean(wpe_display_get_settings(display),
                             WPE_SETTING_DARK_MODE, prefs.dark,
                             WPE_SETTINGS_SOURCE_APPLICATION, NULL);
    if (mobile) {
        wpe_threados_toplevel_set_bottom(toplevel, M_BOT_H);
        wpe_threados_toplevel_set_chrome(toplevel, M_TOP_H, band_event, NULL);
    } else {
        wpe_threados_toplevel_set_chrome(toplevel, CHROME_H, band_event, NULL);
    }

    /* (No EGL and no GPU: the web process paints pages on the processor
       by itself - apps/webkit/patches/web-process-without-egl.patch) */
    settings = webkit_settings_new();
    set_agent(settings);
    set_languages();
    /* Cookies and the cache in memory: the disk is TFS3's 32 MiB, and a
       system started from a stick may have none */
    session = webkit_network_session_new_ephemeral();
    /* A network that lets nothing out but through its proxy (an office,
       a school) is reached only this way: WebKit's own default asks
       GIO, which on ThreadOS knows of no proxy */
    if (proxy) {
        WebKitNetworkProxySettings *ps =
            webkit_network_proxy_settings_new(proxy, NULL);
        webkit_network_session_set_proxy_settings(
            session, WEBKIT_NETWORK_PROXY_MODE_CUSTOM, ps);
        webkit_network_proxy_settings_free(ps);
    }

    loop = g_main_loop_new(NULL, FALSE);
    tab_new(uri, NULL);
    tab_front(0);
    if (!uri)
        address_edit();
    g_timeout_add(20, watch_frames, NULL);
    g_main_loop_run(loop);

    while (ntabs) {
        struct tab *t = tabs[--ntabs];
        g_signal_handlers_disconnect_by_data(t->view, t);
        g_object_unref(t->view);
        g_free(t);
    }
    g_object_unref(session);
    g_object_unref(settings);
    g_object_unref(toplevel);
    g_object_unref(display);
    return 0;
}
