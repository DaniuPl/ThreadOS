/* ThreadOS on ARM64 - switching from one thread to another
 *
 * switch_to(&old->sp, new_sp) is an ordinary function call, so only
 * what a callee has to preserve needs saving: x19-x28, the frame
 * pointer x29 and the return address x30. They go onto the old
 * thread's stack, its stack pointer into *old_sp; then the new
 * thread's stack pointer is loaded, its registers popped, and 'ret'
 * returns into wherever that thread called switch_to from - or, for a
 * thread that has never run, into the entry the creator put in its x30.
 *
 * The kernel is built without the FP/SIMD registers, so there are
 * none of those to carry.
 */

.section .text
.global switch_to
switch_to:
    sub     sp, sp, #96
    stp     x19, x20, [sp, #0]
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]
    mov     x9, sp
    str     x9, [x0]

    mov     sp, x1
    ldp     x19, x20, [sp, #0]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     x29, x30, [sp, #80]
    add     sp, sp, #96
    ret

/* A program's FP/SIMD registers, to and from its thread's area
   (thread.h FPSTATE_WORDS): the kernel is built not to use them, so
   only these two touch them, and the assembler is told the core has
   them. */
.arch armv8-a+fp+simd
.global fp_save
fp_save:
    stp     q0, q1, [x0, #0]
    stp     q2, q3, [x0, #32]
    stp     q4, q5, [x0, #64]
    stp     q6, q7, [x0, #96]
    stp     q8, q9, [x0, #128]
    stp     q10, q11, [x0, #160]
    stp     q12, q13, [x0, #192]
    stp     q14, q15, [x0, #224]
    stp     q16, q17, [x0, #256]
    stp     q18, q19, [x0, #288]
    stp     q20, q21, [x0, #320]
    stp     q22, q23, [x0, #352]
    stp     q24, q25, [x0, #384]
    stp     q26, q27, [x0, #416]
    stp     q28, q29, [x0, #448]
    stp     q30, q31, [x0, #480]
    mrs     x1, fpcr
    mrs     x2, fpsr
    add     x0, x0, #512            /* past the reach of stp's offset */
    stp     x1, x2, [x0]
    ret

.global fp_restore
fp_restore:
    ldp     q0, q1, [x0, #0]
    ldp     q2, q3, [x0, #32]
    ldp     q4, q5, [x0, #64]
    ldp     q6, q7, [x0, #96]
    ldp     q8, q9, [x0, #128]
    ldp     q10, q11, [x0, #160]
    ldp     q12, q13, [x0, #192]
    ldp     q14, q15, [x0, #224]
    ldp     q16, q17, [x0, #256]
    ldp     q18, q19, [x0, #288]
    ldp     q20, q21, [x0, #320]
    ldp     q22, q23, [x0, #352]
    ldp     q24, q25, [x0, #384]
    ldp     q26, q27, [x0, #416]
    ldp     q28, q29, [x0, #448]
    ldp     q30, q31, [x0, #480]
    add     x0, x0, #512
    ldp     x1, x2, [x0]
    msr     fpcr, x1
    msr     fpsr, x2
    ret

/* Into a program, at EL0: enter_el0(entry, user sp, x0, x1). With
   interrupts off while ELR and SPSR are set - an interrupt in between
   would write over them - and every other register cleared, so that
   nothing of the kernel's goes along. SPSR 0 is EL0 with interrupts
   unmasked.

   enter_el0_stack(entry, user sp, x0, x1, kernel stack top) is the same
   from deep inside the kernel - exec, in the middle of a system call:
   the kernel's stack starts again at its top, or every exec would
   leave the depth of a system call behind on it for good. (SP_EL1 is
   where the next exception from EL0 puts its frame.) */
.global enter_el0_stack
enter_el0_stack:
    msr     daifset, #2
    mov     sp, x4
.global enter_el0
enter_el0:
    msr     daifset, #2
    msr     elr_el1, x0
    msr     sp_el0, x1
    msr     spsr_el1, xzr
    mov     x0, x2
    mov     x1, x3
    mov     x2, xzr
    mov     x3, xzr
    mov     x4, xzr
    mov     x5, xzr
    mov     x6, xzr
    mov     x7, xzr
    mov     x8, xzr
    mov     x9, xzr
    mov     x10, xzr
    mov     x11, xzr
    mov     x12, xzr
    mov     x13, xzr
    mov     x14, xzr
    mov     x15, xzr
    mov     x16, xzr
    mov     x17, xzr
    mov     x18, xzr
    mov     x19, xzr
    mov     x20, xzr
    mov     x21, xzr
    mov     x22, xzr
    mov     x23, xzr
    mov     x24, xzr
    mov     x25, xzr
    mov     x26, xzr
    mov     x27, xzr
    mov     x28, xzr
    mov     x29, xzr
    mov     x30, xzr
    eret
