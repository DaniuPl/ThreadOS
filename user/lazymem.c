/* lazymem - paging on demand
 *
 * The process asks for 64 MiB of heap and does not touch it at all:
 * RAM usage does not move, because the heap is for now only a promise.
 * Only the first write to a given address costs a frame - so we write
 * 1 MiB and exactly that much is added.
 *
 * Separately we check a page the program never touched once, but which
 * the kernel fills in inside a syscall: pointer validation has to be
 * able to make it real, otherwise a freshly allocated buffer would look
 * like a bad pointer from the kernel.
 */

#include "usys.h"

#define ASK_MIB   64
#define TOUCH_MIB 1
#define PAGE      4096

static uint32_t used_kib(void)
{
    struct sysinfo si;
    sys_sysinfo(&si);
    return si.used_kib;
}

static void fail(const char *msg)
{
    print("[lazymem] ZLE: ");
    print(msg);
    print("\n");
    sys_exit(1);
}

void _start(void)
{
    uint32_t before = used_kib();

    unsigned char *p = sys_sbrk(ASK_MIB * 1024 * 1024);
    if (p == (void *)-1)
        fail("sbrk odmowil 64 MiB");
    /* The start of the heap usually falls in the middle of a page the
       process already has (the end of the ELF image) - we count from the
       next boundary, so that every touched page is really new */
    p = (unsigned char *)(((uint32_t)p + PAGE - 1) & ~(uint32_t)(PAGE - 1));

    uint32_t after_ask = used_kib();
    print("[lazymem] poproszone ");
    print_dec(ASK_MIB);
    print(" MiB, RAM urosl o ");
    print_dec(after_ask - before);
    print(" KiB\n");
    if (after_ask - before > 64)
        fail("sama prosba o pamiec juz cos kosztowala");

    /* Touch the first megabyte - every page is one frame */
    for (uint32_t i = 0; i < TOUCH_MIB * 1024u * 1024u; i += PAGE)
        p[i] = (unsigned char)(i >> 12);
    uint32_t after_touch = used_kib();
    uint32_t grew = after_touch - after_ask;
    print("[lazymem] dotkniete ");
    print_dec(TOUCH_MIB);
    print(" MiB, RAM urosl o ");
    print_dec(grew);
    print(" KiB\n");
    if (grew < TOUCH_MIB * 1024 || grew > TOUCH_MIB * 1024 + 32)
        fail("przyrost nie odpowiada dotknietym stronom");

    for (uint32_t i = 0; i < TOUCH_MIB * 1024u * 1024u; i += PAGE)
        if (p[i] != (unsigned char)(i >> 12))
            fail("dane w dotknietych stronach sie nie zgadzaja");

    /* A page far beyond the touched area, filled in by the kernel */
    char *far = (char *)(p + 32u * 1024 * 1024);
    sys_getcwd(far);
    if (far[0] != '/')
        fail("syscall nie zapisal nietknietej strony sterty");
    print("[lazymem] syscall urzeczywistnil nietknieta strone sterty\n");

    /* The same for mmap: the area is promised, the frames on demand */
    uint32_t before_map = used_kib();
    unsigned char *m = sys_mmap(8 * 1024 * 1024);
    if (!m)
        fail("mmap odmowil 8 MiB");
    if (used_kib() - before_map > 64)
        fail("mmap przydzielil ramki od razu");
    m[7 * 1024 * 1024] = 0x5A;
    if (m[7 * 1024 * 1024] != 0x5A)
        fail("zapis w obszarze mmap nie doszedl");
    print("[lazymem] mmap tez daje strony dopiero przy dotknieciu\n");
    sys_exit(0);
}
