/* Context switch in long mode
 *
 * We save only the registers the calling convention requires a callee
 * to preserve - the rest belong to the caller anyway and already sit on
 * its stack. There are six of them instead of four, which is the whole
 * difference from the 32-bit version.
 *
 * The rest of a thread's state - everything that happened between
 * calls - stays on its own stack. That is why moving RSP is enough:
 * a thread preempted by an interrupt has a full interrupt frame there
 * and will return through iretq, while a thread that gave up the CPU
 * itself returns from an ordinary call.
 *
 *   rdi = where to store the old thread's RSP
 *   rsi = RSP of the thread that is to run
 */

.section .text
.code64
.global context_switch64
.type context_switch64, @function
context_switch64:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, (%rdi)            /* remember the old thread's stack */
    movq %rsi, %rsp              /* and move onto the new one's */

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

.size context_switch64, . - context_switch64
.section .note.GNU-stack, "", @progbits
