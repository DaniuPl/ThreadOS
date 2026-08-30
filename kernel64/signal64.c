/* Signals in long mode
 *
 * A signal is not a message to be read but an interruption of a
 * program's course. The kernel cannot deliver it at the moment it is
 * sent: the addressee is somewhere else then - it may be asleep, it
 * may be sitting inside a system call, it may not be running at all.
 * So the signal waits at the task until the NEAREST return to ring 3.
 *
 * That return again comes down to a frame. Delivering a signal means:
 * put the program's frame on its own stack, point it at the handler
 * and slip underneath it a return address that will call the kernel
 * back. The program on the other side has no way of noticing that
 * anything interrupted it - and when the handler finishes it returns
 * exactly where it was interrupted, down to the register.
 */

#include <stdint.h>
#include "signal64.h"
#include "task64.h"
#include "paging64.h"
#include "../kernel/include/isr.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/string.h"

/* Flags a program may set for itself. The rest - including permission
   for interrupts and the port access level - we never hand over: the
   frame comes back from the user stack, so the program could swap it
   out. */
#define RFLAGS_USER_MASK 0x0CD5
#define RFLAGS_ALWAYS    0x0202          /* the fixed bit + interrupts enabled */

void signal64_deliver(struct regs *r)
{
    int sig = task64_take_signal();
    if (!sig)
        return;

    uint64_t handler = task64_sighandler();
    uint64_t restorer = task64_sigrestorer();

    /* Without a handler of its own the default reaction is left. SIGKILL
       has no other by design - which is why it cannot be caught. */
    if (!handler || sig == SIGKILL) {
        printk("[jadro] zadanie %d dostalo sygnal %d\n",
               task64_current_id(), sig);
        task64_kill_current("sygnal");     /* does not return */
    }

    /* Room for the frame and the return address on the program's stack.
       We align the top to sixteen bytes and then subtract eight for the
       return address - the handler is to find the stack exactly as
       every function called by a call instruction finds it. */
    uint64_t frame = (r->rsp - sizeof(struct regs)) & ~0xFULL;
    uint64_t sp = frame - 8;

    if (!paging64_user_range(sp, sizeof(struct regs) + 8)) {
        printk("[jadro] sygnal %d: brak miejsca na stosie programu\n", sig);
        task64_kill_current("brak stosu na sygnal");
    }

    memcpy((void *)(uintptr_t)frame, r, sizeof(*r));
    *(uint64_t *)(uintptr_t)sp = restorer;

    r->rsp = sp;
    r->rip = handler;
    r->rdi = (uint64_t)sig;              /* the signal number for the handler */
}

int signal64_return(struct regs *r)
{
    /* The handler finished with an ordinary return, so the stack now
       points exactly at the frame that was put there */
    uint64_t src = r->rsp;
    if (!paging64_user_range(src, sizeof(struct regs)))
        return -1;

    struct regs saved;
    memcpy(&saved, (const void *)(uintptr_t)src, sizeof(saved));

    /* The frame lay on the user stack, so the program could have
       swapped it out. The registers and the return address are its
       business - the segments and flags are not, because that is the
       way into the kernel. */
    saved.cs = 0x23;
    saved.ss = 0x1B;
    saved.rflags = (saved.rflags & RFLAGS_USER_MASK) | RFLAGS_ALWAYS;
    saved.int_no = 0;
    saved.err_code = 0;

    *r = saved;
    return 0;
}
