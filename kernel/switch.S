/* Context switching between kernel threads
 *
 * void context_switch(uint32_t *old_esp_store, uint32_t new_esp);
 *
 * Saves the callee-saved registers and the current thread's ESP, then
 * jumps onto the new thread's stack and pops its registers. From every
 * thread's point of view this is an ordinary function call that
 * "lasted" as long as the thread was set aside.
 */

.section .text
.global context_switch
context_switch:
    movl 4(%esp), %eax      /* where to save the old ESP */
    movl 8(%esp), %edx      /* the new ESP */

    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi

    movl %esp, (%eax)
    movl %edx, %esp

    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret

.section .note.GNU-stack, "", @progbits
