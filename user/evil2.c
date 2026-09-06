/* evil2 - naughty in another way: it does not reach for kernel memory
 * itself (that is what 'evil' and a page fault are for), it tries to
 * get it done by somebody else's hands.
 *
 * It passes pointers to kernel memory in syscalls: if the kernel
 * trusted them naively, it would write and read at those addresses in
 * ring 0, with full rights - paging protection would give nothing.
 * Every such attempt has to end in a -1 error, and the process has to
 * survive it.
 */

#include "usys.h"

#define KERNEL_ADDR 0x100000u    /* the start of the kernel image */
#define VGA_ADDR    0xB8000u     /* the VGA text mode buffer */

/* A raw syscall - needed because we want to see eax even where the
   usys.h library returns void (sys_time, sys_getcwd). */
static inline int32_t raw(int32_t nr, uint32_t a, uint32_t b, uint32_t c)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "b"(a), "c"(b), "d"(c)
                     : "memory");
    return ret;
}

static int failures;

static void expect_refused(const char *what, int32_t ret)
{
    print("[evil2] ");
    print(what);
    if (ret == -1) {
        print(": refused (-1) OK\n");
    } else {
        print(": IT PASSED! it returned ");
        print_dec((uint32_t)ret);
        print("\n");
        failures++;
    }
}

void _start(void)
{
    print("[evil2] trying to trick the kernel with pointers into its own memory\n");

    /* Printing kernel memory to the console by somebody else's hands */
    expect_refused("write(1, kernel, 32)",
                   raw(SYS_WRITE, 1, KERNEL_ADDR, 32));

    /* Overwriting kernel memory with the date from the RTC */
    expect_refused("time(-> kernel)", raw(SYS_TIME, KERNEL_ADDR, 0, 0));

    /* Overwriting the VGA buffer with memory statistics */
    expect_refused("sysinfo(-> VGA)", raw(SYS_SYSINFO, VGA_ADDR, 0, 0));

    /* Writing a directory path to address zero */
    expect_refused("getcwd(NULL)", raw(SYS_GETCWD, 0, 0, 0));

    /* A file path pointing into kernel memory */
    expect_refused("open(kernel)", raw(SYS_OPEN, KERNEL_ADDR, 0, 0));

    /* A thread entry point inside kernel code */
    expect_refused("thread(-> kernel)", raw(SYS_THREAD, KERNEL_ADDR, 0, 0));

    /* A positive control: an ordinary, honest pointer is to keep
   working */
    struct utime t;
    if (raw(SYS_TIME, (uint32_t)&t, 0, 0) != 0) {
        print("[evil2] ERROR: an honest pointer was refused too!\n");
        failures++;
    }

    if (failures) {
        print("[evil2] A HOLE: the kernel was tricked\n");
        sys_exit(1);
    }
    print("[evil2] the kernel refused every attempt - the boundary holds\n");
    sys_exit(0);
}
