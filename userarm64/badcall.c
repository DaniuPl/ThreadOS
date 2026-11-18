/* badcall - system calls given what is not the program's
 *
 * A pointer into the kernel and one into nothing: the kernel must say
 * no (-1) to both rather than read them for the program, and the
 * program goes on.
 */
#include "usys.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    print("write of the kernel's memory: ");
    print_dec(sys_write(1, (const void *)0x40200000UL, 16));
    print("\nwrite of unmapped memory: ");
    print_dec(sys_write(1, (const void *)0x5000000000UL, 16));
    print("\nwrite to a file it does not have: ");
    print_dec(sys_write(7, "x", 1));
    print("\nan unknown system call: ");
    print_dec(syscall3(9999, 0, 0, 0));
    print("\n");
    return 0;
}
