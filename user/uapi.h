/* The entry into the kernel for the programs' shared code
 *
 * The cryptography, TLS, HTTP, DNS and the HTML parser compute on
 * bytes and know nothing about the kernel underneath. They do talk to
 * it through a handful of names, though: this file brings in the system
 * calls (usys64.h) and the dictionary that gives them those names
 * (compat64.h), so that none of those headers has to know more.
 */

#ifndef THREADOS_UAPI_H
#define THREADOS_UAPI_H

#include "../user64/usys64.h"
#include "../user64/compat64.h"

#endif
