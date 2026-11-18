/* fault KIND - a program doing what a program may not
 *
 *   fault kernel   reads the kernel's own code (each address on itself,
 *                  reachable only at EL1)
 *   fault null     reads address 0
 *   fault text     writes into its own code, which is read-only
 *
 * Each must end the program, and only the program: the kernel says why
 * and goes on, and the test runs another program after it.
 */
#include "usys.h"

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int main(int argc, char **argv)
{
    const char *kind = argc > 1 ? argv[1] : "";
    print("fault: trying ");
    print(kind);
    print("\n");
    if (same(kind, "kernel"))
        print_hex(*(volatile uint64_t *)0x40200000UL);
    else if (same(kind, "null"))
        print_hex(*(volatile uint64_t *)0);
    else if (same(kind, "text"))
        *(volatile uint32_t *)(uintptr_t)main = 0;
    print("fault: nothing stopped it\n");
    return 0;
}
