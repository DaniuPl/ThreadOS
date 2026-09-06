/* nx64 - a program that tries to execute its own data
 *
 * The no-execute bit has been set for a long time, but setting the bit
 * proves nothing by itself. What proves it is that the processor really
 * reacts to it - and the only way to check that is to try.
 *
 * The program writes a single return instruction into its array and
 * jumps to its address. The array lies in the .bss section, to which
 * the kernel gave write permission and no execute permission - so the
 * write goes through and the jump ends in an exception and the
 * program's death.
 */

#include "usys64.h"

static volatile unsigned char data[16];

void _start(void)
{
    print("[nx64] writing an instruction into my own data\n");
    data[0] = 0xC3;                  /* ret */

    print("[nx64] and trying to execute it - this should kill me\n");
    void (*fn)(void);
    *(void **)&fn = (void *)(unsigned char *)data;
    fn();

    print("[nx64] ERROR: the processor executed data!\n");
    sys_exit(1);
}
