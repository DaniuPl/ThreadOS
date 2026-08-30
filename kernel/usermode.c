/* Entering user mode (ring 3)
 *
 * A "return" to a lower privilege level through iret with a hand-built
 * frame: ss, esp, eflags (IF=1), cs, eip. The data segments have to be
 * loaded by hand - iret does not touch them.
 */

#include "include/usermode.h"
#include "include/gdt.h"
#include "include/isr.h"

#define USER_CS (GDT_USER_CODE | 3)   /* selectors with RPL=3 */
#define USER_DS (GDT_USER_DATA | 3)

void enter_ring3(uint32_t entry, uint32_t user_stack_top)
{
    __asm__ volatile(
        "cli\n\t"
        "movw %w0, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "pushl %0\n\t"
        "pushl %1\n\t"
        "pushl $0x202\n\t"
        "pushl %2\n\t"
        "pushl %3\n\t"
        "iret\n\t"
        :
        : "r"((uint32_t)USER_DS), "r"(user_stack_top),
          "r"((uint32_t)USER_CS), "r"(entry)
        : "eax", "memory");
    __builtin_unreachable();
}

/* Return to ring 3 from a saved register frame.
 *
 * fork() uses this: the child is a separate kernel thread that
 * immediately "returns from a syscall" at the same point of the
 * program as the parent. We lay the frame out in the order popal
 * expects and add at the end what iret takes off (eip, cs, eflags,
 * esp, ss).
 */
void enter_ring3_regs(const struct regs *r)
{
    volatile uint32_t f[13];
    f[0] = r->edi;  f[1] = r->esi;  f[2] = r->ebp;  f[3] = 0; /* esp - popal skips it */
    f[4] = r->ebx;  f[5] = r->edx;  f[6] = r->ecx;  f[7] = r->eax;
    f[8] = r->eip;  f[9] = r->cs;   f[10] = r->eflags | 0x200; /* IF=1 */
    f[11] = r->useresp;
    f[12] = r->ss;

    __asm__ volatile(
        "cli\n\t"
        "movw %w1, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movl %0, %%esp\n\t"
        "popal\n\t"
        "iret\n\t"
        :
        : "r"(&f[0]), "r"((uint32_t)USER_DS)
        : "eax", "memory");
    __builtin_unreachable();
}
