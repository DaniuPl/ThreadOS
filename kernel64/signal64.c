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
 *
 * The frame is Linux's for x86-64 (struct rt_sigframe), so that what
 * reads a thread's registers from its signal frame finds them where it
 * looks: the handler gets the signal, its siginfo_t and a ucontext_t
 * whose uc_mcontext has every register in glibc's gregs order and a
 * pointer to the SSE state below the frame. WebKit's garbage collector
 * stops a thread with a signal and scans its stack from the stack
 * pointer it finds there; an unwinder walks through a handler the same
 * way.
 */

#include <stddef.h>
#include <stdint.h>
#include "signal64.h"
#include "task64.h"
#include "paging64.h"
#include "fpu64.h"
#include "../kernel/include/isr.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/string.h"

/* Flags a program may set for itself. The rest - including permission
   for interrupts and the port access level - we never hand over: the
   frame comes back from the user stack, so the program could swap it
   out. */
#define RFLAGS_USER_MASK 0x0CD5
#define RFLAGS_ALWAYS    0x0202          /* the fixed bit + interrupts enabled */
#define RFLAGS_TF        0x0100
#define RFLAGS_DF        0x0400
#define RFLAGS_RF        0x10000

#define USER_CS 0x23
#define USER_SS 0x1B

/* Past this an address is not canonical, or is the kernel's half: iretq
   to such a RIP faults in ring 0, before it ever reaches the program */
#define USER_LIMIT 0x0000800000000000ULL

#define RED_ZONE 128                     /* the System V ABI's, below RSP */
#define FX_AREA  512                     /* what fxsave writes */

#define SIGNAL_CHLD  17
#define SIGNAL_CONT  18
#define SIGNAL_URG   23
#define SIGNAL_WINCH 28

/* siginfo_t, as far as kill and tkill fill it */
struct sig_info {
    int32_t signo, err, code, pad;
    int32_t pid;                         /* the sender's program */
    uint32_t uid;
    uint8_t rest[128 - 24];
};

/* Linux's struct sigcontext - glibc's and musl's gregs[] in this order */
struct sig_context {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip;
    uint64_t eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate;                    /* the fxsave area, below the frame */
    uint64_t reserved1[8];
};

#define UC_SIGCONTEXT_SS     2           /* the ss above is the real one */
#define UC_STRICT_RESTORE_SS 4
#define SS_DISABLE           2           /* no alternate stack */

struct sig_ucontext {
    uint64_t flags, link;
    uint64_t ss_sp;
    int32_t ss_flags, ss_pad;
    uint64_t ss_size;
    struct sig_context mc;
    uint64_t sigmask;                    /* a C library's sigset_t is longer:
                                            the rest is the siginfo_t
                                            after it, as on Linux */
};

struct sig_frame {
    uint64_t pretcode;                   /* where the handler's ret goes */
    struct sig_ucontext uc;
    struct sig_info info;
};

_Static_assert(offsetof(struct sig_ucontext, mc) == 40,
               "uc_mcontext where a C library looks for it");
_Static_assert(sizeof(struct sig_context) == 256,
               "Linux's struct sigcontext");
_Static_assert(offsetof(struct sig_ucontext, sigmask) == 296,
               "uc_sigmask after uc_mcontext");
_Static_assert(offsetof(struct sig_frame, info) == 312 &&
               sizeof(struct sig_frame) == 440,
               "Linux's struct rt_sigframe");

/* What a signal does to a program with no handler: these are let go,
   the rest end it */
static int ignored_by_default(int sig)
{
    return sig == SIGNAL_CHLD || sig == SIGNAL_CONT || sig == SIGNAL_URG ||
           sig == SIGNAL_WINCH;
}

void signal64_deliver(struct regs *r)
{
    uint64_t mask;
    int from, code;
    int sig = task64_take_signal(&mask, &from, &code);
    if (!sig)
        return;

    uint64_t handler = task64_sighandler();
    uint64_t restorer = task64_sigrestorer();

    /* Without a handler of its own the default reaction is left. SIGKILL
       has no other by design - which is why it cannot be caught - and
       SIGSTOP, with nothing here to stop a program and resume it, ends
       it the same way. */
    /* Another thread's exit(): no news worth a line on the console */
    if (sig == SIGKILL && task64_group_exiting())
        task64_kill_current("the program ended");
    if (!handler && ignored_by_default(sig))
        return;
    if (!handler || sig == SIGKILL || sig == SIGSTOP) {
        printk("[kernel] task %d got signal %d\n",
               task64_current_id(), sig);
        task64_kill_current("a signal");     /* does not return */
    }

    /* Below the red zone, which the interrupted function may be using
       without having moved RSP over it (a leaf function's locals): the
       SSE state, aligned to 64 as Linux puts it, then the frame -
       sixteen-byte aligned and then eight lower, so that the handler
       finds the stack exactly as every function called by a call
       instruction finds it, with its return address on top. */
    uint64_t sp = r->rsp - RED_ZONE;
    uint64_t fx = (sp - FX_AREA) & ~63ULL;
    uint64_t at = ((fx - sizeof(struct sig_frame)) & ~15ULL) - 8;

    /* The frame goes below anything the program has put on its stack,
       so it may well land in a page the stack has not grown into yet -
       that one is brought in now, as the program's own push would bring
       it. The check is the one for WRITING: the kernel writes the frame
       itself, and a stack pointer aimed at the program's code, or at a
       page still shared after a fork, must end in a refusal or a copy
       here rather than in a kernel fault in the middle of the copy. */
    if (r->rsp < 4096 || r->rsp >= USER_LIMIT ||
        !paging64_user_writable(at, sp - at)) {
        printk("[kernel] signal %d: no room on the program's stack\n", sig);
        task64_kill_current("no stack for the signal");
    }

    struct sig_frame *f = (struct sig_frame *)(uintptr_t)at;
    memset(f, 0, sizeof(*f));
    f->pretcode = restorer;
    f->uc.flags = UC_SIGCONTEXT_SS | UC_STRICT_RESTORE_SS;
    f->uc.ss_flags = SS_DISABLE;
    struct sig_context *mc = &f->uc.mc;
    mc->r8 = r->r8;
    mc->r9 = r->r9;
    mc->r10 = r->r10;
    mc->r11 = r->r11;
    mc->r12 = r->r12;
    mc->r13 = r->r13;
    mc->r14 = r->r14;
    mc->r15 = r->r15;
    mc->rdi = r->rdi;
    mc->rsi = r->rsi;
    mc->rbp = r->rbp;
    mc->rbx = r->rbx;
    mc->rdx = r->rdx;
    mc->rax = r->rax;
    mc->rcx = r->rcx;
    mc->rsp = r->rsp;
    mc->rip = r->rip;
    mc->eflags = r->rflags;
    mc->cs = (uint16_t)r->cs;
    mc->ss = (uint16_t)r->ss;
    if (r->int_no < 32) {                /* an exception: which one */
        mc->trapno = r->int_no;
        mc->err = r->err_code;
    }
    mc->oldmask = mask;
    mc->fpstate = fx;
    f->uc.sigmask = mask;
    f->info.signo = sig;
    f->info.code = code;
    uint64_t addr;
    if (task64_fault_addr(sig, &addr))
        memcpy(&f->info.pid, &addr, sizeof(addr));   /* si_addr's place */
    else
        f->info.pid = from;

    /* This core's SSE registers are the program's: the kernel is built
       without them, and a switch puts a task's back before it runs.
       Copied out through an aligned buffer of the kernel's - fxsave
       wants sixteen bytes, and the page under the frame may go to disk
       and come back in the middle. The bytes fxsave leaves alone are
       where Linux marks an XSAVE record; there is none. */
    uint8_t fp[FX_AREA] __attribute__((aligned(16)));
    fpu64_save(fp);
    memset(fp + 464, 0, FX_AREA - 464);
    memcpy((void *)(uintptr_t)fx, fp, FX_AREA);
    /* The handler starts with the unit a new task gets, as on Linux: a
       rounding mode or a mask the interrupted code set is its own */
    fpu64_reset();

    /* The handler runs with the signal held back, as without SA_NODEFER;
       the mask from before comes back with SYS64_SIGRETURN */
    task64_set_sigmask(mask | SIG_BIT(sig));
    r->rdi = (uint64_t)sig;
    r->rsi = at + offsetof(struct sig_frame, info);
    r->rdx = at + offsetof(struct sig_frame, uc);
    r->rax = 0;                          /* no vector registers: a handler
                                            declared without a prototype */
    r->rsp = at;
    r->rip = handler;
    r->rflags &= ~(uint64_t)(RFLAGS_DF | RFLAGS_TF | RFLAGS_RF);
    r->cs = USER_CS;
    r->ss = USER_SS;
}

int signal64_return(struct regs *r)
{
    /* The handler finished with an ordinary return, which took the
       restorer's address off the frame: the frame starts eight bytes
       below the stack pointer the restorer calls the kernel with */
    uint64_t at = r->rsp - 8;
    if (!paging64_user_range(at, sizeof(struct sig_frame)))
        return -1;

    /* A copy, so that what is checked is what is used: the frame lies
       in the program's memory, and another of its threads could be
       writing it */
    struct sig_frame f;
    memcpy(&f, (const void *)(uintptr_t)at, sizeof(f));
    const struct sig_context *mc = &f.uc.mc;
    if (mc->rip >= USER_LIMIT || mc->rsp >= USER_LIMIT)
        return -1;
    uint8_t fp[FX_AREA] __attribute__((aligned(16)));
    if (mc->fpstate) {
        if ((mc->fpstate & 15) || !paging64_user_range(mc->fpstate, FX_AREA))
            return -1;
        memcpy(fp, (const void *)(uintptr_t)mc->fpstate, FX_AREA);
        /* A reserved MXCSR bit would make fxrstor fault in the kernel */
        *(uint32_t *)(fp + 24) &= fpu64_mxcsr_mask();
    }

    /* The registers and the return address are the program's business -
       the segments and flags are not, because that is the way into the
       kernel. */
    r->r8 = mc->r8;
    r->r9 = mc->r9;
    r->r10 = mc->r10;
    r->r11 = mc->r11;
    r->r12 = mc->r12;
    r->r13 = mc->r13;
    r->r14 = mc->r14;
    r->r15 = mc->r15;
    r->rdi = mc->rdi;
    r->rsi = mc->rsi;
    r->rbp = mc->rbp;
    r->rbx = mc->rbx;
    r->rdx = mc->rdx;
    r->rax = mc->rax;
    r->rcx = mc->rcx;
    r->rsp = mc->rsp;
    r->rip = mc->rip;
    r->rflags = (mc->eflags & RFLAGS_USER_MASK) | RFLAGS_ALWAYS;
    r->cs = USER_CS;
    r->ss = USER_SS;
    r->int_no = 0;
    r->err_code = 0;
    task64_set_sigmask(f.uc.sigmask);
    if (mc->fpstate)
        fpu64_load(fp);
    else
        fpu64_reset();
    return 0;
}
