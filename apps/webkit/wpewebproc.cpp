// wpewebproc: WebKit's web process on ThreadOS
//
// The process that parses, lays out, runs the scripts of and paints a
// page, started by the UI process (apps/webkit/weft.c) with its
// socket's number in its arguments. WebKit's own entry point does all
// of it; this is the program around it, named as ThreadOS names
// programs (apps/webkit/patches/process-names.patch).
//
// Its fonts come through fontconfig, whose own default configuration
// is a path where the libraries were built; the system's is the one
// the C library makes up out of the initrd's fonts
// (libc/overlay/src/threados/socket.c).

#include <stdlib.h>

namespace WebKit {
int WebProcessMain(int argc, char** argv);
}

int main(int argc, char** argv)
{
    setenv("FONTCONFIG_FILE", "/etc/fonts/fonts.conf", 0);
    return WebKit::WebProcessMain(argc, argv);
}
