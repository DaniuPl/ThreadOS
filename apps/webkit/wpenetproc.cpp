// wpenetproc: WebKit's network process on ThreadOS
//
// The process that fetches what pages ask for, through libsoup, for
// the UI process (apps/webkit/webview.c) and the web processes. HTTPS
// is glib-networking's OpenSSL backend, which GIO loads as a module
// elsewhere; a ThreadOS program has no modules to load, so it is
// registered here before WebKit's own entry point starts asking.

#include <gio/gio.h>

extern "C" void g_io_openssl_load(GIOModule* module);

namespace WebKit {
int NetworkProcessMain(int argc, char** argv);
}

int main(int argc, char** argv)
{
    g_io_openssl_load(nullptr);
    return WebKit::NetworkProcessMain(argc, argv);
}
