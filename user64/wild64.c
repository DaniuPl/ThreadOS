/* wild64 - a program that reaches for kernel memory
 *
 * Twice, because there are two ways to it. First it asks the kernel to
 * print it a piece of memory from the kernel image's address - that is
 * an attempt to cheat the kernel with its own hands, and the kernel has
 * to refuse, because it checks whether a pointer belongs to the program
 * at all.
 *
 * Then it reaches there itself. That attempt is no longer stopped by
 * any check in the code, but by the processor itself: a kernel page has
 * no access bit for ring 3.
 */

#include "usys64.h"

#define KERNEL_ADDR 0x100000ULL          /* the kernel image below a megabyte */

void _start(void)
{
    print("[wild64] asking the kernel to print its own memory\n");
    int64_t ret = syscall64(SYS64_WRITE, (int64_t)KERNEL_ADDR, 16);
    if (ret < 0)
        print("[wild64] the kernel refused - good\n");
    else
        print("[wild64] ERROR: the kernel printed its memory!\n");

    print("[wild64] then I will reach in myself - this should kill me\n");
    volatile unsigned char *p = (volatile unsigned char *)(uintptr_t)KERNEL_ADDR;
    unsigned char v = *p;
    (void)v;

    print("[wild64] ERROR: I read the kernel's memory!\n");
    sys_exit(1);
}
