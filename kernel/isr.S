/* Interrupt stubs
 *
 * On an interrupt the CPU puts eflags/cs/eip on the stack (plus an
 * error code for some exceptions). The stub adds the vector number
 * (and a zero error code where the CPU does not supply one), saves the
 * registers and calls isr_handler() in C, passing a pointer to a regs
 * structure.
 */

/* An exception with no error code from the CPU - we add a zero one so
   that the frame always looks the same */
.macro ISR_NOERR n
.global isr\n
isr\n:
    pushl $0
    pushl $\n
    jmp isr_common
.endm

/* An exception for which the CPU puts an error code down itself */
.macro ISR_ERR n
.global isr\n
isr\n:
    pushl $\n
    jmp isr_common
.endm

.section .text

/* CPU exceptions 0-31. The ones that push an error code: 8, 10-14, 17 */
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

/* IRQ 0-15 land on vectors 32-47 after the PIC is remapped */
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

/* The local APIC timer - every core has its own, so it does not go
   through the PIC and has no IRQ number. Vector 48 sits just past them. */
ISR_NOERR 48

/* Inter-core wake-up: the boot core uses it to wake sleeping cores once
   it has work for them. It carries no content - what matters is only
   that an interrupt pulls the processor out of the hlt instruction. */
ISR_NOERR 49

/* A changed mapping: the core that changed it asks the others to throw
   away what they worked out from the old entry - and waits for the
   answer. See kernel/tlb.c. */
ISR_NOERR 50

/* Syscalls: int 0x80 (a gate with DPL=3, reachable from ring 3) */
ISR_NOERR 128

isr_common:
    pushal
    pushl %ds
    pushl %es
    pushl %fs
    pushl %gs

    /* Load the kernel data segments (it matters once ring 3 arrives) */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    pushl %esp          /* the argument: a pointer to struct regs */
    call isr_handler
    addl $4, %esp

    popl %gs
    popl %fs
    popl %es
    popl %ds
    popal
    addl $8, %esp       /* drop int_no and err_code */
    iret

.section .note.GNU-stack, "", @progbits
