/* The entry into the kernel for the programs' shared code
 *
 * The cryptography, TLS, HTTP, DNS and the HTML parser are the same on
 * both sides - they compute on bytes and know nothing about the
 * processor mode. They do talk to the kernel through a handful of
 * names, though, and those have a different shape in long mode. This
 * file picks the right side, so that none of those headers has to know
 * about it.
 */

#ifndef THREADOS_UAPI_H
#define THREADOS_UAPI_H

#ifdef __x86_64__
#include "../user64/usys64.h"
#include "../user64/compat64.h"
#else
#include "usys.h"
#endif

#endif
