/* faults - how many page faults the kernel can serve at once
 *
 * The 'cores' program measured counting in ring 3, where the kernel is
 * not touched at all. 'syscalls' measured entering the kernel through
 * the cheapest door there is. This one measures the door that programs
 * really use: a first touch of a page.
 *
 * Asking for memory costs nothing - the page appears only when
 * somebody writes to it, and that write is a fault, an entry into the
 * kernel, a frame taken from the physical allocator, four kilobytes
 * zeroed and one page table entry written. Every program doing
 * anything with memory goes this way thousands of times.
 *
 * Two processes, each in its OWN address space, touching their own
 * pages. There is nothing here they share - and that is the point: if
 * two cores still cannot do it at once, the reason is the lock and not
 * the work.
 *
 * The checksum is not decoration. A fresh page has to be zeroed, and
 * every byte written has to be readable afterwards; a frame handed to
 * two processes at once shows up here as a wrong sum.
 *
 * Usage: run faults    - both processes on the boot core
 *        runw faults   - they are allowed to run anywhere
 */

#include "uapi.h"

#define PAGES   9000u              /* pages touched by one process */
#define PAGE    4096u

static uint32_t touch(uint32_t *mask_out)
{
    uint32_t mask = 0, sum = 0;

    /* The heap is asked for ONCE. What is measured is the touching, not
       the asking: sbrk is an ordinary system call and would put its own
       waiting into the middle of the measurement. */
    unsigned char *base = (unsigned char *)sys_sbrk((int32_t)((PAGES + 1) * PAGE));
    if (!base || base == (unsigned char *)-1) {
        *mask_out = 0;
        return 0;
    }
    base = (unsigned char *)
        (((uint32_t)(uintptr_t)base + PAGE - 1) & ~(PAGE - 1));

    for (uint32_t i = 0; i < PAGES; i++) {
        unsigned char *page = base + i * PAGE;
        /* The write is the fault. What comes back has to be what was
           written - and the last byte of a fresh page has to be zero,
           which a frame handed out twice would not be. */
        page[0] = (unsigned char)(i & 0xFF);
        page[PAGE / 2] = (unsigned char)((i * 7 + 3) & 0xFF);
        sum += page[0] + page[PAGE / 2] + page[PAGE - 1];

        if ((i & 0x3FF) == 0)
            mask |= 1u << (uint32_t)sys_core();
    }
    *mask_out = mask;
    return sum;
}

static void report(const char *who, uint32_t mask, uint32_t ticks,
                   uint32_t sum)
{
    print("[faults] ");
    print(who);
    print(": rdzenie ");
    print_dec(mask);
    print(", tyknac ");
    print_dec(ticks);
    print(", stron ");
    print_dec(PAGES);
    print(", suma ");
    print_dec(sum);
    print("\n");
}

void _start(void)
{
    int32_t pid = sys_fork();
    if (pid < 0) {
        print("[faults] ZLE: fork sie nie udal\n");
        sys_exit(1);
    }

    uint32_t start = sys_getticks();
    uint32_t mask = 0;
    uint32_t sum = touch(&mask);
    uint32_t ticks = sys_getticks() - start;

    if (pid == 0) {
        report("dziecko", mask, ticks, sum);
        sys_exit(0);
    }

    int code = 0;
    sys_wait_child(&code);
    report("rodzic ", mask, ticks, sum);
    print("[faults] gotowe\n");
    sys_exit(0);
}
