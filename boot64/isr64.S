/* Interrupt stubs for long mode
 *
 * On an interrupt the processor pushes rip, cs, rflags, rsp and ss -
 * and it does so ALWAYS, even without a change of privilege level.
 * That is a difference from 32-bit mode, where the stack was only
 * pushed when coming from ring 3.
 *
 * The second difference is less obvious at first glance: long mode no
 * longer has the pushal instruction. Sixteen registers have to be
 * pushed by hand, and in exactly the order struct regs describes them.
 */

.macro ISR_NOERR n
.global isr64_\n
isr64_\n:
    pushq $0
    pushq $\n
    jmp isr64_common
.endm

.macro ISR_ERR n
.global isr64_\n
isr64_\n:
    pushq $\n
    jmp isr64_common
.endm

.section .text
.code64

/* Exceptions 0-31; the ones that push an error code: 8, 10-14, 17, 21 */
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
ISR_ERR   21
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

/* Hardware interrupts: IRQ 0-15 on vectors 32-47 */
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

isr64_common:
    /* Reverse order of the struct regs fields - the stack grows down */
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, %rdi              /* first argument: pointer to the frame */
    call isr64_handler

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    addq $16, %rsp               /* vector number and error code */
    iretq

.section .note.GNU-stack, "", @progbits
