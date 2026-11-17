/* WPE WebKit's platform for ThreadOS
 *
 * WPEPlatform asks a platform for four things: a display (here, the
 * desktop), toplevels (its windows), views (what a page is shown in)
 * and a keymap. The displays the WebKit sources carry are for Wayland,
 * DRM and none at all (headless); this is the fourth kind.
 *
 * Pages reach a view as buffers. A system without EGL gets them in
 * shared memory, painted by Skia on the processor in the web process
 * (apps/webkit/patches/web-process-without-egl.patch): ARGB8888 rows,
 * which are what the desktop's true-colour windows take too, so
 * showing a frame is copying its changed rectangles into the window.
 * The desktop takes at most 1024 pixels a call, so it goes a row - or
 * a piece of one - at a time.
 *
 * The desktop's events are polled, not waited for (win_poll), so a
 * GLib source looks at the windows every few milliseconds and turns
 * what it finds into WPE's events: a press, drags and a release become
 * a button going down, the pointer moving with it held, and the button
 * going up; a key arrives as the bytes of its UTF-8 (a Polish letter
 * is two), an arrow or Home as a code of the desktop's own.
 */

#include "wpethreados.h"

#include <stdio.h>
#include <string.h>
#include "usys64.h"

/* The desktop's codes for keys that are not characters
   (kernel/include/keyboard.h) */
#define TOS_KEY_UP    0x11
#define TOS_KEY_DOWN  0x12
#define TOS_KEY_LEFT  0x13
#define TOS_KEY_RIGHT 0x14
#define TOS_KEY_PGUP  0x15
#define TOS_KEY_PGDN  0x16
#define TOS_KEY_HOME  0x17
#define TOS_KEY_END   0x18

/* How often the windows are looked at: more often than frames come */
#define POLL_MS 10
/* The desktop takes this many pixels in one win_blit32 */
#define BLIT_MAX 1024
/* A frame is over this long after it was shown, as at 60 Hz */
#define FRAME_US (G_USEC_PER_SEC / 60)

static guint64 frames_shown;
static gboolean tracing;

void wpe_threados_trace(gboolean on)
{
    tracing = on;
}

guint64 wpe_threados_frames_shown(void)
{
    return frames_shown;
}

/* --- The keymap ------------------------------------------------------
 *
 * There are no key codes of a keyboard's here, only what the desktop
 * says was typed. A key's code is therefore its keysym, and the keymap
 * translates a code into the same number. */

typedef struct {
    WPEKeymap parent;
    WPEModifiers modifiers;
} WPEKeymapThreadOS;
typedef struct {
    WPEKeymapClass parent_class;
} WPEKeymapThreadOSClass;

static GType wpe_keymap_threados_get_type(void);
G_DEFINE_TYPE(WPEKeymapThreadOS, wpe_keymap_threados, WPE_TYPE_KEYMAP)

static gboolean keymap_entries_for_keyval(WPEKeymap *keymap, guint keyval,
                                          WPEKeymapEntry **entries,
                                          guint *n_entries)
{
    (void)keymap;
    *entries = g_new0(WPEKeymapEntry, 1);
    (*entries)[0].keycode = keyval;
    *n_entries = 1;
    return TRUE;
}

static gboolean keymap_translate(WPEKeymap *keymap, guint keycode,
                                 WPEModifiers modifiers, int group,
                                 guint *keyval, int *effective_group,
                                 int *level, WPEModifiers *consumed)
{
    (void)keymap;
    (void)modifiers;
    if (keyval)
        *keyval = keycode;
    if (effective_group)
        *effective_group = group;
    if (level)
        *level = 0;
    if (consumed)
        *consumed = 0;
    return TRUE;
}

static WPEModifiers keymap_modifiers(WPEKeymap *keymap)
{
    return ((WPEKeymapThreadOS *)keymap)->modifiers;
}

static void wpe_keymap_threados_init(WPEKeymapThreadOS *keymap)
{
    (void)keymap;
}

static void wpe_keymap_threados_class_init(WPEKeymapThreadOSClass *klass)
{
    WPEKeymapClass *keymap_class = WPE_KEYMAP_CLASS(klass);
    keymap_class->get_entries_for_keyval = keymap_entries_for_keyval;
    keymap_class->translate_keyboard_state = keymap_translate;
    keymap_class->get_modifiers = keymap_modifiers;
}

/* --- The types ------------------------------------------------------- */

struct _WPEDisplayThreadOS {
    WPEDisplay parent;
    int width, height;
    char *title;
    WPEKeymapThreadOS *keymap;
};
G_DEFINE_FINAL_TYPE(WPEDisplayThreadOS, wpe_display_threados, WPE_TYPE_DISPLAY)

typedef struct {
    WPEView parent;
    WPEBuffer *pending;       /* on the screen; its frame not yet over */
    WPEBuffer *committed;     /* the one before it, still held */
    guint frame_source;
    gint64 last_frame;
    guint32 row[BLIT_MAX];    /* pixels on their way to the window */
} WPEViewThreadOS;
typedef struct {
    WPEViewClass parent_class;
} WPEViewThreadOSClass;

static GType wpe_view_threados_get_type(void);
G_DEFINE_TYPE(WPEViewThreadOS, wpe_view_threados, WPE_TYPE_VIEW)

typedef struct {
    WPEToplevel parent;
    int win;
    guint poll_source;
    WPEModifiers buttons;     /* WPE_MODIFIER_POINTER_BUTTON* held */
    WPEModifiers keys;        /* WPE_MODIFIER_KEYBOARD_* last said */
    double x, y;              /* where the pointer was last */
    guint32 utf8;             /* a character's value, so far */
    int utf8_left;            /* bytes of it still to come */
} WPEToplevelThreadOS;
typedef struct {
    WPEToplevelClass parent_class;
} WPEToplevelThreadOSClass;

static GType wpe_toplevel_threados_get_type(void);
G_DEFINE_TYPE(WPEToplevelThreadOS, wpe_toplevel_threados, WPE_TYPE_TOPLEVEL)

/* --- Showing a frame ------------------------------------------------- */

/* The window of the view's toplevel, or -1 */
int wpe_threados_view_window(WPEView *view)
{
    WPEToplevel *toplevel = wpe_view_get_toplevel(view);
    if (!toplevel || !G_TYPE_CHECK_INSTANCE_TYPE(toplevel,
                                                 wpe_toplevel_threados_get_type()))
        return -1;
    return ((WPEToplevelThreadOS *)toplevel)->win;
}

static void blit_rect(WPEViewThreadOS *self, int win, const guint8 *data,
                      guint stride, int bw, int bh, int x, int y, int w,
                      int h)
{
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > bw)
        w = bw - x;
    if (y + h > bh)
        h = bh - y;
    for (int yy = y; yy < y + h; yy++) {
        const guint32 *src = (const guint32 *)(data + (gsize)yy * stride);
        for (int xx = x; xx < x + w; xx += BLIT_MAX) {
            int n = MIN(BLIT_MAX, x + w - xx);
            /* ARGB8888 is 0xAARRGGBB as a 32-bit word, which is the
               window's 0x00RRGGBB once the alpha is gone: a page's
               last layer is opaque, and the desktop does not blend */
            for (int i = 0; i < n; i++)
                self->row[i] = src[xx + i] & 0x00FFFFFFu;
            win_blit32(win, xx, yy, n, 1, self->row);
        }
    }
}

/* The frame is over: WebKit may paint the next one, and the buffer
   shown before this one is free again */
static gboolean frame_done(gpointer data)
{
    WPEViewThreadOS *self = data;
    WPEView *view = WPE_VIEW(self);
    self->frame_source = 0;
    if (self->committed)
        wpe_view_buffer_released(view, self->committed);
    g_clear_object(&self->committed);
    self->committed = self->pending;
    self->pending = NULL;
    if (self->committed)
        wpe_view_buffer_rendered(view, self->committed);
    return G_SOURCE_REMOVE;
}

static gboolean view_render_buffer(WPEView *view, WPEBuffer *buffer,
                                   const WPERectangle *damage,
                                   guint n_damage, GError **error)
{
    WPEViewThreadOS *self = (WPEViewThreadOS *)view;
    if (!WPE_IS_BUFFER_SHM(buffer)) {
        g_set_error_literal(error, WPE_VIEW_ERROR,
                            WPE_VIEW_ERROR_RENDER_FAILED,
                            "ThreadOS shows buffers in shared memory only");
        return FALSE;
    }
    WPEBufferSHM *shm = WPE_BUFFER_SHM(buffer);
    if (wpe_buffer_shm_get_format(shm) != WPE_PIXEL_FORMAT_ARGB8888) {
        g_set_error_literal(error, WPE_VIEW_ERROR,
                            WPE_VIEW_ERROR_RENDER_FAILED,
                            "ThreadOS shows ARGB8888 buffers only");
        return FALSE;
    }

    int win = wpe_threados_view_window(view);
    if (win >= 0) {
        gsize size;
        const guint8 *data = g_bytes_get_data(wpe_buffer_shm_get_data(shm),
                                              &size);
        guint stride = wpe_buffer_shm_get_stride(shm);
        int bw = wpe_buffer_get_width(buffer);
        int bh = wpe_buffer_get_height(buffer);
        if (data && (gsize)stride * (gsize)bh <= size) {
            if (!n_damage)
                blit_rect(self, win, data, stride, bw, bh, 0, 0, bw, bh);
            for (guint i = 0; i < n_damage; i++)
                blit_rect(self, win, data, stride, bw, bh, damage[i].x,
                          damage[i].y, damage[i].width, damage[i].height);
            frames_shown++;
        }
    }

    /* The frame ends at the next tick of a 60 Hz screen, not at once:
       WebKit paints the next one when it hears, and a page that
       animates would otherwise paint as fast as the processor goes */
    g_clear_object(&self->pending);
    self->pending = g_object_ref(buffer);
    if (!self->frame_source) {
        gint64 now = g_get_monotonic_time();
        gint64 next = self->last_frame + FRAME_US;
        self->last_frame = now;
        guint wait = next > now ? (guint)((next - now) / 1000) : 0;
        self->frame_source = g_timeout_add(wait, frame_done, self);
    }
    return TRUE;
}

static gboolean view_can_be_mapped(WPEView *view)
{
    return wpe_threados_view_window(view) >= 0;
}

static void view_dispose(GObject *object)
{
    WPEViewThreadOS *self = (WPEViewThreadOS *)object;
    if (self->frame_source) {
        g_source_remove(self->frame_source);
        self->frame_source = 0;
    }
    g_clear_object(&self->pending);
    g_clear_object(&self->committed);
    G_OBJECT_CLASS(wpe_view_threados_parent_class)->dispose(object);
}

/* A view put in a toplevel takes its size and is shown; taken out of
   one, it is not */
static void view_toplevel_changed(WPEView *view, GParamSpec *pspec,
                                  gpointer data)
{
    (void)pspec;
    (void)data;
    WPEToplevel *toplevel = wpe_view_get_toplevel(view);
    if (!toplevel) {
        wpe_view_unmap(view);
        return;
    }
    int width, height;
    wpe_toplevel_get_size(toplevel, &width, &height);
    if (width && height)
        wpe_view_resized(view, width, height);
    wpe_view_map(view);
    wpe_view_focus_in(view);
}

static void view_constructed(GObject *object)
{
    G_OBJECT_CLASS(wpe_view_threados_parent_class)->constructed(object);
    g_signal_connect(object, "notify::toplevel",
                     G_CALLBACK(view_toplevel_changed), NULL);
}

static void wpe_view_threados_init(WPEViewThreadOS *self)
{
    (void)self;
}

static void wpe_view_threados_class_init(WPEViewThreadOSClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->constructed = view_constructed;
    object_class->dispose = view_dispose;
    WPEViewClass *view_class = WPE_VIEW_CLASS(klass);
    view_class->render_buffer = view_render_buffer;
    view_class->can_be_mapped = view_can_be_mapped;
}

/* --- Events ---------------------------------------------------------- */

static gboolean first_view_cb(WPEToplevel *toplevel, WPEView *view,
                              gpointer data)
{
    (void)toplevel;
    *(WPEView **)data = view;
    return TRUE;
}

static WPEView *first_view(WPEToplevel *toplevel)
{
    WPEView *view = NULL;
    wpe_toplevel_foreach_view(toplevel, first_view_cb, &view);
    return view;
}

static guint32 now_ms(void)
{
    return (guint32)(g_get_monotonic_time() / 1000);
}

static void send(WPEView *view, WPEEvent *event)
{
    wpe_view_event(view, event);
    wpe_event_unref(event);
}

/* The desktop's buttons (1 left, 2 right, 4 middle) as WPE's numbers,
   which are X11's: 1 primary, 2 middle, 3 secondary */
static guint button_of(int mask)
{
    return (mask & 2) ? 3 : (mask & 4) ? 2 : 1;
}

static WPEModifiers button_modifier(guint button)
{
    return button == 3 ? WPE_MODIFIER_POINTER_BUTTON3
         : button == 2 ? WPE_MODIFIER_POINTER_BUTTON2
                       : WPE_MODIFIER_POINTER_BUTTON1;
}

static WPEModifiers keys_of(int desktop_mods)
{
    WPEModifiers m = 0;
    if (desktop_mods & WEV_MOD_SHIFT)
        m |= WPE_MODIFIER_KEYBOARD_SHIFT;
    if (desktop_mods & WEV_MOD_CTRL)
        m |= WPE_MODIFIER_KEYBOARD_CONTROL;
    if (desktop_mods & WEV_MOD_ALT)
        m |= WPE_MODIFIER_KEYBOARD_ALT;
    return m;
}

static void key(WPEToplevelThreadOS *self, WPEView *view, guint keyval)
{
    WPEDisplayThreadOS *display =
        WPE_DISPLAY_THREADOS(wpe_toplevel_get_display(WPE_TOPLEVEL(self)));
    display->keymap->modifiers = self->keys;
    guint32 t = now_ms();
    send(view, wpe_event_keyboard_new(WPE_EVENT_KEYBOARD_KEY_DOWN, view,
                                      WPE_INPUT_SOURCE_KEYBOARD, t,
                                      self->keys, keyval, keyval));
    send(view, wpe_event_keyboard_new(WPE_EVENT_KEYBOARD_KEY_UP, view,
                                      WPE_INPUT_SOURCE_KEYBOARD, t,
                                      self->keys, keyval, keyval));
}

/* One byte the desktop typed: a key of its own, or a byte of UTF-8 */
static void key_byte(WPEToplevelThreadOS *self, WPEView *view,
                     unsigned char c)
{
    if (self->utf8_left) {
        if ((c & 0xC0) == 0x80) {
            self->utf8 = (self->utf8 << 6) | (c & 0x3F);
            if (--self->utf8_left == 0)
                key(self, view, wpe_unicode_to_keyval(self->utf8));
            return;
        }
        self->utf8_left = 0;        /* broken off: start again */
    }
    if (c >= 0xC2 && c <= 0xF4) {
        self->utf8_left = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
        self->utf8 = c & (0x3F >> self->utf8_left);
        return;
    }

    guint keyval;
    switch (c) {
    case '\b':          keyval = WPE_KEY_BackSpace; break;
    case '\t':          keyval = WPE_KEY_Tab; break;
    case '\n':
    case '\r':          keyval = WPE_KEY_Return; break;
    case 0x1B:          keyval = WPE_KEY_Escape; break;
    case TOS_KEY_UP:    keyval = WPE_KEY_Up; break;
    case TOS_KEY_DOWN:  keyval = WPE_KEY_Down; break;
    case TOS_KEY_LEFT:  keyval = WPE_KEY_Left; break;
    case TOS_KEY_RIGHT: keyval = WPE_KEY_Right; break;
    case TOS_KEY_PGUP:  keyval = WPE_KEY_Page_Up; break;
    case TOS_KEY_PGDN:  keyval = WPE_KEY_Page_Down; break;
    case TOS_KEY_HOME:  keyval = WPE_KEY_Home; break;
    case TOS_KEY_END:   keyval = WPE_KEY_End; break;
    default:
        if (c < 0x20 || c >= 0x7F)
            return;                 /* the desktop's own shortcuts */
        keyval = c;                 /* Latin-1's keysyms are its codes */
    }
    key(self, view, keyval);
}

static void pointer_move(WPEToplevelThreadOS *self, WPEView *view, int x,
                         int y)
{
    double dx = x - self->x, dy = y - self->y;
    self->x = x;
    self->y = y;
    send(view, wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, view,
                                          WPE_INPUT_SOURCE_MOUSE, now_ms(),
                                          self->buttons | self->keys,
                                          x, y, dx, dy));
}

static void handle(WPEToplevelThreadOS *self, WPEView *view,
                   const struct win_event *ev)
{
    if (tracing) {
        printf("[webview] event %d at %d,%d val %d\n", ev->type, ev->x,
               ev->y, ev->val);
        fflush(stdout);
    }
    switch (ev->type) {
    case WEV_CLICK:
        /* What comes with the press that follows: the keys held */
        self->keys = keys_of(ev->val);
        break;
    case WEV_PRESS: {
        guint button = button_of(ev->val);
        if (ev->x != self->x || ev->y != self->y)
            pointer_move(self, view, ev->x, ev->y);
        send(view, wpe_event_pointer_button_new(
                       WPE_EVENT_POINTER_DOWN, view, WPE_INPUT_SOURCE_MOUSE,
                       now_ms(), self->buttons | self->keys, button, ev->x,
                       ev->y, 1));
        self->buttons |= button_modifier(button);
        break;
    }
    case WEV_DRAG:
        pointer_move(self, view, ev->x, ev->y);
        break;
    case WEV_RELEASE: {
        WPEModifiers held = self->buttons;
        guint button = (held & WPE_MODIFIER_POINTER_BUTTON3) ? 3
                     : (held & WPE_MODIFIER_POINTER_BUTTON2) ? 2 : 1;
        if (ev->x != self->x || ev->y != self->y)
            pointer_move(self, view, ev->x, ev->y);
        self->buttons = 0;
        send(view, wpe_event_pointer_button_new(
                       WPE_EVENT_POINTER_UP, view, WPE_INPUT_SOURCE_MOUSE,
                       now_ms(), held | self->keys, button, ev->x, ev->y,
                       1));
        break;
    }
    case WEV_SCROLL:
        /* A notch is three lines of 40 pixels, as GTK's wheel goes;
           away from the person (positive) is up the page */
        send(view, wpe_event_scroll_new(view, WPE_INPUT_SOURCE_MOUSE,
                                        now_ms(), self->keys, 0,
                                        -ev->val * 120.0, TRUE, FALSE,
                                        ev->x, ev->y));
        break;
    case WEV_KEY:
        self->keys = keys_of(ev->x);
        key_byte(self, view, (unsigned char)ev->val);
        break;
    case WEV_SIZE:
        wpe_toplevel_resized(WPE_TOPLEVEL(self), ev->x, ev->y);
        wpe_view_resized(view, ev->x, ev->y);
        break;
    }
}

static gboolean poll_window(gpointer data)
{
    WPEToplevelThreadOS *self = data;
    struct win_event ev;
    while (self->win >= 0 && win_poll(self->win, &ev) == 1) {
        if (ev.type == WEV_CLOSE) {
            /* WebKit hears the window went; the program decides what
               that means (apps/webkit/webview.c quits) */
            self->poll_source = 0;
            wpe_toplevel_closed(WPE_TOPLEVEL(self));
            return G_SOURCE_REMOVE;
        }
        WPEView *view = first_view(WPE_TOPLEVEL(self));
        if (view)
            handle(self, view, &ev);
    }
    return G_SOURCE_CONTINUE;
}

/* --- The toplevel ---------------------------------------------------- */

static void toplevel_constructed(GObject *object)
{
    G_OBJECT_CLASS(wpe_toplevel_threados_parent_class)->constructed(object);
    WPEToplevelThreadOS *self = (WPEToplevelThreadOS *)object;
    WPEDisplayThreadOS *display = WPE_DISPLAY_THREADOS(
        wpe_toplevel_get_display(WPE_TOPLEVEL(self)));
    self->win = (int)win_create(display->width, display->height,
                                display->title ? display->title : "Web");
    if (self->win < 0)
        return;
    /* Colours, not the desktop's palette */
    win_truecolour(self->win);
    wpe_toplevel_resized(WPE_TOPLEVEL(self), display->width,
                         display->height);
    wpe_toplevel_state_changed(WPE_TOPLEVEL(self), WPE_TOPLEVEL_STATE_ACTIVE);
    self->poll_source = g_timeout_add(POLL_MS, poll_window, self);
}

static void toplevel_dispose(GObject *object)
{
    WPEToplevelThreadOS *self = (WPEToplevelThreadOS *)object;
    if (self->poll_source) {
        g_source_remove(self->poll_source);
        self->poll_source = 0;
    }
    G_OBJECT_CLASS(wpe_toplevel_threados_parent_class)->dispose(object);
}

static void toplevel_set_title(WPEToplevel *toplevel, const char *title)
{
    WPEToplevelThreadOS *self = (WPEToplevelThreadOS *)toplevel;
    if (self->win >= 0 && title)
        win_title(self->win, title);
}

/* The desktop sizes windows; a program cannot */
static gboolean toplevel_resize(WPEToplevel *toplevel, int width,
                                int height)
{
    (void)toplevel;
    (void)width;
    (void)height;
    return FALSE;
}

static void wpe_toplevel_threados_init(WPEToplevelThreadOS *self)
{
    self->win = -1;
    self->x = self->y = -1;
}

static void wpe_toplevel_threados_class_init(WPEToplevelThreadOSClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->constructed = toplevel_constructed;
    object_class->dispose = toplevel_dispose;
    WPEToplevelClass *toplevel_class = WPE_TOPLEVEL_CLASS(klass);
    toplevel_class->set_title = toplevel_set_title;
    toplevel_class->resize = toplevel_resize;
}

/* --- The display ----------------------------------------------------- */

static gboolean display_connect(WPEDisplay *display, GError **error)
{
    (void)display;
    (void)error;
    return TRUE;
}

static WPEView *display_create_view(WPEDisplay *display)
{
    return WPE_VIEW(g_object_new(wpe_view_threados_get_type(), "display",
                                 display, NULL));
}

static WPEToplevel *display_create_toplevel(WPEDisplay *display,
                                            guint max_views)
{
    return WPE_TOPLEVEL(g_object_new(wpe_toplevel_threados_get_type(),
                                     "display", display, "max-views",
                                     max_views, NULL));
}

/* (transfer none: the display keeps a reference of its own) */
static WPEKeymap *display_get_keymap(WPEDisplay *display)
{
    return WPE_KEYMAP(WPE_DISPLAY_THREADOS(display)->keymap);
}

/* No EGL: the display has none to give, and WebKit then paints on the
   processor into shared memory */
static gpointer display_get_egl_display(WPEDisplay *display, GError **error)
{
    (void)display;
    g_set_error_literal(error, WPE_EGL_ERROR, WPE_EGL_ERROR_NOT_AVAILABLE,
                        "ThreadOS has no EGL");
    return NULL;
}

static void display_finalize(GObject *object)
{
    WPEDisplayThreadOS *self = WPE_DISPLAY_THREADOS(object);
    g_free(self->title);
    g_clear_object(&self->keymap);
    G_OBJECT_CLASS(wpe_display_threados_parent_class)->finalize(object);
}

static void wpe_display_threados_init(WPEDisplayThreadOS *self)
{
    self->keymap = g_object_new(wpe_keymap_threados_get_type(), NULL);
}

static void wpe_display_threados_class_init(WPEDisplayThreadOSClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = display_finalize;
    WPEDisplayClass *display_class = WPE_DISPLAY_CLASS(klass);
    display_class->connect = display_connect;
    display_class->create_view = display_create_view;
    display_class->create_toplevel = display_create_toplevel;
    display_class->get_keymap = display_get_keymap;
    display_class->get_egl_display = display_get_egl_display;
}

WPEDisplay *wpe_display_threados_new(int width, int height,
                                     const char *title)
{
    WPEDisplayThreadOS *self = g_object_new(WPE_TYPE_DISPLAY_THREADOS, NULL);
    self->width = width;
    self->height = height;
    self->title = g_strdup(title);
    GError *error = NULL;
    if (!wpe_display_connect(WPE_DISPLAY(self), &error)) {
        g_warning("ThreadOS display: %s", error->message);
        g_error_free(error);
        g_object_unref(self);
        return NULL;
    }
    return WPE_DISPLAY(self);
}
