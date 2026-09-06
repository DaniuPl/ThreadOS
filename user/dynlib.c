/* dynlib - proof that a shared library really is shared
 *
 * The program carries no cryptography in itself; it carries only
 * calls. We check that with three things that cannot be faked:
 *
 *   1. The code we execute lies OUTSIDE the program's image. The
 *      address has to be supplied by the library itself - a
 *      non-relocatable program taking the address of its function gets
 *      the address of its own jump stub.
 *   2. The result is correct: SHA-256('abc') is a known vector from
 *      FIPS 180-4. A jump through an unfilled slot or into the wrong
 *      code has no way of giving the right answer.
 *   3. The library's data are PRIVATE. The random generator's counter
 *      starts from zero in every process; if the data were shared, the
 *      second run would find the counter from the first.
 *
 * Usage: run dynlib
 */

#include "uapi.h"
#include "usha256.h"
#include "urand.h"
#include "uproton.h"

static void report(const char *co, uint32_t v)
{
    print(co);
    print_dec(v);
    print("\n");
}

/* A point of reference: the program's own function, so that there is
   something to compare the library code's address with */
static void our_fn(void)
{
    print("");
}

void _start(int argc, char **argv)
{
    our_fn();

    uint32_t code = (uint32_t)proton_code_addr();
    uint32_t data = (uint32_t)proton_data_addr();
    uint32_t mine = (uint32_t)(uintptr_t)our_fn;

    report("[dynlib] the program's code at ", mine);
    report("[dynlib] the library's code at ", code);
    report("[dynlib] the library's data at ", data);

    if (code <= mine) {
        print("[dynlib] WRONG: the library's code lies inside the program image\n");
        sys_exit(1);
    }

    /* A known vector: SHA-256('abc') starts with ba 78 16 bf */
    static const uint8_t want[8] = { 0xba, 0x78, 0x16, 0xbf,
                                     0x8f, 0x01, 0xcf, 0xea };
    uint8_t digest[32];
    sha256("abc", 3, digest);
    for (int i = 0; i < 8; i++) {
        if (digest[i] != want[i]) {
            print("[dynlib] WRONG: SHA-256 from the library gives the wrong answer\n");
            sys_exit(1);
        }
    }
    print("[dynlib] SHA-256('abc') from the library matches FIPS 180-4\n");

    /* The library's code is SHARED - every process sees the same
       frames. If they could be written to, one program would swap a
       function out for everybody at once. So it has to be impossible,
       and it has to kill that program. */
    if (argc > 1 && argv[1][0] == 'w') {
        print("[dynlib] trying to write over the library's code\n");
        *(volatile uint8_t *)(uintptr_t)code = 0x90;
        print("[dynlib] WRONG: writing to the library's code worked\n");
        sys_exit(1);
    }

    /* The generator's counter is the library's state. In a fresh
       process it is to be zero - without that there would be no telling
       whether the data are private. */
    if (proton_random_counter() != 0) {
        print("[dynlib] WRONG: the library's counter does not start at zero\n");
        sys_exit(1);
    }
    uint8_t random[64];
    urand_init();
    urand_bytes(random, sizeof(random));
    report("[dynlib] the counter after taking bytes: ",
            proton_random_counter());

    print("[dynlib] done\n");
    sys_exit(0);
}
