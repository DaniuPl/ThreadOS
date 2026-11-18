/* ThreadOS on ARM64 - the exception vector table
 *
 * On x86 an interrupt descriptor table holds 256 addresses. ARM has
 * sixteen slots of 128 bytes each, holding code rather than addresses:
 * four kinds of exception (synchronous, IRQ, FIQ, SError) for each of
 * four origins - this level on SP_EL0, this level on its own stack,
 * a lower level running AArch64, and one running AArch32. VBAR_EL1
 * points at the table, which must be aligned to 2 KiB.
 *
 * 128 bytes is too little to save 31 registers, so each slot saves two,
 * notes which slot it was in x1 and jumps to the common part, which
 * saves the rest with the return address and state (ELR and SPSR),
 * calls arm64_exception(frame, slot) and puts everything back. Putting
 * ELR back is the point: the handler may move it on, to step over the
 * instruction that trapped.
 */

#define FRAME 288           /* 31 registers, ELR, SPSR, SP_EL0, padding */

.macro slot n
    .balign 0x80
    sub     sp, sp, #FRAME
    stp     x0, x1, [sp, #0]
    mov     x1, #\n
    b       exception_common
.endm

.section .text
.balign 2048
.global exception_vectors
exception_vectors:
    slot 0          /* this level, SP_EL0: sync, IRQ, FIQ, SError */
    slot 1
    slot 2
    slot 3
    slot 4          /* this level, own stack - where the kernel runs */
    slot 5
    slot 6
    slot 7
    slot 8          /* a lower level, AArch64 - programs */
    slot 9
    slot 10
    slot 11
    slot 12         /* a lower level, AArch32 */
    slot 13
    slot 14
    slot 15

exception_common:
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    mrs     x2, elr_el1
    mrs     x3, spsr_el1
    stp     x30, x2, [sp, #240]
    /* And a program's stack pointer: while it waits in here another
       program may run on this core, with a stack of its own */
    mrs     x4, sp_el0
    stp     x3, x4, [sp, #256]

    mov     x0, sp
    bl      arm64_exception         /* (frame, slot in x1) */

/* The way out of an exception, with the frame at sp: a forked program's
   first return to EL0 comes in here too (fork_return) */
.global exception_return
exception_return:
    ldp     x30, x2, [sp, #240]
    ldp     x3, x4, [sp, #256]
    msr     elr_el1, x2
    msr     spsr_el1, x3
    msr     sp_el0, x4
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldp     x0, x1, [sp, #0]
    add     sp, sp, #FRAME
    eret

/* fork_return(frame): a new thread of the kernel's leaves for EL0 as if
   from an exception it never took - a forked program's child, whose
   registers are its parent's at the system call, the answer 0. The
   frame is on this thread's own stack, and the stack goes on below it. */
.global fork_return
fork_return:
    msr     daifset, #2
    mov     sp, x0
    b       exception_return
