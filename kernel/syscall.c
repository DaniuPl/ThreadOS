/* Syscall handling (int 0x80)
 *
 * The convention is the old Linux one: the number in eax, arguments in
 * ebx/ecx/edx, the result in eax. The handler gets a pointer to the
 * saved user registers and modifies eax in the frame - the interrupt
 * stub restores it on iret.
 *
 * The kernel/user border: a process in ring 3 cannot reach kernel
 * memory itself (paging sees to that), but it could do so by proxy -
 * by passing a pointer to kernel memory in a syscall. The kernel would
 * then write to that address in ring 0, with full rights. That is why
 * every address arriving from ring 3 goes through user_ok()/user_str():
 * it has to lie in user space and be really mapped in the process's
 * page directory. A bad pointer = error -1 (EFAULT), not a page fault
 * inside the kernel or a quiet theft of memory.
 */

#include "include/sem.h"
#include <stdint.h>
#include <stddef.h>
#include "include/syscall.h"
#include "include/smp.h"
#include "include/config.h"
#include "include/install.h"
#include "include/hda.h"
#include "include/user.h"
#include "include/clip.h"
#include "include/isr.h"
#include "include/task.h"
#include "include/pit.h"
#include "include/printk.h"
#include "include/keyboard.h"
#include "include/exec.h"
#include "include/kfile.h"
#include "include/pipe.h"
#include "include/fs.h"
#include "include/pmm.h"
#include "include/rtc.h"
#include "include/desktop.h"
#include "include/heap.h"
#include "include/video.h"
#include "include/paging.h"
#include "include/net.h"
#include "include/tcp.h"
#include "include/signal.h"
#include "include/pci.h"
#include "include/pci_info.h"
#include "include/string.h"

#define USER_AUTH_WINDOW_TICKS 30000u
static int auth_pid = -1;
static int auth_uid = -1;
static uint32_t auth_tick;

static void auth_remember(void)
{
    auth_pid = task_current_id();
    auth_uid = task_uid();
    auth_tick = pit_ticks();
}

static void auth_forget_current(void)
{
    if (task_current_id() == auth_pid)
        auth_pid = -1;
}

static int auth_recent(void)
{
    return auth_pid == task_current_id() && auth_uid == task_uid() &&
           (uint32_t)(pit_ticks() - auth_tick) <= USER_AUTH_WINDOW_TICKS;
}

/* --- which calls do not need the big kernel lock -------------------
 *
 * There is one lock for the whole kernel and every entry from ring 3
 * takes it, because the drivers, the filesystem and the swapper were
 * written for a single core and quietly count on it. But not every
 * call reaches there. The ones below touch ONLY the calling task's
 * state and things that have locks of their own - two cores can be
 * inside them at once and nothing will come of it.
 *
 * The list is short and explicit on purpose: adding a call to it is a
 * promise that it has been read to the end, together with what it
 * calls further down. The default answer is "with the lock" and that
 * is how it is to stay.
 */
int syscall_lockless(uint32_t nr)
{
    switch (nr) {
    case SYS_GETPID:     /* its own number, one task field */
    case SYS_GETTICKS:   /* the timer interrupt counter, one word */
    case SYS_CORE:      /* a question to its own local APIC */
    case SYS_SLEEP:      /* its own "wake me" and the scheduler, which has
                            a lock of its own - and the big lock is not
                            held across a context switch anyway */
        return 1;
    default:
        return 0;
    }
}

/* Drawing commands passed by pointer (layout matching usys.h) */
struct win_rect_cmd {
    int32_t x, y, w, h, color;
};
struct win_draw_cmd {
    int32_t op, x, y, w, h, r;
    uint32_t rgb, rgb2;
    int32_t a, font, px;
    uint32_t s;                       /* the text, or 0 */
};
struct win_text_cmd {
    int32_t x, y, color;
    uint32_t s;                 /* pointer to a string (char *) */
};
struct sysinfo_buf {
    uint32_t ram_kib;
    uint32_t used_kib;
    uint32_t uptime_s;
};
struct win_blit_cmd {
    int32_t x, y, w, h;
    uint32_t pixels;            /* pointer to the bytes (uint8_t *) */
};
struct win_event_buf {
    int32_t type, x, y, val;
};
struct udp_send_cmd {
    uint32_t ip, port, buf, len;
};
struct udp_recv_cmd {
    uint32_t buf, max, timeout_ms;
    uint32_t from_ip, from_port;    /* filled in by the kernel */
};
struct netinfo_buf {
    uint32_t ip, mask, gw, dns;
};
struct tcp_io_cmd {
    uint32_t buf, len, timeout_ms;
};

#define IO_MAX     4096          /* limit for a single read/write */
#define PATH_MAX   128           /* the longest path/command line */
#define USER_TOP   0xC0000000u   /* top of user space (the stack) */
#define EFAULT     ((uint32_t)-1)
/* Not allowed - a different thing from 'it did not work'. A program
   gets -2, so that it can say which of the two happened. */
#define EPERM      ((uint32_t)-2)

/* --- validating pointers from ring 3 ---------------------------------- */

/* Does [addr, addr+len) lie entirely in the current process's user
   memory? paging_get_mapping() returns a frame only for pages from the
   process's own tables - kernel pages (shared PDEs) give 0, so an
   address in the kernel area is rejected automatically. */
static int user_ok(uint32_t addr, uint32_t len)
{
    if (!len)
        return 1;
    if (!addr || addr >= USER_TOP || len > USER_TOP - addr)
        return 0;

    uint32_t cr3 = task_current_cr3();
    for (uint32_t page = addr & ~0xFFFu; page < addr + len; page += 0x1000) {
        /* A page may be promised to the process but not yet touched
           (heap/mmap) - we then make it real here instead of treating a
           freshly allocated buffer as a bad pointer */
        if (!paging_get_mapping(cr3, page) && !process_demand_page(page))
            return 0;
    }
    return 1;
}

/* Copies a string from user space into a kernel buffer.
   0 = OK, -1 = bad pointer or a string longer than max-1. */
/* A bounded copy into a buffer going out to ring 3: what does not fit
   is cut, and the terminating zero is always there. */
static void cfg_copy(char *dst, const char *src, uint32_t max)
{
    uint32_t i = 0;
    while (src && src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static int user_str(char *dst, uint32_t uaddr, uint32_t max)
{
    uint32_t cr3 = task_current_cr3();

    for (uint32_t i = 0; i < max; i++) {
        uint32_t va = uaddr + i;
        if (!va || va >= USER_TOP)
            return -1;
        /* Check the mapping at the start and at every page crossing */
        if (i == 0 || (va & 0xFFF) == 0) {
            if (!paging_get_mapping(cr3, va & ~0xFFFu) &&
                !process_demand_page(va))
                return -1;
        }
        dst[i] = *(const volatile char *)va;
        if (!dst[i])
            return 0;
    }
    return -1;                   /* no zero within the limit - reject */
}

/* A write into user memory after checking the address. 0 = OK, -1 = error. */
static int user_put(uint32_t uaddr, const void *src, uint32_t len)
{
    if (!user_ok(uaddr, len))
        return -1;
    memcpy((void *)uaddr, src, len);
    return 0;
}

/* A read from user memory after checking the address. 0 = OK, -1 = error. */
static int user_get(void *dst, uint32_t uaddr, uint32_t len)
{
    if (!user_ok(uaddr, len))
        return -1;
    memcpy(dst, (const void *)uaddr, len);
    return 0;
}

void syscall_handler(struct regs *r)
{
    switch (r->eax) {
    case SYS_WRITE: {
        uint32_t len = r->edx > IO_MAX ? IO_MAX : r->edx;
        if (!user_ok(r->ecx, len)) {
            r->eax = EFAULT;
            break;
        }
        r->eax = (uint32_t)file_write((int)r->ebx, (const void *)r->ecx, len);
        break;
    }
    case SYS_EXIT:
        auth_forget_current();
        printk("[kernel] process '%s' has ended (code %d)\n",
               task_current_name(), (int)r->ebx);
        task_exit((int)r->ebx);
        break;
    case SYS_SLEEP:
        task_sleep(r->ebx);
        r->eax = 0;
        break;
    case SYS_GETTICKS:
        r->eax = pit_ticks();
        break;
    case SYS_READ: {
        uint32_t len = r->edx > IO_MAX ? IO_MAX : r->edx;
        if (!user_ok(r->ecx, len)) {
            r->eax = EFAULT;
            break;
        }
        r->eax = (uint32_t)file_read((int)r->ebx, (void *)r->ecx, len);
        break;
    }
    case SYS_SPAWN: {
        /* The command line (and the stdout file name) go into kernel
           buffers - exec sets up an address space of its own, so the
           calling process's memory must not be needed under it. */
        char cmd[PATH_MAX], out[PATH_MAX];
        if (user_str(cmd, r->ebx, sizeof(cmd)) < 0) {
            r->eax = EFAULT;
            break;
        }
        if (r->ecx) {
            if (user_str(out, r->ecx, sizeof(out)) < 0) {
                r->eax = EFAULT;
                break;
            }
            r->eax = (uint32_t)exec_spawn_redirect(cmd, out, (int)r->edx);
        } else {
            r->eax = (uint32_t)exec_spawn(cmd);
        }
        break;
    }
    case SYS_WAIT:
        r->eax = (uint32_t)task_wait((int)r->ebx);  /* -> exit code */
        break;
    case SYS_GETPID:
        r->eax = (uint32_t)task_current_id();
        break;
    case SYS_SBRK:
        r->eax = process_sbrk((int32_t)r->ebx);
        break;
    case SYS_OPEN: {
        char path[PATH_MAX];
        if (user_str(path, r->ebx, sizeof(path)) < 0) {
            r->eax = EFAULT;
            break;
        }
        /* The system's own files may be READ by anybody - there is
           nothing secret in them - but opening one to write is the
           same act as changing the setting inside it. */
        if ((int)r->ecx != O_READ && user_is_system_file(path) &&
            task_level() != USER_ADMIN) {
            r->eax = EPERM;
            break;
        }
        r->eax = (uint32_t)file_open(path, (int)r->ecx);
        break;
    }
    case SYS_CLOSE:
        r->eax = (uint32_t)file_close((int)r->ebx);
        break;
    case SYS_SEEK:
        r->eax = (uint32_t)file_seek((int)r->ebx, r->ecx);
        break;
    case SYS_PIPELINE: {
        char cmd[PATH_MAX], out[PATH_MAX];
        if (user_str(cmd, r->ebx, sizeof(cmd)) < 0) {
            r->eax = EFAULT;
            break;
        }
        if (r->ecx && user_str(out, r->ecx, sizeof(out)) < 0) {
            r->eax = EFAULT;
            break;
        }
        r->eax = (uint32_t)exec_spawn_pipeline(cmd, r->ecx ? out : 0);
        break;
    }
    case SYS_READDIR: {
        char nb[FS_NAME_MAX + 1];
        uint32_t size;
        int is_d;
        if (!fs_readdir(task_cwd(), (int)r->ebx, nb, &size, &is_d)) {
            r->eax = (uint32_t)-1;       /* no further entry */
            break;
        }
        if (user_put(r->ecx, nb, strlen(nb) + 1) < 0)
            r->eax = EFAULT;
        else
            r->eax = is_d ? (uint32_t)-2 : size;  /* -2 = a directory */
        break;
    }
    case SYS_UNLINK: {
        char path[PATH_MAX];
        if (user_str(path, r->ebx, sizeof(path)) < 0) {
            r->eax = EFAULT;
            break;
        }
        if (user_is_system_file(path) && task_level() != USER_ADMIN) {
            r->eax = EPERM;
            break;
        }
        r->eax = (uint32_t)fs_unlink(task_cwd(), path);
        break;
    }
    case SYS_RENAME: {
        char from[PATH_MAX], to[PATH_MAX];
        if (user_str(from, r->ebx, sizeof(from)) < 0 ||
            user_str(to, r->ecx, sizeof(to)) < 0) {
            r->eax = EFAULT;
            break;
        }
        /* The system's own files are the admin's business, on both
           sides of the rename: renaming a system file away is as good
           as deleting it, and renaming something ONTO its name would
           put a stranger's file where the system looks for its own. */
        if ((user_is_system_file(from) || user_is_system_file(to)) &&
            task_level() != USER_ADMIN) {
            r->eax = EPERM;
            break;
        }
        r->eax = (uint32_t)fs_rename(task_cwd(), from, to);
        break;
    }
    case SYS_TIME: {
        struct rtc_time t;
        rtc_read(&t);
        r->eax = user_put(r->ebx, &t, sizeof(t)) < 0 ? EFAULT : 0;
        break;
    }
    case SYS_CHDIR: {
        char path[PATH_MAX];
        int nc;
        if (user_str(path, r->ebx, sizeof(path)) < 0) {
            r->eax = EFAULT;
            break;
        }
        if (fs_chdir(task_cwd(), path, &nc) == 0) {
            task_set_cwd(nc);
            r->eax = 0;
        } else {
            r->eax = (uint32_t)-1;
        }
        break;
    }
    case SYS_GETCWD: {
        char path[PATH_MAX];
        fs_getpath(task_cwd(), path);
        r->eax = user_put(r->ebx, path, strlen(path) + 1) < 0 ? EFAULT : 0;
        break;
    }
    case SYS_MKDIR: {
        char path[PATH_MAX];
        if (user_str(path, r->ebx, sizeof(path)) < 0)
            r->eax = EFAULT;
        else
            r->eax = (uint32_t)fs_mkdir(task_cwd(), path);
        break;
    }
    case SYS_KILL:
        /* ecx = signal number; 0 means "just kill" (SIGKILL).
           A plain user may signal only their own processes: a level of
           its own is exactly what makes a process somebody else's. */
        if (task_level() != USER_ADMIN &&
            task_level_of((int)r->ebx) != task_level()) {
            r->eax = EPERM;
            break;
        }
        r->eax = (uint32_t)task_signal_send((int)r->ebx,
                                            r->ecx ? (int)r->ecx : SIGKILL);
        break;
    case SYS_SIGNAL:
        /* ebx=signal, ecx=handler (0=default, 1=ignore),
           edx=the ring 3 return trampoline */
        r->eax = (uint32_t)task_signal_set((int)r->ebx, r->ecx, r->edx);
        break;
    case SYS_SIGRETURN:
        /* We do not set eax here - the whole state (eax included) comes
           back from the frame pushed when the signal was delivered */
        signal_return(r);
        break;
    case SYS_WIN_CREATE: {
        char title[64];
        if (user_str(title, r->edx, sizeof(title)) < 0)
            r->eax = EFAULT;
        else
            r->eax = (uint32_t)desktop_win_create(task_current_id(),
                                                  (int)r->ebx, (int)r->ecx,
                                                  title);
        break;
    }
    case SYS_WIN_RECT: {
        struct win_rect_cmd c;
        if (user_get(&c, r->ecx, sizeof(c)) < 0)
            r->eax = EFAULT;
        else
            r->eax = (uint32_t)desktop_win_rect((int)r->ebx,
                                                task_current_id(),
                                                c.x, c.y, c.w, c.h,
                                                (uint8_t)c.color);
        break;
    }
    case SYS_WIN_TEXT: {
        struct win_text_cmd c;
        char s[128];
        if (user_get(&c, r->ecx, sizeof(c)) < 0 ||
            user_str(s, c.s, sizeof(s)) < 0) {
            r->eax = EFAULT;
            break;
        }
        r->eax = (uint32_t)desktop_win_text((int)r->ebx, task_current_id(),
                                            c.x, c.y, s, (uint8_t)c.color);
        break;
    }
    case SYS_WIN_DRAW: {
        struct win_draw_cmd c;
        char s[256];
        if (user_get(&c, r->ecx, sizeof(c)) < 0 ||
            (c.s && user_str(s, c.s, sizeof(s)) < 0)) {
            r->eax = EFAULT;
            break;
        }
        struct win_draw d = { c.op, c.x, c.y, c.w, c.h, c.r, c.rgb, c.rgb2,
                              c.a, c.font, c.px };
        r->eax = (uint32_t)desktop_win_draw((int)r->ebx, task_current_id(),
                                            &d, c.s ? s : (const char *)0);
        break;
    }
    case SYS_UI_LOOK: {
        uint32_t look[UI_LOOK_N];
        desktop_ui_look((int)r->ecx, look);
        r->eax = user_put(r->ebx, look, sizeof(look)) < 0 ? EFAULT : 0;
        break;
    }
    case SYS_NOTIFY: {
        char title[40], body[80];
        if (user_str(title, r->ebx, sizeof(title)) < 0 ||
            user_str(body, r->ecx, sizeof(body)) < 0) {
            r->eax = EFAULT;
            break;
        }
        desktop_notify(title, body);
        r->eax = 0;
        break;
    }
    case SYS_BG_PREVIEW: {
        struct { int32_t bg, dark, w, h; uint32_t out; } c;
        if (user_get(&c, r->ebx, sizeof(c)) < 0 || c.w <= 0 || c.h <= 0 ||
            c.w * c.h > BG_PREVIEW_MAX) {
            r->eax = EFAULT;
            break;
        }
        uint32_t *px = kmalloc((uint32_t)(c.w * c.h) * 4);
        if (!px) {
            r->eax = (uint32_t)-1;
            break;
        }
        int rc = desktop_bg_preview(c.bg, c.dark, c.w, c.h, px);
        if (rc == 0 && user_put(c.out, px, (uint32_t)(c.w * c.h) * 4) < 0)
            rc = EFAULT;
        kfree(px);
        r->eax = (uint32_t)rc;
        break;
    }
    case SYS_WIN_POLL: {
        struct win_event_buf ev;
        int got = desktop_win_poll((int)r->ebx, task_current_id(), &ev);
        if (got == 1 && user_put(r->ecx, &ev, sizeof(ev)) < 0)
            r->eax = EFAULT;
        else
            r->eax = (uint32_t)got;
        break;
    }
    case SYS_PS: {
        /* The list is read into the kernel first and only then handed
           over: the copy to ring 3 can touch a page that is not there,
           and a page fault with the task list locked would be a
           machine that stops. */
        struct psinfo info;
        if (task_ps((int)r->ebx, &info) < 0)
            r->eax = (uint32_t)-1;
        else if (user_put(r->ecx, &info, sizeof(info)) < 0)
            r->eax = EFAULT;
        else
            r->eax = 0;
        break;
    }
    case SYS_WIN_TITLE: {
        char title[64];
        if (user_str(title, r->ecx, sizeof(title)) < 0)
            r->eax = EFAULT;
        else
            r->eax = (uint32_t)desktop_win_title((int)r->ebx,
                                                 task_current_id(), title);
        break;
    }
    case SYS_WIN_BLIT: {
        struct win_blit_cmd c;
        if (user_get(&c, r->ecx, sizeof(c)) < 0) {
            r->eax = EFAULT;
            break;
        }
        /* One row at a time is what the caller is expected to do, so
           the kernel's own buffer is one row wide - and a request for
           more than that is refused rather than quietly cut short. */
        if (c.w <= 0 || c.h <= 0 || c.w > 1024 || c.h > 1024 ||
            c.w * c.h > 4096) {
            r->eax = (uint32_t)-1;
            break;
        }
        uint8_t px[4096];
        if (user_get(px, c.pixels, (uint32_t)(c.w * c.h)) < 0) {
            r->eax = EFAULT;
            break;
        }
        r->eax = (uint32_t)desktop_win_blit((int)r->ebx, task_current_id(),
                                            c.x, c.y, c.w, c.h, px);
        break;
    }
    case SYS_WIN_TRUECOL:
        r->eax = (uint32_t)desktop_win_truecolour((int)r->ebx,
                                                  task_current_id());
        break;
    case SYS_WIN_BLIT32: {
        struct win_blit_cmd c;
        if (user_get(&c, r->ecx, sizeof(c)) < 0) {
            r->eax = EFAULT;
            break;
        }
        /* A colour is four bytes, so this holds a quarter of the
           pixels the index call does for the same four kilobytes of
           kernel stack - still a row of any screen this machine can
           have. */
        if (c.w <= 0 || c.h <= 0 || c.w > 1024 || c.h > 1024 ||
            c.w * c.h > 1024) {
            r->eax = (uint32_t)-1;
            break;
        }
        uint32_t px32[1024];
        if (user_get(px32, c.pixels, (uint32_t)(c.w * c.h) * 4) < 0) {
            r->eax = EFAULT;
            break;
        }
        r->eax = (uint32_t)desktop_win_blit32((int)r->ebx, task_current_id(),
                                              c.x, c.y, c.w, c.h, px32);
        break;
    }
    case SYS_PALETTE: {
        /* The whole palette in one go. A program that wants to show a
           picture from the outside world has to turn colours into the
           numbers this screen speaks, and it cannot do that without
           knowing what the numbers mean. */
        uint32_t rgb[256];
        for (int i = 0; i < 256; i++)
            rgb[i] = video_palette_rgb((uint8_t)i);
        r->eax = user_put(r->ebx, rgb, sizeof(rgb)) < 0 ? EFAULT : 0;
        break;
    }
    case SYS_WIN_KEEP:
        r->eax = (uint32_t)desktop_win_keep((int)r->ebx, task_current_id());
        break;
    case SYS_MMAP:
        r->eax = process_mmap(r->ebx);
        break;
    case SYS_MMAP_FILE: {
        /* A file seen as memory. The name arrives from ring 3, so we copy
           it into the kernel, and hand the size back through a pointer,
           because the address already takes up the whole return
           value. */
        char name[FS_NAME_MAX + 1];
        if (user_str(name, r->ebx, sizeof(name)) < 0) {
            r->eax = 0;
            break;
        }
        uint32_t size = 0;
        uint32_t addr = process_mmap_file(name, &size, r->edx);
        if (addr && user_put(r->ecx, &size, sizeof(size)) < 0)
            addr = 0;
        r->eax = addr;
        break;
    }
    case SYS_MSYNC:
        r->eax = process_msync();
        break;
    /* The core number. A program has no way of reading it itself - the
       local APIC is outside its address space - and without it there
       is no way to show that a process really ran somewhere else. */
    case SYS_CORE:
        r->eax = (uint32_t)smp_core();
        break;
    /* --- System settings ------------------------------------------
     *
     * A program may ask about them and change them, but it holds none
     * of them: what the machine remembers about itself is one place,
     * and it is the kernel's. The names are fixed, so an unknown one
     * is an error and not a new setting. */
    case SYS_CFG_GET: {
        struct cfg_entry out;
        const char *name = config_name((int)r->ebx);
        if (!name) {
            r->eax = (uint32_t)-1;
            break;
        }
        memset(&out, 0, sizeof(out));
        cfg_copy(out.name, name, sizeof(out.name));
        cfg_copy(out.value, config_value((int)r->ebx), sizeof(out.value));
        cfg_copy(out.help, config_help((int)r->ebx), sizeof(out.help));
        r->eax = user_put(r->ecx, &out, sizeof(out)) < 0 ? EFAULT : 0;
        break;
    }
    case SYS_CFG_SET: {
        char name[CFG_NAME_MAX], value[CFG_VAL_MAX];
        if (user_str(name, r->ebx, sizeof(name)) < 0 ||
            user_str(value, r->ecx, sizeof(value)) < 0) {
            r->eax = EFAULT;
            break;
        }
        /* Changing what the system reads at boot is changing the
           system. A password can be typed into the program that asks
           for it - and then this same call goes through. */
        if (task_level() != USER_ADMIN || config_is_hidden(name)) {
            r->eax = EPERM;
            break;
        }
        r->eax = (uint32_t)config_set(name, value);
        break;
    }
    /* --- installing onto this machine's own disk -------------------
     *
     * Looking is reading and is allowed to anybody. Writing is not:
     * every step goes through the same gate as changing a setting,
     * because what is behind it is the disk of a machine that boots
     * without us. */
    case SYS_INSTALL_LOOK: {
        struct install_info info;
        int can = install_look((int)r->ebx, (int)r->edx, &info);
        if (user_put(r->ecx, &info, sizeof(info)) < 0) {
            r->eax = EFAULT;
            break;
        }
        r->eax = (uint32_t)can;
        break;
    }
    case SYS_INSTALL_STEP:
        if (install_update_allowed() &&
            (task_level() != USER_ADMIN || !auth_recent())) {
            r->eax = EPERM;
            break;
        }
        r->eax = (uint32_t)install_step((int)r->ebx);
        break;

    /* --- sound ------------------------------------------------------
     *
     * Loudness is a knob and not a change to the system, so nothing
     * here asks who is turning it. Playing a tone is the kernel's -
     * a program says what it wants heard, not how. */
    case SYS_SND_GET: {
        struct snd_info info;
        info.present = (uint32_t)hda_present();
        info.volume = (uint32_t)hda_volume();
        info.muted = (uint32_t)hda_muted();
        info.steps = (uint32_t)hda_volume_steps();
        r->eax = user_put(r->ebx, &info, sizeof(info)) < 0 ? EFAULT : 0;
        break;
    }
    case SYS_SND_SET:
        if ((int32_t)r->ebx >= 0)
            hda_set_volume((int)r->ebx);
        if ((int32_t)r->ecx >= 0)
            hda_set_mute((int)r->ecx);
        hda_remember();
        r->eax = 0;
        break;
    case SYS_SND_BEEP:
        r->eax = (uint32_t)hda_beep(r->ebx, r->ecx);
        break;
    case SYS_SND_STREAM: {
        static int16_t samples[HDA_STREAM_MAX_FRAMES * 2];
        int result = -1;
        if (r->ebx == SND_STREAM_BEGIN) {
            result = hda_stream_begin();
        } else if (r->ebx == SND_STREAM_WRITE) {
            uint32_t frames = r->edx;
            if (hda_stream_owned() && frames <= HDA_STREAM_MAX_FRAMES &&
                user_get(samples, r->ecx, frames * 4) == 0)
                result = hda_stream_write(samples, frames);
            else if (frames <= HDA_STREAM_MAX_FRAMES &&
                     !user_ok(r->ecx, frames * 4))
                result = (int)EFAULT;
        } else if (r->ebx == SND_STREAM_END) {
            result = hda_stream_end();
        }
        r->eax = (uint32_t)result;
        break;
    }

    /* --- semaphores: waiting for one another ----------------------- */
    case SYS_SEM_GET:
        r->eax = (uint32_t)sem_get((int)r->ebx, (int)r->ecx);
        break;
    case SYS_SEM_WAIT:
        r->eax = (uint32_t)sem_wait((int)r->ebx);
        break;
    case SYS_SEM_POST:
        r->eax = (uint32_t)sem_post((int)r->ebx);
        break;

    /* --- who is at the machine ------------------------------------
     *
     * Up by a password, down for nothing. Going down is never a thing
     * to check: giving up a right is always allowed. */
    case SYS_AUTH: {
        char pass[USER_PASSWORD_MAX + 1];
        if (user_str(pass, r->ebx, sizeof(pass)) < 0) {
            memset(pass, 0, sizeof(pass));
            r->eax = EFAULT;
            break;
        }
        if (!user_password_ok(pass)) {
            memset(pass, 0, sizeof(pass));
            r->eax = EPERM;
            break;
        }
        memset(pass, 0, sizeof(pass));
        task_set_level(USER_ADMIN);
        auth_remember();
        r->eax = 0;
        break;
    }
    case SYS_DROP:
        task_set_level(USER_PLAIN);
        auth_forget_current();
        r->eax = 0;
        break;
    case SYS_LEVEL:
        r->eax = (uint32_t)task_level();
        break;
    case SYS_USER: {
        struct user_ctl ctl = { 0 };
        if (user_get(&ctl, r->ebx, sizeof(ctl)) < 0) {
            memset(&ctl, 0, sizeof(ctl));
            r->eax = EFAULT;
            break;
        }
        int rc = -1;
        if (ctl.op == USER_CTL_LIST) {
            int uid, level;
            memset(ctl.name, 0, sizeof(ctl.name));
            rc = user_account_info(ctl.index, ctl.name, sizeof(ctl.name),
                                   &uid, &level);
            if (rc == 0) {
                ctl.uid = uid;
                ctl.level = level;
                memset(ctl.password, 0, sizeof(ctl.password));
                memset(ctl.admin_password, 0, sizeof(ctl.admin_password));
                if (user_put(r->ebx, &ctl, sizeof(ctl)) < 0)
                    rc = -1;
            }
        } else {
            uint32_t base = r->ebx;
            if (user_str(ctl.name, base + offsetof(struct user_ctl, name),
                         sizeof(ctl.name)) < 0) {
                memset(&ctl, 0, sizeof(ctl));
                r->eax = EFAULT;
                break;
            }
            if (ctl.op == USER_CTL_ADD || ctl.op == USER_CTL_PASSWORD) {
                if (user_str(ctl.password,
                             base + offsetof(struct user_ctl, password),
                             sizeof(ctl.password)) < 0) {
                    memset(&ctl, 0, sizeof(ctl));
                    r->eax = EFAULT;
                    break;
                }
            }
            if (ctl.op == USER_CTL_ADD || ctl.op == USER_CTL_DELETE ||
                ctl.op == USER_CTL_PASSWORD || ctl.op == USER_CTL_ROLE) {
                if (user_str(ctl.admin_password,
                             base + offsetof(struct user_ctl, admin_password),
                             sizeof(ctl.admin_password)) < 0) {
                    memset(&ctl, 0, sizeof(ctl));
                    r->eax = EFAULT;
                    break;
                }
            }
            if (ctl.op == USER_CTL_ADD)
                rc = user_account_add(ctl.name, ctl.password, ctl.level,
                                      ctl.admin_password);
            else if (ctl.op == USER_CTL_DELETE)
                rc = user_account_delete(ctl.name, ctl.admin_password);
            else if (ctl.op == USER_CTL_ROLE)
                rc = user_account_set_level(ctl.name, ctl.level,
                                            ctl.admin_password);
            else if (ctl.op == USER_CTL_PASSWORD) {
                if (!strcmp(ctl.name, user_current_name()))
                    rc = user_password_current_ok(ctl.admin_password)
                             ? user_set_password(ctl.password) : -2;
                else
                    rc = user_set_password_for(ctl.name, ctl.password,
                                               ctl.admin_password);
            }
        }
        memset(&ctl, 0, sizeof(ctl));
        r->eax = (uint32_t)rc;
        break;
    }
    case SYS_SPAWN_TALK: {
        char cmd[PATH_MAX], out[PATH_MAX];
        if (user_str(cmd, r->ebx, sizeof(cmd)) < 0) {
            r->eax = EFAULT;
            break;
        }
        out[0] = '\0';
        if (r->ecx && user_str(out, r->ecx, sizeof(out)) < 0) {
            r->eax = EFAULT;
            break;
        }
        int idx = -1;
        int pid = exec_spawn_talk(cmd, r->ecx ? out : (const char *)0, &idx);
        if (pid < 0) {
            r->eax = (uint32_t)-1;
            break;
        }
        /* The write end becomes an ordinary descriptor of ours, so
           writing to the program is writing to a file number - which is
           what every other program already knows how to do. */
        int fd = file_reserve();
        if (fd < 0) {
            pipe_close_writer(idx);
            r->eax = (uint32_t)-1;
            break;
        }
        file_set_pipe(fd, idx, 1);
        if (r->edx && user_put(r->edx, &fd, sizeof(fd)) < 0) {
            r->eax = EFAULT;
            break;
        }
        r->eax = (uint32_t)pid;
        break;
    }
    /* --- the clipboard: text waiting between two windows --------- */
    case SYS_CLIP_SET: {
        uint32_t len = r->ecx > CLIP_MAX ? CLIP_MAX : r->ecx;
        char buf[CLIP_MAX];
        if (len && !user_ok(r->ebx, len)) {
            r->eax = EFAULT;
            break;
        }
        for (uint32_t i = 0; i < len; i++)
            buf[i] = ((const char *)r->ebx)[i];
        r->eax = clip_set(buf, len);
        break;
    }
    case SYS_CLIP_GET: {
        uint32_t max = r->ecx > CLIP_MAX + 1 ? CLIP_MAX + 1 : r->ecx;
        char buf[CLIP_MAX + 1];
        uint32_t n = clip_get(buf, max);
        if (max && user_put(r->ebx, buf, n + 1) < 0) {
            r->eax = EFAULT;
            break;
        }
        r->eax = n;
        break;
    }
    case SYS_TRYWAIT: {
        if (task_alive((int)r->ebx)) {
            r->eax = (uint32_t)-3;         /* still running */
            break;
        }
        int code = task_wait((int)r->ebx);  /* it has ended: does not block */
        if (r->ecx) {
            if (user_put(r->ecx, &code, sizeof(code)) < 0) {
                r->eax = EFAULT;
                break;
            }
        }
        r->eax = 0;
        break;
    }
    case SYS_SHM:
        r->eax = process_shm((int)r->ebx, r->ecx);
        break;
    case SYS_THREAD:
        /* A thread starts in ring 3 - the entry point must be process code */
        if (!user_ok(r->ebx, 1))
            r->eax = EFAULT;
        else
            r->eax = (uint32_t)process_thread(r->ebx, r->ecx);
        break;
    case SYS_SETPRIO:
        task_set_priority((int)r->ebx);
        r->eax = 0;
        break;
    case SYS_SYSINFO: {
        struct sysinfo_buf si;
        si.ram_kib = pmm_ram_kib();
        si.used_kib = pmm_used_frames() * 4;
        si.uptime_s = pit_ticks() / 100;
        r->eax = user_put(r->ebx, &si, sizeof(si)) < 0 ? EFAULT : 0;
        break;
    }
    case SYS_UDP_OPEN:
        r->eax = (uint32_t)net_udp_open((uint16_t)r->ebx, task_current_id());
        break;
    case SYS_UDP_SEND: {
        struct udp_send_cmd c;
        if (user_get(&c, r->ecx, sizeof(c)) < 0 || c.len > IO_MAX ||
            !user_ok(c.buf, c.len)) {
            r->eax = EFAULT;
            break;
        }
        r->eax = (uint32_t)net_udp_send((int)r->ebx, task_current_id(), c.ip,
                                        (uint16_t)c.port, (const void *)c.buf,
                                        (uint16_t)c.len);
        break;
    }
    case SYS_UDP_RECV: {
        struct udp_recv_cmd c;
        if (user_get(&c, r->ecx, sizeof(c)) < 0 || c.max > IO_MAX ||
            !user_ok(c.buf, c.max)) {
            r->eax = EFAULT;
            break;
        }
        /* Receiving waits (task_sleep) but writes into the buffer only
           after waking - and by then this process's address space is
           active again. The user pointer has already been checked, so
           the data can go straight there, without an intermediary in
           the kernel. */
        uint32_t from_ip = 0;
        uint16_t from_port = 0;
        int n = net_udp_recv((int)r->ebx, task_current_id(), &from_ip,
                             &from_port, (void *)c.buf, (uint16_t)c.max,
                             c.timeout_ms);
        if (n < 0) {
            r->eax = (uint32_t)-1;
            break;
        }
        c.from_ip = from_ip;
        c.from_port = from_port;
        r->eax = user_put(r->ecx, &c, sizeof(c)) < 0 ? EFAULT : (uint32_t)n;
        break;
    }
    case SYS_UDP_CLOSE:
        net_udp_close((int)r->ebx, task_current_id());
        r->eax = 0;
        break;
    case SYS_NETINFO: {
        struct netinfo_buf ni;
        if (!net_configured()) {
            r->eax = (uint32_t)-1;      /* the network has no address yet */
            break;
        }
        ni.ip = net_ip();
        ni.mask = net_mask();
        ni.gw = net_gw();
        ni.dns = net_dns();
        r->eax = user_put(r->ebx, &ni, sizeof(ni)) < 0 ? EFAULT : 0;
        break;
    }
    case SYS_TCP_CONNECT:
        r->eax = (uint32_t)tcp_connect(r->ebx, (uint16_t)r->ecx,
                                       task_current_id(), r->edx);
        break;
    case SYS_TCP_LISTEN:
        r->eax = (uint32_t)tcp_listen((uint16_t)r->ebx, task_current_id());
        break;
    case SYS_TCP_ACCEPT:
        r->eax = (uint32_t)tcp_accept((int)r->ebx, task_current_id(), r->ecx);
        break;
    case SYS_TCP_SEND: {
        struct tcp_io_cmd c;
        if (user_get(&c, r->ecx, sizeof(c)) < 0 || c.len > IO_MAX ||
            !user_ok(c.buf, c.len)) {
            r->eax = EFAULT;
            break;
        }
        r->eax = (uint32_t)tcp_send((int)r->ebx, task_current_id(),
                                    (const void *)c.buf, (uint16_t)c.len,
                                    c.timeout_ms);
        break;
    }
    case SYS_TCP_RECV: {
        struct tcp_io_cmd c;
        if (user_get(&c, r->ecx, sizeof(c)) < 0 || c.len > IO_MAX ||
            !user_ok(c.buf, c.len)) {
            r->eax = EFAULT;
            break;
        }
        /* As with UDP: after waking, this process's space is active
           again, so the data can go straight into the checked buffer. */
        r->eax = (uint32_t)tcp_recv((int)r->ebx, task_current_id(),
                                    (void *)c.buf, (uint16_t)c.len,
                                    c.timeout_ms);
        break;
    }
    case SYS_TCP_CLOSE:
        tcp_close((int)r->ebx, task_current_id());
        r->eax = 0;
        break;
    case SYS_WAITANY: {
        int code = 0;
        int pid = task_wait_any(&code);
        if (pid >= 0 && r->ebx && user_put(r->ebx, &code, sizeof(code)) < 0) {
            r->eax = EFAULT;
            break;
        }
        r->eax = (uint32_t)pid;      /* -1 = there are no children left */
        break;
    }
    case SYS_PCI_INFO: {
        struct pci_info info;
        struct pci_dev dev;
        uint32_t class_code;
        if ((int32_t)r->ebx < 0 || !user_ok(r->ecx, sizeof(info))) {
            r->eax = EFAULT;
            break;
        }
        int found = pci_enumerate((int)r->ebx, &dev, &class_code);
        if (found <= 0) {
            r->eax = (uint32_t)found;
            break;
        }
        info.bus = (uint32_t)dev.bus;
        info.slot = (uint32_t)dev.slot;
        info.function = (uint32_t)dev.func;
        info.vendor = dev.vendor;
        info.device = dev.device;
        info.class_code = class_code;
        r->eax = user_put(r->ecx, &info, sizeof(info)) < 0
                     ? EFAULT : 1;
        break;
    }
    case SYS_EXEC: {
        /* The command line into a kernel buffer: the process's memory is
           about to vanish along with its whole address space */
        char cmd[PATH_MAX];
        if (user_str(cmd, r->ebx, sizeof(cmd)) < 0) {
            r->eax = EFAULT;
            break;
        }
        r->eax = (uint32_t)process_exec(cmd);   /* on success it does not return */
        break;
    }
    case SYS_FORK:
        /* The only syscall that needs the whole register frame: the child
           returns from it into ring 3 where the parent did */
        r->eax = (uint32_t)process_fork(r);
        break;
    default:
        printk("[kernel] unknown syscall %u from '%s'\n",
               r->eax, task_current_name());
        r->eax = (uint32_t)-1;
    }
}
