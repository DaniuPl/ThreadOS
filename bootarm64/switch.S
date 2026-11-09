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
