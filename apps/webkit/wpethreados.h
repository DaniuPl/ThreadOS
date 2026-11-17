/* WPE WebKit's platform for ThreadOS (apps/webkit/wpethreados.c)
 *
 * WPE draws through WPEPlatform: a display, its views, and the
 * toplevels that hold them. This is the ThreadOS one: a toplevel is a
 * window of the desktop's, a view shows the pages WebKit paints on the
 * processor (shared-memory buffers) in it, and what the desktop says
 * of the pointer and the keys becomes WPE's events.
 */
#ifndef WPETHREADOS_H
#define WPETHREADOS_H

#include <wpe/wpe-platform.h>

G_BEGIN_DECLS

#define WPE_TYPE_DISPLAY_THREADOS (wpe_display_threados_get_type())
G_DECLARE_FINAL_TYPE(WPEDisplayThreadOS, wpe_display_threados, WPE,
                     DISPLAY_THREADOS, WPEDisplay)

/* The display, connected; views made on it open windows of the given
   size, titled as given until the page names itself */
WPEDisplay *wpe_display_threados_new(int width, int height,
                                     const char *title);

/* The desktop's number for the window of the toplevel a view is in,
   or -1 */
int wpe_threados_view_window(WPEView *view);

/* How many frames views of this display have put on the screen */
guint64 wpe_threados_frames_shown(void);

G_END_DECLS

#endif
