/* Signals
 *
 * A signal is the simplest form of an asynchronous event: one bit in a
 * pending mask. We always deliver it at the same safe place - on the
 * return to ring 3, when a full frame of the process's registers lies
 * on the kernel stack.
 *
 * When the process has a handler of its own, the kernel pushes that
 * frame onto the process's own stack, adding the signal number as an
 * argument and the trampoline address as the return address. The
 * handler therefore runs in ring 3, on the process's own stack. When
 * it returns, the trampoline calls sigreturn and the kernel restores
 * the interrupted state from that frame - the process comes back
 * exactly where the signal found it.
 *
 * Nothing we did not write ourselves goes back to the process: when
 * restoring we take from the frame only the general registers, eip,
 * esp and the arithmetic flags. The segment selectors and IOPL stay
 * the kernel's, otherwise a process could substitute a privileged
 * state of its own in sigreturn.
 */

#include <stdint.h>
#include "include/signal.h"
#include "include/isr.h"
#include "include/task.h"
#include "include/paging.h"
#include "include/printk.h"
#include "include/string.h"

#define USER_TOP 0xC0000000u

/* The eflags a process may restore for itself: the arithmetic ones
   plus the direction flag. No TF (single-step trap), no IOPL, no NT. */
#define EFLAGS_USER_MASK 0xCD5u

const char *signal_name(int sig)
{
    switch (sig) {
    case SIGINT:  return "SIGINT";
    case SIGKILL: return "SIGKILL";
    case SIGTERM: return "SIGTERM";
    case SIGCHLD: return "SIGCHLD";
    default:      return "sygnal";
    }
}

/* Does the whole range lie in mapped user memory? */
static int user_range_ok(uint32_t addr, uint32_t len)
{
    if (!addr || addr >= USER_TOP || len > USER_TOP - addr)
        return 0;
    uint32_t cr3 = task_current_cr3();
    for (uint32_t page = addr & ~0xFFFu; page < addr + len; page += 0x1000)
        if (!paging_get_mapping(cr3, page))
            return 0;
    return 1;
}

/* The kernel's default reaction to a signal with no handler */
static void default_action(int sig)
{
    if (sig == SIGCHLD)
        return;                     /* by default nobody cares */
    printk("[jadro] proces '%s' (pid %d) zakonczony przez %s\n",
           task_current_name(), task_current_id(), signal_name(sig));
    task_exit(-sig);
}

void signal_deliver(struct regs *r)
{
    if ((r->cs & 3) != 3)               /* only on the return to ring 3 */
        return;
    if (task_signal_in_handler())       /* we do not nest handlers */
        return;

    int sig = task_signal_take();
    if (!sig)
        return;

    uint32_t handler = task_signal_handler(sig);
    if (handler == SIG_IGN)
        return;
    if (handler == SIG_DFL) {
        default_action(sig);
        return;
    }

    /* The interrupted state's frame plus the two words of the handler call */
    uint32_t frame = (r->useresp - sizeof(struct regs)) & ~3u;
    uint32_t sp = frame - 8;
    if (!user_range_ok(sp, sizeof(struct regs) + 8)) {
        printk("[jadro] proces '%s': brak miejsca na stosie na %s\n",
               task_current_name(), signal_name(sig));
        task_exit(-sig);
        return;
    }

    memcpy((void *)frame, r, sizeof(struct regs));
    ((uint32_t *)sp)[1] = (uint32_t)sig;          /* the handler's argument */
    ((uint32_t *)sp)[0] = task_signal_tramp();    /* the return address */

    r->useresp = sp;
    r->eip = handler;
    r->eflags |= 0x200;                           /* the handler runs with IF=1 */
    task_signal_enter(frame);
}

void signal_return(struct regs *r)
{
    uint32_t frame = task_signal_leave();
    if (!frame || !user_range_ok(frame, sizeof(struct regs))) {
        printk("[jadro] proces '%s': sigreturn bez wlasnej ramki\n",
               task_current_name());
        task_exit(-1);
        return;
    }

    struct regs saved;
    memcpy(&saved, (const void *)frame, sizeof(saved));

    r->edi = saved.edi;
    r->esi = saved.esi;
    r->ebp = saved.ebp;
    r->ebx = saved.ebx;
    r->edx = saved.edx;
    r->ecx = saved.ecx;
    r->eax = saved.eax;
    r->eip = saved.eip;
    r->useresp = saved.useresp;
    r->eflags = (saved.eflags & EFLAGS_USER_MASK) | 0x202;
}
