/* System call handling in long mode
 *
 * The syscall/sysret instructions have to be enabled first, and the
 * processor told three things:
 *
 *   EFER.SCE - permission to use these instructions at all,
 *   STAR     - which selectors to build the segments from on entry
 *              and on return,
 *   LSTAR    - what address to jump to,
 *   SFMASK   - which flags to clear on entry; we clear IF so that the
 *              kernel starts work with interrupts off, just as with a
 *              gate.
 */

#include "../kernel/include/sem.h"
#include <stdint.h>
#include "syscall64.h"
#include "../kernel/include/video.h"
#include "../kernel/include/smp.h"
#include "../kernel/include/user.h"
#include "../kernel/include/install.h"
#include "../kernel/include/hda.h"
#include "../kernel/include/clip.h"
#include "../kernel/include/config.h"
#include "../kernel/include/biglock.h"
#include "gdt64.h"
#include "task64.h"
#include "paging64.h"
#include "../kernel/include/pmm.h"
#include "../kernel/include/string.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/pit.h"
#include "../kernel/include/isr.h"
#include "proc64.h"
#include "signal64.h"
#include "keyboard64.h"
#include "../kernel/include/fs.h"
#include "elf64.h"
#include "pipe64.h"
#include "../kernel/include/net.h"
#include "../kernel/include/tcp.h"
#include "../kernel/include/desktop.h"
#include "../kernel/include/shm.h"
#include "../kernel/include/rtc.h"

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084
#define MSR_KERNEL_GS 0xC0000102

#define EFER_SCE   (1u << 0)
#define RFLAGS_IF  0x200

extern void syscall64_entry(void);

/* The core's data block, seen by the entry stub through the GS
   register. The layout has to match boot64/syscall64.S. */
struct core_syscall {
    uint64_t kernel_rsp;     /* offset 0 */
    uint64_t scratch;        /* offset 8 */
};
extern struct core_syscall syscall64_blocks[];
_Static_assert(sizeof(struct core_syscall) == 16,
               "the stub counts sixteen bytes per core");

static int user_ran;
static uint32_t user_cs;
static int user_exited;
static int user_status;
static int user_exits, user_failures;

static void wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr"
                     : : "c"(msr), "a"((uint32_t)value),
                         "d"((uint32_t)(value >> 32)));
}

static uint64_t rdmsr(uint32_t msr)
{
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

/* Called on EVERY core: all these registers are its own. A core that
   does not set them will not accept the syscall instruction at all. */
void syscall64_init(void)
{
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);

    /* Pointer to this core's block. The entry stub reaches for it
       through swapgs - the only way with no free register. */
    wrmsr(MSR_KERNEL_GS,
          (uint64_t)(uintptr_t)&syscall64_blocks[smp_core()]);

    /* Bits 47-32: the kernel code selector on entry.
       Bits 63-48: the base from which sysret builds the program's
       selectors - data is base + 8, code is base + 16. That is why we
       write the kernel DATA selector here, not the user code one. */
    wrmsr(MSR_STAR, ((uint64_t)GDT64_KERNEL_DATA << 48) |
                    ((uint64_t)GDT64_KERNEL_CODE << 32));
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall64_entry);
    wrmsr(MSR_SFMASK, RFLAGS_IF);
}

void syscall64_set_kernel_stack(uint64_t rsp)
{
    syscall64_blocks[smp_core()].kernel_rsp = rsp;
    gdt64_set_kernel_stack(rsp);   /* the same for interrupts from ring 3 */
}

int syscall64_user_ran(void)
{
    return user_ran;
}

uint32_t syscall64_user_cs(void)
{
    return user_cs;
}

int syscall64_user_exited(void)
{
    return user_exited;
}

int syscall64_user_status(void)
{
    return user_status;
}

int syscall64_user_exits(void)
{
    return user_exits;
}

int syscall64_user_failures(void)
{
    return user_failures;
}

void syscall64_forget_user(void)
{
    user_ran = 0;
    user_cs = 0;
    user_exited = 0;
    user_status = 0;
    user_exits = 0;
    user_failures = 0;
}

/* Called from the stub in boot64/syscall64.S.
 *
 * The stub lays down exactly the frame an interrupt lays down and
 * passes a pointer to it. The call number is in RAX, the arguments in
 * RDI and RSI, and the result goes back where the program looks for it
 * - into the frame's rax field, because that is what the processor
 * restores the registers from on return.
 *
 * Pointers from ring 3 must not be taken on trust: before the kernel
 * reads anything through them, it asks the page tables whether the
 * program has any right to reach there at all. Without that it would
 * be enough to pass a kernel memory address to have it printed out. */
/* A file name from program memory into the kernel. Returns 0 or -1. */
static int name_from_ring3(uint64_t ptr, uint64_t len, char *out)
{
    if (len == 0 || len > FS_NAME_MAX || !paging64_user_range(ptr, len))
        return -1;
    const char *src = (const char *)(uintptr_t)ptr;
    for (uint64_t i = 0; i < len; i++)
        out[i] = src[i];
    out[len] = '\0';
    return 0;
}

/* A zero-terminated string from program memory into the kernel. We do
   not know the length up front, so every byte has to be checked
   separately: the string may reach the last byte of a mapping and not
   one further. Returns 0, or -1 when the string runs past the
   program's memory or does not fit in the buffer. */
static int text_from_ring3(uint64_t ptr, char *out, int max)
{
    for (int i = 0; i < max - 1; i++) {
        if (!paging64_user_range(ptr + (uint64_t)i, 1))
            return -1;
        out[i] = *(const char *)(uintptr_t)(ptr + (uint64_t)i);
        if (!out[i])
            return 0;
    }
    out[max - 1] = '\0';
    return 0;
}

/* Drawing commands passed by the program. The layout has to match the
   one in user64/usys64.h - in long mode a pointer is eight bytes wide
   and needs alignment, hence the hole after the three numbers. */
struct win_rect_cmd64 {
    int32_t x, y, w, h, color;
};
struct win_text_cmd64 {
    int32_t x, y, color;
    uint64_t text;
};
struct win_event64 {
    int32_t typ;
    int32_t x, y;
    int32_t val;
};

/* A socket description passed by the program. The layout has to match
   the one in user64/usys64.h. */
struct udp_info {
    int sock;
    uint32_t ip;
    uint16_t port;
    uint16_t _padding;
    uint32_t timeout;
};

static void handle(struct regs *r);

/* Entry from the stub. We deliver the signal just before returning -
   then and only then does the frame describe the program, not the
   kernel. */
/* --- which calls do not need the big kernel lock ------------------
 *
 * There is one lock for the whole kernel and every entry from ring 3
 * takes it, because the drivers, the file system and page swapping
 * were written for a single core and quietly count on it. But not
 * every call reaches there. The ones below touch ONLY the calling
 * task's state and things that have locks of their own - two cores can
 * be inside them at once and nothing comes of it.
 *
 * The list is short and explicit on purpose: adding a call to it is a
 * promise that it was read to the end, together with whatever it calls
 * further on. The default answer is "with the lock" and that is how it
 * should stay.
 */
static int lockless(uint64_t nr)
{
    switch (nr) {
    case SYS64_GETPID:   /* own number, a single task field */
    case SYS64_TICKS:    /* the timer interrupt counter, a single word */
    case SYS64_RING:     /* own code selector from the interrupt frame */
    case SYS64_CORE:    /* a question to our own local APIC */
    case SYS64_SLEEP:    /* our own "wake me up" and the scheduler, which
                            has a lock of its own - and across a context
                            switch the big lock is not held anyway */
        return 1;
    default:
        return 0;
    }
}

/* A system call comes from ring 3, so it takes the big kernel lock:
   a program may run on any core, but inside the kernel there is to be
   one at a time. The syscall instruction does not go through the
   interrupt table, so this is the only place where that can be done
   for it. */
void syscall64_handler(struct regs *r)
{
    if (lockless(r->rax)) {
        handle(r);
        /* A signal changes the program's stack and return address - that
           is no longer work "on our own only". We take the lock only
           when there really is something to deliver, though. */
        if ((r->cs & 3) == 3 && task64_signal_pending()) {
            biglock_enter();
            signal64_deliver(r);
            biglock_leave();
        }
        return;
    }

    biglock_enter();
    handle(r);
    if ((r->cs & 3) == 3)
        signal64_deliver(r);
    biglock_leave();
}

static void handle(struct regs *r)
{
    /* The fourth argument travels in R10, not in RCX: the syscall
       instruction writes the return address into RCX, so RCX has no
       chance of carrying an argument. Linux does the same and for the
       same reason. */
    uint64_t nr = r->rax, a1 = r->rdi, a2 = r->rsi, a3 = r->rdx,
             a4 = r->r10;

    switch (nr) {
    case SYS64_WRITE: {
        const char *buf = (const char *)(uintptr_t)a2;
        if (a3 > 4096)
            a3 = 4096;               /* clamp first, check second -
                                        otherwise a length of the order
                                        of billions of pages would tie
                                        the kernel up for good */
        if (!paging64_user_range(a2, a3)) {
            printk("[kernel] write: the pointer %x does not belong to the program\n",
                   (uint32_t)a2);
            r->rax = (uint64_t)-1;
            return;
        }
        /* Where a write goes is decided by the KIND of the descriptor,
           not by its number. One with no redirection is the console -
           but after a dup2 it may be the end of a pipe, and the program
           does not see that. */
        int kind = task64_fd_kind((int)a1);
        if (kind == FDK_FREE) {
            if (a1 != FD_STDOUT) {
                r->rax = (uint64_t)-1;
                return;
            }
            for (uint64_t i = 0; i < a3; i++)
                printk("%c", buf[i]);
            r->rax = a3;
            return;
        }
        if (kind == FDK_POTOK_W) {
            int n = pipe64_write(task64_fd_slot((int)a1), buf, (uint32_t)a3);
            r->rax = (uint64_t)(int64_t)n;
            return;
        }
        if (kind != FDK_FILE) {
            r->rax = (uint64_t)-1;   /* write into the read end */
            return;
        }
        int slot = task64_fd_slot((int)a1);
        /* A file behind descriptor ONE is a redirected stdout, and a
           redirected stdout is shared: the program and everything it
           starts write into it. Each of them keeps an offset of its
           own, so the only place they can all agree on is the end -
           otherwise a shell's next prompt lands on top of what its
           child has just written. */
        uint32_t at = task64_fd_pos((int)a1);
        if (a1 == FD_STDOUT) {
            int sz = fs_size(slot);
            at = sz > 0 ? (uint32_t)sz : 0;
        }
        int n = fs_write_at(slot, at, buf, (uint32_t)a3);
        if (n > 0)
            task64_fd_seek((int)a1, at + (uint32_t)n);
        r->rax = (uint64_t)(int64_t)n;
        return;
    }
    case SYS64_EXIT:
        user_status = (int)a1;
        user_exited = 1;
        user_exits++;
        if (a1)
            user_failures++;
        task64_exit_code((int)a1);   /* does not return - the scheduler goes on */
        return;
    case SYS64_RING:                 /* the program says which ring it is in */
        user_cs = (uint32_t)a1;
        user_ran = 1;
        r->rax = 0;
        return;
    case SYS64_SLEEP:
        task64_sleep((uint32_t)a1);
        r->rax = 0;
        return;
    case SYS64_TICKS:
        r->rax = pit_ticks();
        return;
    case SYS64_FORK:
        r->rax = (uint64_t)proc64_fork(r);
        return;
    case SYS64_EXEC:
        /* On success it does not return where it was called from: the
           frame already describes the new program and the old one does
           not exist */
        r->rax = (uint64_t)proc64_exec(r, (const char *)(uintptr_t)a1,
                                       a2, a3);
        return;
    case SYS64_WAIT: {
        int code = 0;
        int id = task64_wait(&code);
        if (id >= 0 && a1) {
            if (!paging64_user_range(a1, sizeof(int)))
                return;              /* a bad pointer - the code is lost */
            *(int *)(uintptr_t)a1 = code;
        }
        r->rax = (uint64_t)(int64_t)id;
        return;
    }
    case SYS64_READ: {
        if (a3 > 4096)
            a3 = 4096;
        if (!paging64_user_range(a2, a3)) {
            printk("[kernel] read: the pointer %x does not belong to the program\n",
                   (uint32_t)a2);
            r->rax = (uint64_t)-1;
            return;
        }
        int kind = task64_fd_kind((int)a1);
        if (kind == FDK_FREE) {
            if (a1 != FD_STDIN) {
                r->rax = (uint64_t)-1;
                return;
            }
            int n = keyboard64_read((char *)(uintptr_t)a2, (uint32_t)a3);
            r->rax = (uint64_t)(int64_t)n;
            return;
        }
        if (kind == FDK_POTOK_R) {
            int n = pipe64_read(task64_fd_slot((int)a1),
                                (char *)(uintptr_t)a2, (uint32_t)a3);
            r->rax = (uint64_t)(int64_t)n;
            return;
        }
        if (kind != FDK_FILE) {
            r->rax = (uint64_t)-1;   /* read from the write end */
            return;
        }
        int slot = task64_fd_slot((int)a1);
        int n = fs_read_at(slot, task64_fd_pos((int)a1),
                           (void *)(uintptr_t)a2, (uint32_t)a3);
        if (n > 0)
            task64_fd_advance((int)a1, (uint32_t)n);
        r->rax = (uint64_t)(int64_t)n;
        return;
    }
    case SYS64_OPEN: {
        char name[FS_NAME_MAX + 1];
        if (a2 == 0 || a2 > FS_NAME_MAX ||
            !paging64_user_range(a1, a2)) {
            r->rax = (uint64_t)-1;
            return;
        }
        const char *src = (const char *)(uintptr_t)a1;
        for (uint64_t i = 0; i < a2; i++)
            name[i] = src[i];
        name[a2] = '\0';

        /* The system's own files may be read by anybody; opening one
           to create or truncate it is changing the system. */
        if (a3 && user_is_system_file(name) &&
            task64_level() != USER_ADMIN) {
            r->rax = (uint64_t)-2;              /* not allowed */
            return;
        }

        int slot = fs_open(task64_cwd(), name, (int)(a3 & OPEN64_CREATE));
        if (slot < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        if (a3 & OPEN64_TRUNC)
            fs_truncate(slot);
        r->rax = (uint64_t)(int64_t)task64_fd_open(slot);
        return;
    }
    case SYS64_SBRK: {
        /* We return the old limit - the program gets the address it may
           start writing from. A shift of zero is the question "where am
           I". */
        int64_t delta = (int64_t)a1;
        uint64_t old_one = task64_brk();
        if (!old_one) {
            r->rax = (uint64_t)-1;
            return;
        }
        uint64_t fresh = old_one + (uint64_t)delta;
        if (fresh < task64_brk_base() || fresh >= USER64_TOP) {
            r->rax = (uint64_t)-1;   /* below its own memory or into the stack */
            return;
        }
        /* Shrinking gives the pages back at once - otherwise the memory
           would return to the pool only together with the whole
           process */
        for (uint64_t va = fresh & ~0xFFFULL; va + 4096 <= old_one;
             va += 4096) {
            if (va < fresh)
                continue;
            uint64_t f = paging64_unmap(va);
            if (f)
                pmm_free_frame((uint32_t)f);
        }
        task64_set_brk(fresh);
        r->rax = old_one;
        return;
    }
    case SYS64_MKDIR: {
        char name[FS_NAME_MAX + 1];
        if (name_from_ring3(a1, a2, name) < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        r->rax = (uint64_t)(int64_t)fs_mkdir(task64_cwd(), name);
        return;
    }
    case SYS64_CHDIR: {
        char name[FS_NAME_MAX + 1];
        int fresh_one = FS_ROOT;
        if (name_from_ring3(a1, a2, name) < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        if (fs_chdir(task64_cwd(), name, &fresh_one) < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        task64_set_cwd(fresh_one);
        r->rax = 0;
        return;
    }
    case SYS64_GETCWD: {
        char path[FS_NAME_MAX * 4];
        if (a2 == 0 || !paging64_user_range(a1, a2)) {
            r->rax = (uint64_t)-1;
            return;
        }
        fs_getpath(task64_cwd(), path);
        char *dst = (char *)(uintptr_t)a1;
        uint64_t i = 0;
        while (i + 1 < a2 && path[i]) {
            dst[i] = path[i];
            i++;
        }
        dst[i] = '\0';
        r->rax = i;
        return;
    }
    case SYS64_READDIR: {
        /* One entry per call - the program asks for successive numbers
           as long as it keeps getting an answer. The buffer is described
           by struct dirent64 from usys64.h and lives on the program's
           side. */
        struct {
            char name[FS_NAME_MAX + 1];
            uint32_t size;
            int is_dir;
        } entry;

        if (!paging64_user_range(a2, sizeof(entry))) {
            r->rax = (uint64_t)-1;
            return;
        }
        uint32_t size = 0;
        int dir = 0;
        /* fs_readdir returns 1 on a hit and 0 at the end of the
           directory - not a negative value. Checking for "less than
           zero" ends in an endless loop here, because the end of the
           directory looks like success. */
        if (fs_readdir(task64_cwd(), (int)a1, entry.name,
                       &size, &dir) != 1) {
            r->rax = (uint64_t)-1;
            return;
        }
        entry.size = size;
        entry.is_dir = dir;
        memcpy((void *)(uintptr_t)a2, &entry, sizeof(entry));
        r->rax = 0;
        return;
    }
    case SYS64_UNLINK: {
        char name[FS_NAME_MAX + 1];
        if (name_from_ring3(a1, a2, name) < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        if (user_is_system_file(name) && task64_level() != USER_ADMIN) {
            r->rax = (uint64_t)-2;
            return;
        }
        r->rax = (uint64_t)(int64_t)fs_unlink(task64_cwd(), name);
        return;
    }
    case SYS64_PIPE: {
        /* The program gets two numbers: the read end and the write end.
           As long as it holds both, the pipe will never end - which is
           why the shell closes both of its own as soon as it has handed
           them to the children. */
        if (!paging64_user_range(a1, 2 * sizeof(int))) {
            r->rax = (uint64_t)-1;
            return;
        }
        int idx = pipe64_new();
        if (idx < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        int rfd = task64_fd_pipe(idx, 1);
        int wfd = task64_fd_pipe(idx, 0);
        if (rfd < 0 || wfd < 0) {
            if (rfd >= 0)
                task64_fd_close(rfd);
            if (wfd >= 0)
                task64_fd_close(wfd);
            else
                pipe64_unref(idx, 0);
            r->rax = (uint64_t)-1;
            return;
        }
        int *out = (int *)(uintptr_t)a1;
        out[0] = rfd;
        out[1] = wfd;
        r->rax = 0;
        return;
    }
    case SYS64_DUP2:
        r->rax = (uint64_t)(int64_t)task64_fd_dup2((int)a1, (int)a2);
        return;
    /* --- Network ------------------------------------------------
     *
     * A socket belongs to the task that opened it - the kernel keeps
     * its number and cleans up after its death. Thanks to that a
     * program cannot reach somebody else's socket by passing its
     * number. */
    case SYS64_NETINFO: {
        /* Four addresses: own, mask, gateway, DNS */
        if (!paging64_user_range(a1, 4 * sizeof(uint32_t))) {
            r->rax = (uint64_t)-1;
            return;
        }
        uint32_t *out = (uint32_t *)(uintptr_t)a1;
        out[0] = net_ip();
        out[1] = net_mask();
        out[2] = net_gw();
        out[3] = net_dns();
        r->rax = (uint64_t)net_configured();
        return;
    }
    case SYS64_UDP_OPEN:
        r->rax = (uint64_t)(int64_t)net_udp_open((uint16_t)a1,
                                                 task64_current_id());
        return;
    case SYS64_UDP_CLOSE:
        net_udp_close((int)a1, task64_current_id());
        r->rax = 0;
        return;
    case SYS64_UDP_SEND: {
        /* Everything except the data itself travels in one description.
           Three registers for arguments are too few for a socket, an
           address, a port, a length and a timeout - and packing two
           things into one register reads terribly and breaks at the
           first change. */
        struct udp_info info;
        if (!paging64_user_range(a1, sizeof(info))) {
            r->rax = (uint64_t)-1;
            return;
        }
        memcpy(&info, (const void *)(uintptr_t)a1, sizeof(info));
        if (a3 > 1400 || !paging64_user_range(a2, a3)) {
            r->rax = (uint64_t)-1;
            return;
        }
        r->rax = (uint64_t)(int64_t)net_udp_send(
            info.sock, task64_current_id(), info.ip, info.port,
            (const void *)(uintptr_t)a2, (uint16_t)a3);
        return;
    }
    case SYS64_UDP_RECV: {
        struct udp_info info;
        if (!paging64_user_range(a1, sizeof(info))) {
            r->rax = (uint64_t)-1;
            return;
        }
        memcpy(&info, (const void *)(uintptr_t)a1, sizeof(info));
        if (a3 > 1400 || !paging64_user_range(a2, a3)) {
            r->rax = (uint64_t)-1;
            return;
        }

        uint32_t from_ip = 0;
        uint16_t from_port = 0;
        int n = net_udp_recv(info.sock, task64_current_id(),
                             &from_ip, &from_port,
                             (void *)(uintptr_t)a2, (uint16_t)a3,
                             info.timeout);
        if (n > 0) {
            /* Who sent it - the program asks about that through the same
           description */
            info.ip = from_ip;
            info.port = from_port;
            memcpy((void *)(uintptr_t)a1, &info, sizeof(info));
        }
        r->rax = (uint64_t)(int64_t)n;
        return;
    }
    case SYS64_TCP_CONNECT: {
        /* The same description as for UDP - the port and the timeout have
           their place there, and the address is where it can be seen */
        struct udp_info info;
        if (!paging64_user_range(a1, sizeof(info))) {
            r->rax = (uint64_t)-1;
            return;
        }
        memcpy(&info, (const void *)(uintptr_t)a1, sizeof(info));
        r->rax = (uint64_t)(int64_t)tcp_connect(info.ip, info.port,
                                                task64_current_id(),
                                                info.timeout);
        return;
    }
    case SYS64_TCP_SEND:
        if (a3 > 1400 || !paging64_user_range(a2, a3)) {
            r->rax = (uint64_t)-1;
            return;
        }
        r->rax = (uint64_t)(int64_t)tcp_send(
            (int)a1, task64_current_id(), (const void *)(uintptr_t)a2,
            (uint16_t)a3, 5000);
        return;
    case SYS64_TCP_RECV:
        if (a3 > 1400 || !paging64_user_range(a2, a3)) {
            r->rax = (uint64_t)-1;
            return;
        }
        r->rax = (uint64_t)(int64_t)tcp_recv(
            (int)a1, task64_current_id(), (void *)(uintptr_t)a2,
            (uint16_t)a3, 5000);
        return;
    case SYS64_TCP_CLOSE:
        tcp_close((int)a1, task64_current_id());
        r->rax = 0;
        return;
    case SYS64_CLOSE:
        r->rax = (uint64_t)(int64_t)task64_fd_close((int)a1);
        return;
    case SYS64_SIGNAL:
        /* The program supplies its own handler and the address to return
           to once it finishes. The latter has to come from it:
           executable code lives only in its own pages. */
        task64_set_sighandler(a2, a3);
        r->rax = 0;
        return;
    case SYS64_SIGRETURN:
        if (signal64_return(r) < 0) {
            printk("[kernel] sigreturn: the frame is outside the program's memory\n");
            task64_kill_current("a broken sigreturn");
        }
        return;                      /* the frame already describes the interrupted spot */
    case SYS64_KILL:
        /* A plain user may signal only their own processes: a level of
           its own is what makes a process somebody else's. */
        if (task64_level() != USER_ADMIN &&
            task64_level_of((int)a1) != task64_level()) {
            r->rax = (uint64_t)-2;
            return;
        }
        r->rax = task64_signal((int)a1, (int)a2) ? 0 : (uint64_t)-1;
        return;

    /* --- who is at the machine: up by a password, down for free --- */
    case SYS64_AUTH: {
        char pass[CONFIG_VAL_MAX];
        if (text_from_ring3(a1, pass, sizeof(pass)) < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        if (!user_password_ok(pass)) {
            r->rax = (uint64_t)-2;
            return;
        }
        task64_set_level(USER_ADMIN);
        r->rax = 0;
        return;
    }
    case SYS64_DROP:
        task64_set_level(USER_PLAIN);
        r->rax = 0;
        return;
    case SYS64_LEVEL:
        r->rax = (uint64_t)(int64_t)task64_level();
        return;
    case SYS64_CLIP_SET: {
        uint64_t len = a2 > CLIP_MAX ? CLIP_MAX : a2;
        if (len && !paging64_user_range(a1, len)) {
            r->rax = (uint64_t)-1;
            return;
        }
        r->rax = clip_set((const char *)(uintptr_t)a1, (uint32_t)len);
        return;
    }
    case SYS64_CLIP_GET: {
        uint64_t max = a2 > CLIP_MAX + 1 ? CLIP_MAX + 1 : a2;
        if (!max || !paging64_user_range(a1, max)) {
            r->rax = (uint64_t)-1;
            return;
        }
        r->rax = clip_get((char *)(uintptr_t)a1, (uint32_t)max);
        return;
    }
    case SYS64_SEEK:
        r->rax = (uint64_t)(int64_t)task64_fd_seek((int)a1, (uint32_t)a2);
        return;
    case SYS64_TRYWAIT: {
        int code = 0;
        int rc = task64_trywait((int)a1, &code);
        if (rc == 0 && a2) {
            if (!paging64_user_range(a2, sizeof(int))) {
                r->rax = (uint64_t)-1;
                return;
            }
            *(int *)(uintptr_t)a2 = code;
        }
        r->rax = (uint64_t)(int64_t)rc;
        return;
    }
    /* Windows. The owner is the process, not whoever passes a window
       number: the compositor checks it on every draw, so a program will
       not draw anything in somebody else's window. */
    case SYS64_WIN_CREATE: {
        char title[64];
        if (text_from_ring3(a3, title, sizeof(title)) < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        r->rax = (uint64_t)(int64_t)desktop_win_create(
            task64_current_id(), (int)a1, (int)a2, title);
        return;
    }
    case SYS64_WIN_RECT: {
        struct win_rect_cmd64 c;
        if (!paging64_user_range(a2, sizeof(c))) {
            r->rax = (uint64_t)-1;
            return;
        }
        c = *(const struct win_rect_cmd64 *)(uintptr_t)a2;
        r->rax = (uint64_t)(int64_t)desktop_win_rect(
            (int)a1, task64_current_id(), c.x, c.y, c.w, c.h,
            (uint8_t)c.color);
        return;
    }
    case SYS64_WIN_TEXT: {
        struct win_text_cmd64 c;
        char s[128];
        if (!paging64_user_range(a2, sizeof(c))) {
            r->rax = (uint64_t)-1;
            return;
        }
        c = *(const struct win_text_cmd64 *)(uintptr_t)a2;
        if (text_from_ring3(c.text, s, sizeof(s)) < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        r->rax = (uint64_t)(int64_t)desktop_win_text(
            (int)a1, task64_current_id(), c.x, c.y, s, (uint8_t)c.color);
        return;
    }
    /* Shared memory. The region lives in the kernel and is common to
       everyone who gives the same key; a process gets it at the next
       free address of its mapping area. */
    case SYS64_SHM:
        r->rax = proc64_shm((int)a1, a2);
        return;
    case SYS64_SYSINFO: {
        /* The same thing 'mem' and 'uptime' show - only to a program.
           Nothing here is a secret: the numbers describe the machine,
           not somebody else's processes. */
        struct {
            uint32_t ram_kib;
            uint32_t used_kib;
            uint32_t uptime_s;
        } info;

        if (!paging64_user_range(a1, sizeof(info))) {
            r->rax = (uint64_t)-1;
            return;
        }
        info.ram_kib = pmm_ram_kib();
        info.used_kib = pmm_used_frames() * 4;
        info.uptime_s = pit_ticks() / 100;
        *(typeof(info) *)(uintptr_t)a1 = info;
        r->rax = 0;
        return;
    }
    case SYS64_MMAP_FILE: {
        char name[FS_NAME_MAX + 1];
        if (name_from_ring3(a1, a2, name) < 0 ||
            !paging64_user_range(a3, sizeof(uint64_t))) {
            r->rax = 0;
            return;
        }
        uint64_t size = 0;
        uint64_t addr = proc64_mmap_file(name, &size, a4);
        if (addr)
            *(uint64_t *)(uintptr_t)a3 = size;
        r->rax = addr;
        return;
    }
    case SYS64_MSYNC:
        r->rax = proc64_msync();
        return;
    /* The core number. The program has no way of reading it itself -
       the local APIC is outside its space. */
    case SYS64_CORE:
        r->rax = (uint64_t)(uint32_t)smp_core();
        return;
    case SYS64_MMAP:
        r->rax = proc64_mmap(a1);
        return;
    case SYS64_THREAD:
        /* A thread starts in ring 3, so its entry point has to be the
           program's own code - and the program is the only one that
           can say where that is */
        r->rax = paging64_user_range(a1, 1)
                     ? (uint64_t)(int64_t)proc64_thread(a1, a2)
                     : (uint64_t)(int64_t)-1;
        return;
    case SYS64_GETPID:
        r->rax = (uint64_t)(int64_t)task64_current_id();
        return;
    case SYS64_TIME: {
        struct rtc_time t;
        if (!paging64_user_range(a1, sizeof(t))) {
            r->rax = (uint64_t)-1;
            return;
        }
        rtc_read(&t);
        *(struct rtc_time *)(uintptr_t)a1 = t;
        r->rax = 0;
        return;
    }
    case SYS64_WIN_POLL: {
        struct win_event64 ev;
        int got = desktop_win_poll((int)a1, task64_current_id(), &ev);
        if (got == 1) {
            if (!paging64_user_range(a2, sizeof(ev))) {
                r->rax = (uint64_t)-1;
                return;
            }
            *(struct win_event64 *)(uintptr_t)a2 = ev;
        }
        r->rax = (uint64_t)(int64_t)got;
        return;
    }
    case SYS64_PS: {
        struct psinfo64 info;
        if (!paging64_user_range(a2, sizeof(info)) ||
            task64_ps((int)a1, &info) < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        *(struct psinfo64 *)(uintptr_t)a2 = info;
        r->rax = 0;
        return;
    }
    case SYS64_RENAME: {
        char from[FS_NAME_MAX + 1], to[FS_NAME_MAX + 1];
        if (name_from_ring3(a1, a2, from) < 0 ||
            name_from_ring3(a3, a4, to) < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        /* The system's own files on both sides: renaming one away is
           as good as deleting it, and renaming onto its name puts a
           stranger's file where the system looks for its own. */
        if ((user_is_system_file(from) || user_is_system_file(to)) &&
            task64_level() != USER_ADMIN) {
            r->rax = (uint64_t)-2;
            return;
        }
        r->rax = (uint64_t)(int64_t)fs_rename(task64_cwd(), from, to);
        return;
    }
    case SYS64_WIN_BLIT: {
        struct win_blit_cmd64 {
            int32_t x, y, w, h;
            uint64_t pixels;
        } c;
        if (!paging64_user_range(a2, sizeof(c))) {
            r->rax = (uint64_t)-1;
            return;
        }
        c = *(const struct win_blit_cmd64 *)(uintptr_t)a2;
        if (c.w <= 0 || c.h <= 0 || c.w > 1024 || c.h > 1024 ||
            c.w * c.h > 4096 ||
            !paging64_user_range(c.pixels, (uint64_t)(c.w * c.h))) {
            r->rax = (uint64_t)-1;
            return;
        }
        r->rax = (uint64_t)(int64_t)desktop_win_blit(
            (int)a1, task64_current_id(), c.x, c.y, c.w, c.h,
            (const uint8_t *)(uintptr_t)c.pixels);
        return;
    }
    /* --- installing onto this machine's own disk -------------------
     *
     * Looking is reading and is allowed to anybody. Writing is not:
     * behind it is the disk of a machine that boots without us. */
    case SYS64_INSTALL_LOOK: {
        struct install_info info;
        if (!paging64_user_range(a2, sizeof(info))) {
            r->rax = (uint64_t)-1;
            return;
        }
        int can = install_look((int)a1, (int)(int64_t)a3, &info);
        *(struct install_info *)(uintptr_t)a2 = info;
        r->rax = (uint64_t)(int64_t)can;
        return;
    }
    case SYS64_INSTALL_STEP:
        if (task64_level() != USER_ADMIN) {
            r->rax = (uint64_t)(int64_t)-2;
            return;
        }
        r->rax = (uint64_t)(int64_t)install_step((int)a1);
        return;

    /* --- sound ------------------------------------------------------
     *
     * Loudness is a knob and not a change to the system, so nothing
     * here asks who is turning it. */
    case SYS64_SND_GET: {
        struct snd_info info;
        if (!paging64_user_range(a1, sizeof(info))) {
            r->rax = (uint64_t)-1;
            return;
        }
        info.present = (uint32_t)hda_present();
        info.volume = (uint32_t)hda_volume();
        info.muted = (uint32_t)hda_muted();
        info.steps = (uint32_t)hda_volume_steps();
        *(struct snd_info *)(uintptr_t)a1 = info;
        r->rax = 0;
        return;
    }
    case SYS64_SND_SET:
        if ((int64_t)a1 >= 0)
            hda_set_volume((int)a1);
        if ((int64_t)a2 >= 0)
            hda_set_mute((int)a2);
        hda_remember();
        r->rax = 0;
        return;
    case SYS64_SND_BEEP:
        r->rax = (uint64_t)(int64_t)hda_beep((uint32_t)a1, (uint32_t)a2);
        return;

    /* --- semaphores: waiting for one another ----------------------- */
    case SYS64_SEM_GET:
        r->rax = (uint64_t)(int64_t)sem_get((int)a1, (int)a2);
        return;
    case SYS64_SEM_WAIT:
        r->rax = (uint64_t)(int64_t)sem_wait((int)a1);
        return;
    case SYS64_SEM_POST:
        r->rax = (uint64_t)(int64_t)sem_post((int)a1);
        return;

    case SYS64_WIN_DRAW: {
        struct win_draw_cmd64 {
            int32_t op, x, y, w, h, r;
            uint32_t rgb, rgb2;
            int32_t a, font, px;
            uint64_t s;
        } c;
        char s[256];
        if (!paging64_user_range(a2, sizeof(c))) {
            r->rax = (uint64_t)-1;
            return;
        }
        c = *(const struct win_draw_cmd64 *)(uintptr_t)a2;
        if (c.s && text_from_ring3(c.s, s, sizeof(s)) < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        struct win_draw d = { c.op, c.x, c.y, c.w, c.h, c.r, c.rgb, c.rgb2,
                              c.a, c.font, c.px };
        r->rax = (uint64_t)(int64_t)desktop_win_draw(
            (int)a1, task64_current_id(), &d, c.s ? s : (const char *)0);
        return;
    }
    case SYS64_UI_LOOK: {
        if (!paging64_user_range(a1, UI_LOOK_N * sizeof(uint32_t))) {
            r->rax = (uint64_t)-1;
            return;
        }
        desktop_ui_look((int)a2, (uint32_t *)(uintptr_t)a1);
        r->rax = 0;
        return;
    }
    case SYS64_CFG_GET: {
        /* The same record the 32-bit kernel copies out */
        struct { char name[32], value[32], help[80]; } out;
        const char *name = config_name((int)a1);
        if (!name || !paging64_user_range(a2, sizeof(out))) {
            r->rax = (uint64_t)-1;
            return;
        }
        memset(&out, 0, sizeof(out));
        const char *v = config_value((int)a1), *h = config_help((int)a1);
        for (int i = 0; name[i] && i < 31; i++)
            out.name[i] = name[i];
        for (int i = 0; v && v[i] && i < 31; i++)
            out.value[i] = v[i];
        for (int i = 0; h && h[i] && i < 79; i++)
            out.help[i] = h[i];
        *(__typeof__(out) *)(uintptr_t)a2 = out;
        r->rax = 0;
        return;
    }
    case SYS64_CFG_SET: {
        char name[32], value[32];
        if (text_from_ring3(a1, name, sizeof(name)) < 0 ||
            text_from_ring3(a2, value, sizeof(value)) < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        /* Changing what the system reads at boot is changing the
           system: an admin's business, as on the 32-bit side */
        if (task64_level() != USER_ADMIN || config_is_hidden(name)) {
            r->rax = (uint64_t)-2;
            return;
        }
        r->rax = (uint64_t)(int64_t)config_set(name, value);
        return;
    }
    case SYS64_FDINFO: {
        struct { int32_t kind; uint32_t size, pos; } info;
        if (!paging64_user_range(a2, sizeof(info)) || (int64_t)a1 < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        info.kind = task64_fd_kind((int)a1);
        info.size = info.pos = 0;
        if (info.kind < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        if (info.kind == FDK_FILE) {
            int sz = fs_size(task64_fd_slot((int)a1));
            info.size = sz > 0 ? (uint32_t)sz : 0;
            info.pos = task64_fd_pos((int)a1);
        }
        *(__typeof__(info) *)(uintptr_t)a2 = info;
        r->rax = 0;
        return;
    }
    case SYS64_SET_FS:
        /* Only an address the program could have - a kernel address in
           FS would not let it read the kernel (the pages say so), but
           there is no reason to allow it */
        if (a1 && !paging64_user_range(a1, 8)) {
            r->rax = (uint64_t)-1;
            return;
        }
        task64_set_fsbase(a1);
        r->rax = 0;
        return;
    case SYS64_NOTIFY: {
        char title[40], body[80];
        if (text_from_ring3(a1, title, sizeof(title)) < 0 ||
            text_from_ring3(a2, body, sizeof(body)) < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        desktop_notify(title, body);
        r->rax = 0;
        return;
    }
    case SYS64_BG_PREVIEW: {
        struct { int32_t bg, dark, w, h; uint64_t out; } c;
        if (!paging64_user_range(a1, sizeof(c))) {
            r->rax = (uint64_t)-1;
            return;
        }
        c = *(const __typeof__(c) *)(uintptr_t)a1;
        if (c.w <= 0 || c.h <= 0 || c.w * c.h > BG_PREVIEW_MAX ||
            !paging64_user_range(c.out, (uint64_t)(c.w * c.h) * 4)) {
            r->rax = (uint64_t)-1;
            return;
        }
        r->rax = (uint64_t)(int64_t)desktop_bg_preview(
            c.bg, c.dark, c.w, c.h, (uint32_t *)(uintptr_t)c.out);
        return;
    }
    case SYS64_WIN_TRUECOL:
        r->rax = (uint64_t)(int64_t)desktop_win_truecolour(
            (int)a1, task64_current_id());
        return;
    case SYS64_WIN_BLIT32: {
        struct win_blit_cmd64 {
            int32_t x, y, w, h;
            uint64_t pixels;
        } c;
        if (!paging64_user_range(a2, sizeof(c))) {
            r->rax = (uint64_t)-1;
            return;
        }
        c = *(const struct win_blit_cmd64 *)(uintptr_t)a2;
        if (c.w <= 0 || c.h <= 0 || c.w > 1024 || c.h > 1024 ||
            c.w * c.h > 1024 ||
            !paging64_user_range(c.pixels, (uint64_t)(c.w * c.h) * 4)) {
            r->rax = (uint64_t)-1;
            return;
        }
        r->rax = (uint64_t)(int64_t)desktop_win_blit32(
            (int)a1, task64_current_id(), c.x, c.y, c.w, c.h,
            (const uint32_t *)(uintptr_t)c.pixels);
        return;
    }
    case SYS64_PALETTE: {
        if (!paging64_user_range(a1, 256 * sizeof(uint32_t))) {
            r->rax = (uint64_t)-1;
            return;
        }
        uint32_t *out = (uint32_t *)(uintptr_t)a1;
        for (int i = 0; i < 256; i++)
            out[i] = video_palette_rgb((uint8_t)i);
        r->rax = 0;
        return;
    }
    case SYS64_WIN_TITLE: {
        char title[64];
        if (text_from_ring3(a2, title, sizeof(title)) < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        r->rax = (uint64_t)(int64_t)desktop_win_title(
            (int)a1, task64_current_id(), title);
        return;
    }
    case SYS64_WIN_KEEP:
        r->rax = (uint64_t)(int64_t)desktop_win_keep((int)a1,
                                                     task64_current_id());
        return;
    default:
        printk("[kernel] unknown system call %u\n", (uint32_t)nr);
        r->rax = (uint64_t)-1;
        return;
    }
}
