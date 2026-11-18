/* What happens when the processor stops the kernel
 *
 * The syndrome register (ESR_EL1) says why: its top six bits are the
 * exception class - a breakpoint, a data abort, an instruction abort, a
 * system call - and the rest is detail for that class. For an abort,
 * FAR_EL1 holds the address that was touched. All of it is said on the
 * serial port in words, because the first time a real board faults the
 * serial port will be all there is.
 *
 * A breakpoint is the one exception the kernel steps over and carries
 * on from; it is how the way back out of an exception is tested. Every
 * other fault is the kernel's own bug, and it stops.
 *
 * What comes from a program at EL0 is the program's: a system call is
 * answered, and anything else ends the program (proc.c) - not the
 * kernel.
 */

#include <stdint.h>
#include "arm64.h"
#include "thread.h"
#include "uart.h"
#include "proc.h"

extern char exception_vectors[];

/* The slot numbers vectors.S passes: four kinds times four origins */
#define KIND_SYNC   0
#define KIND_IRQ    1

#define EC_INSN_ABORT_LOWER 0x20
#define EC_INSN_ABORT       0x21
#define EC_DATA_ABORT_LOWER 0x24
#define EC_DATA_ABORT       0x25
#define EC_BRK              0x3C
#define EC_SVC64            0x15

/* Slots 8 and on are a lower level's: a program's */
#define FROM_EL0(slot)      ((slot) >= 8)

void exceptions_init(void)
{
    write_sysreg(vbar_el1, (uint64_t)(uintptr_t)exception_vectors);
    __asm__ volatile("isb");
}

/* The fault status code of an abort, in words */
static void say_fault(uint32_t fsc)
{
    unsigned level = fsc & 3;
    switch (fsc & 0x3C) {
    case 0x04:
        printk("translation fault at level %u", level);
        return;
    case 0x08:
        printk("access flag fault at level %u", level);
        return;
    case 0x0C:
        printk("permission fault at level %u", level);
        return;
    }
    if (fsc == 0x21)
        printk("alignment fault");
    else
        printk("fault status 0x%x", fsc);
}

/* The fault status code of an abort, as a few words for a program's
   end */
static const char *fault_name(uint32_t fsc)
{
    switch (fsc & 0x3C) {
    case 0x04: return "translation fault";
    case 0x08: return "access flag fault";
    case 0x0C: return "permission fault";
    }
    return fsc == 0x21 ? "alignment fault" : "fault";
}

/* Whatever a program did that is not a system call ends it: the kernel
   is not the one at fault, and goes on */
static void from_program(struct trap_frame *f, uint32_t ec, uint32_t iss)
{
    if (ec == EC_SVC64) {
        proc_syscall(f);
        return;
    }
    if (ec == EC_DATA_ABORT_LOWER)
        proc_fault(f, (iss & (1u << 6)) ? "writing" : "reading",
                   read_sysreg(far_el1), fault_name(iss & 0x3F));
    if (ec == EC_INSN_ABORT_LOWER)
        proc_fault(f, "running", read_sysreg(far_el1),
                   fault_name(iss & 0x3F));
    proc_fault(f, "an exception at", f->elr,
               ec == 0 ? "an undefined instruction" :
               ec == EC_BRK ? "a breakpoint" : "not one it may take");
}

void arm64_exception(struct trap_frame *f, uint64_t slot)
{
    unsigned kind = slot & 3;
    if (kind == KIND_IRQ) {
        gic_irq();
        /* After the end of the interrupt is signalled, never before: a
           switch from here may not come back for a while, and the GIC
           holds back everything of lower priority until then */
        sched_preempt();
        return;
    }
    if (kind != KIND_SYNC) {
        printk("Exception: %s (slot %u) at 0x%lx - stopping.\n",
                kind == 2 ? "FIQ" : "SError", (uint32_t)slot, f->elr);
        arm64_halt();
    }

    uint64_t esr = read_sysreg(esr_el1);
    uint32_t ec = (esr >> 26) & 0x3F;
    uint32_t iss = esr & 0x1FFFFFF;

    if (FROM_EL0(slot)) {
        from_program(f, ec, iss);
        return;
    }

    if (ec == EC_BRK) {
        printk("Breakpoint (BRK #0x%x) at 0x%lx - stepping over it\n",
                iss & 0xFFFF, f->elr);
        f->elr += 4;
        return;
    }
    if (ec == EC_DATA_ABORT || ec == EC_DATA_ABORT_LOWER) {
        printk("Data abort: %s 0x%lx, ", (iss & (1u << 6)) ? "writing"
                                                            : "reading",
                read_sysreg(far_el1));
        say_fault(iss & 0x3F);
        printk(", at 0x%lx - stopping.\n", f->elr);
        arm64_halt();
    }
    if (ec == EC_INSN_ABORT || ec == EC_INSN_ABORT_LOWER) {
        printk("Instruction abort: running 0x%lx, ", read_sysreg(far_el1));
        say_fault(iss & 0x3F);
        printk(" - stopping.\n");
        arm64_halt();
    }
    printk("Exception: class 0x%x (ESR 0x%lx) at 0x%lx, address 0x%lx "
            "- stopping.\n", ec, esr, f->elr, read_sysreg(far_el1));
    arm64_halt();
}
