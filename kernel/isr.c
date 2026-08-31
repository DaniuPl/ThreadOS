/* Common interrupt handling - called from the stubs in kernel/isr.S */

#include <stddef.h>
#include "include/isr.h"
#include "include/paging.h"
#include "include/signal.h"
#include "include/exec.h"
#include "include/pic.h"
#include "include/printk.h"
#include "include/smp.h"
#include "include/tlb.h"
#include "include/syscall.h"
#include "include/task.h"
#include "include/biglock.h"

static const char *exception_names[32] = {
    "Divide Error", "Debug", "NMI", "Breakpoint",
    "Overflow", "BOUND Range Exceeded", "Invalid Opcode",
    "Device Not Available", "Double Fault", "Coprocessor Segment Overrun",
    "Invalid TSS", "Segment Not Present", "Stack-Segment Fault",
    "General Protection Fault", "Page Fault", "(zarezerwowany)",
    "x87 FPU Error", "Alignment Check", "Machine Check",
    "SIMD FP Exception", "Virtualization Exception", "Control Protection",
    "(zarezerwowany)", "(zarezerwowany)", "(zarezerwowany)",
    "(zarezerwowany)", "(zarezerwowany)", "(zarezerwowany)",
    "(zarezerwowany)", "(zarezerwowany)", "(zarezerwowany)",
    "(zarezerwowany)",
};

static irq_handler_t irq_handlers[16];

void irq_install_handler(int irq, irq_handler_t handler)
{
    irq_handlers[irq] = handler;
}

static void isr_dispatch(struct regs *r);

/* Entry into the kernel from ring 3 takes the big kernel lock.
 *
 * A program may run on any core, but inside the kernel there is to be
 * one at a time: the drivers, the filesystem and the swapper were
 * written for a single core and quietly count on it. An interrupt that
 * found the processor already inside the kernel takes nothing new -
 * the lock is recursive and a core that holds it walks straight
 * through without waiting.
 */
void isr_handler(struct regs *r)
{
    if ((r->cs & 3) != 3) {         /* the kernel interrupted inside the kernel */
        isr_dispatch(r);
        return;
    }

    /* A system call that reaches only for the calling task's own state
       does not need the lock - and only that makes it possible for two
       processes to be inside the kernel at once. The rest enters as
       before. */
    if (r->int_no == SYSCALL_VECTOR && syscall_lockless(r->eax)) {
        syscall_handler(r);
        /* A kill and a signal are no longer work "only on one's own":
           one ends the process, the other moves its stack. We take the
           lock only when there really is something to deliver, though -
           otherwise this path would differ from the ordinary one in
           nothing but its name. */
        if (task_killed() || task_signal_pending()) {
            biglock_enter();
            task_kill_pending();
            signal_deliver(r);
            biglock_leave();
        }
        return;
    }

    biglock_enter();
    isr_dispatch(r);
    biglock_leave();
}

static void isr_dispatch(struct regs *r)
{
    if (r->int_no == SYSCALL_VECTOR) {
        syscall_handler(r);
        task_kill_pending();    /* deliver a pending kill on return from a syscall too */
        signal_deliver(r);      /* ...and right after it any pending signals */
        return;
    }

    if (r->int_no < 32) {
        /* A page fault on a page given over to copy-on-write (fork) is
           not an error - make a private copy and resume the
           instruction. This also covers kernel writes (CR0.WP), e.g.
           when a syscall fills the buffer of a freshly forked
           process. */
        if (r->int_no == 14) {
            uint32_t cr2;
            __asm__ volatile("movl %%cr2, %0" : "=r"(cr2));
            if (paging_handle_cow(cr2, r->err_code))
                return;
            /* A heap or mmap page the process has not touched yet -
               allocate it now and resume the instruction */
            if (!(r->err_code & 0x1) && process_demand_page(cr2))
                return;
        }

        /* A breakpoint (int3) is a trap exception: report it and carry on */
        if (r->int_no == 3) {
            printk("[INT3] breakpoint pod eip=%x\n", r->eip);
            return;
        }

        /* An exception in ring 3: only the user process dies, not the system */
        if ((r->cs & 3) == 3) {
            printk("\n[jadro] proces '%s' ubity na rdzeniu %d: "
                   "%s (err=%x, eip=%x)\n",
                   task_current_name(), smp_core(),
                   exception_names[r->int_no], r->err_code, r->eip);
            if (r->int_no == 14) {
                uint32_t cr2;
                __asm__ volatile("movl %%cr2, %0" : "=r"(cr2));
                printk("[jadro] proces siegnal po adres %x\n", cr2);
            }
            task_exit(-1);          /* killed by a CPU exception */
        }

        printk("\n!!! WYJATEK CPU %u: %s (err=%x) pod eip=%x\n",
               r->int_no, exception_names[r->int_no], r->err_code, r->eip);
        if (r->int_no == 14) {
            uint32_t cr2;
            __asm__ volatile("movl %%cr2, %0" : "=r"(cr2));
            printk("Page fault pod adresem (CR2): %x\n", cr2);
        }
        printk("System zatrzymany.\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }

    /* The local APIC timer. It did not pass through the PIC, so there
       is nothing to close there - the acknowledgement goes to this
       core's own APIC. On a secondary core it is the only interrupt
       that arrives at all. */
    if (r->int_no == 48) {
        smp_timer_tick();
        return;
    }

    /* A wake-up from another core. It carries no content: the point
       was only to pull this processor out of the hlt instruction. */
    if (r->int_no == 49) {
        smp_wakeup_recv();
        return;
    }

    /* A mapping has changed on another core and it is waiting for our
       answer. Nothing here may take a lock: the core on the other side
       is holding one and standing still until we reply. */
    if (r->int_no == 50) {
        tlb_ipi_recv();
        return;
    }

    if (r->int_no >= IRQ_BASE && r->int_no < IRQ_BASE + 16) {
        int irq = r->int_no - IRQ_BASE;
        /* The EOI before the handler: the timer handler switches
           contexts, so the "end" of this interrupt may not come until
           the thread gets the CPU again - and the PIC cannot wait for
           that. Interrupt gates keep IF=0, so nesting is not a
           danger. */
        pic_send_eoi(irq);
        if (irq_handlers[irq])
            irq_handlers[irq](r);
    }

    /* On return to ring 3 deliver a pending kill - the process dies
       here, at the point of return into user mode */
    if ((r->cs & 3) == 3) {
        task_kill_pending();
        signal_deliver(r);
    }
}
