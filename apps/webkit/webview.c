/* webview: a window with WebKit in it (WPE WebKit on ThreadOS)
 *
 *   webview [URL]
 *   webview --eval SCRIPT URL
 *
 * The UI process of WebKit's three: it opens a window through the
 * ThreadOS platform (apps/webkit/wpethreados.c), and WebKit starts the
 * web process, which lays out and paints the page (wpewebproc), and the
 * network process, which fetches it (wpenetproc), and talks to them
 * over socket pairs.
 *
 * What happens is said on standard output, a line each, so that the
 * tests can follow it (scripts/tests/t_webkit.py):
 *
 *   [webview] load committed <uri>
 *   [webview] load finished <uri>
 *   [webview] title <title>
 *   [webview] frame shown            (the first frame in the window)
 *   [webview] eval <result>          (--eval: the script's value, as a
 *                                     string, once the page has loaded)
 *   [webview] load failed <uri>: <why>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wpe/webkit.h>
#include "wpethreados.h"

/* The window's content; small enough for an 800 x 600 screen, which
   is the tests' (scripts/tests/t_webkit.py reads these) */
#define WIDTH  640
#define HEIGHT 400

static GMainLoop *loop;
static const char *eval_script;
static int frame_said;

static void evaluated(GObject *object, GAsyncResult *result, gpointer data)
{
    (void)data;
    GError *error = NULL;
    JSCValue *value = webkit_web_view_evaluate_javascript_finish(
        WEBKIT_WEB_VIEW(object), result, &error);
    if (!value) {
        printf("[webview] eval failed: %s\n", error->message);
        g_error_free(error);
    } else {
        char *s = jsc_value_to_string(value);
        printf("[webview] eval %s\n", s);
        g_free(s);
        g_object_unref(value);
    }
    fflush(stdout);
}

static void load_changed(WebKitWebView *view, WebKitLoadEvent event,
                         gpointer data)
{
    (void)data;
    const char *uri = webkit_web_view_get_uri(view);
    if (event == WEBKIT_LOAD_COMMITTED) {
        printf("[webview] load committed %s\n", uri);
    } else if (event == WEBKIT_LOAD_FINISHED) {
        printf("[webview] load finished %s\n", uri);
        if (eval_script)
            webkit_web_view_evaluate_javascript(view, eval_script, -1, NULL,
                                                NULL, NULL, evaluated, NULL);
    }
    fflush(stdout);
}

static gboolean load_failed(WebKitWebView *view, WebKitLoadEvent event,
                            const char *uri, GError *error, gpointer data)
{
    (void)view;
    (void)event;
    (void)data;
    printf("[webview] load failed %s: %s\n", uri, error->message);
    fflush(stdout);
    return FALSE;                    /* WebKit shows its error page */
}

static void title_changed(WebKitWebView *view, GParamSpec *pspec,
                          gpointer data)
{
    (void)pspec;
    (void)data;
    const char *title = webkit_web_view_get_title(view);
    if (!title || !*title)
        return;
    printf("[webview] title %s\n", title);
    fflush(stdout);
    WPEToplevel *toplevel =
        wpe_view_get_toplevel(webkit_web_view_get_wpe_view(view));
    if (toplevel)
        wpe_toplevel_set_title(toplevel, title);
}

/* The first frame on the screen is said once: from then on the window
   shows the page */
static gboolean watch_frames(gpointer data)
{
    (void)data;
    if (!frame_said && wpe_threados_frames_shown() > 0) {
        frame_said = 1;
        printf("[webview] frame shown\n");
        fflush(stdout);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void closed(WebKitWebView *view, gpointer data)
{
    (void)view;
    (void)data;
    g_main_loop_quit(loop);
}

/* The window went: WebKit's view is no longer in a toplevel */
static void toplevel_changed(WPEView *view, GParamSpec *pspec, gpointer data)
{
    (void)pspec;
    (void)data;
    if (!wpe_view_get_toplevel(view))
        g_main_loop_quit(loop);
}

int main(int argc, char **argv)
{
    const char *uri = "about:blank";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--eval") && i + 1 < argc)
            eval_script = argv[++i];
        else
            uri = argv[i];
    }

    WPEDisplay *display = wpe_display_threados_new(WIDTH, HEIGHT, "Web");
    if (!display) {
        fprintf(stderr, "webview: no display\n");
        return 1;
    }

    /* (No EGL and no GPU: the web process paints pages on the processor
       by itself - apps/webkit/patches/web-process-without-egl.patch) */
    WebKitSettings *settings = webkit_settings_new();

    /* Cookies and the cache in memory: the disk is TFS3's 32 MiB, and a
       system started from a stick may have none */
    WebKitNetworkSession *session = webkit_network_session_new_ephemeral();

    WebKitWebView *view = WEBKIT_WEB_VIEW(g_object_new(
        WEBKIT_TYPE_WEB_VIEW, "display", display, "settings", settings,
        "network-session", session, NULL));
    g_signal_connect(view, "load-changed", G_CALLBACK(load_changed), NULL);
    g_signal_connect(view, "load-failed", G_CALLBACK(load_failed), NULL);
    g_signal_connect(view, "notify::title", G_CALLBACK(title_changed), NULL);
    g_signal_connect(view, "close", G_CALLBACK(closed), NULL);
    g_signal_connect(webkit_web_view_get_wpe_view(view), "notify::toplevel",
                     G_CALLBACK(toplevel_changed), NULL);
    g_timeout_add(20, watch_frames, NULL);

    loop = g_main_loop_new(NULL, FALSE);
    webkit_web_view_load_uri(view, uri);
    g_main_loop_run(loop);

    g_object_unref(view);
    g_object_unref(session);
    g_object_unref(settings);
    g_object_unref(display);
    return 0;
}
