/* What a program can say about the shared library
 *
 * Everything here has to be computed INSIDE the library and there is
 * no way around it. A non-relocatable program taking the address of a
 * function from the library gets the address of its own jump stub - the
 * place it jumps through - and not the place where the function really
 * lies. So the question "where are you" can only be answered by the
 * library itself.
 */

#ifndef PROTONOS_UPROTON_H
#define PROTONOS_UPROTON_H

#include "uapi.h"
#include "usha256.h"
#include "urand.h"

/* The address of the library's code - the same in every process,
   because these are the same physical frames */
static inline uintptr_t proton_code_addr(void)
{
    return (uintptr_t)sha256;
}

/* The address of the library's data. The virtual address is the same
   everywhere too, but the frames under it are not - every process has
   its own. */
static inline uintptr_t proton_data_addr(void)
{
    return (uintptr_t)urand_seed;
}

/* The random generator's counter. It grows with every fetch of bytes,
   so it shows how much this process took from the library - and it is a
   good probe for whether the data really are private. */
static inline uint32_t proton_random_counter(void)
{
    return urand_counter;
}

#endif
