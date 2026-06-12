/* Przełączanie kontekstu między wątkami jądra
 *
 * void context_switch(uint32_t *old_esp_store, uint32_t new_esp);
 *
 * Zapisuje rejestry callee-saved i ESP bieżącego wątku, po czym
 * przeskakuje na stos nowego wątku i zdejmuje jego rejestry.
 * Z perspektywy każdego wątku to zwykłe wywołanie funkcji, które
 * "trwało" tyle, ile wątek był odłożony.
 */

.section .text
.global context_switch
context_switch:
    movl 4(%esp), %eax      /* gdzie zapisac stary ESP */
    movl 8(%esp), %edx      /* nowy ESP */

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
