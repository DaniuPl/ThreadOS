/* Przelaczenie kontekstu w trybie dlugim
 *
 * Zapisujemy tylko rejestry, ktore wedlug konwencji wywolan musi
 * zachowac funkcja wolana - reszta i tak nalezy do wolajacego i lezy
 * juz na jego stosie. Rejestrow tych jest szesc zamiast czterech,
 * co jest cala roznica wobec wersji 32-bitowej.
 *
 * Reszta stanu watku - wszystko, co dzialo sie miedzy wywolaniami -
 * zostaje na jego wlasnym stosie. Dlatego wystarczy przestawic RSP:
 * watek wywlaszczony przerwaniem ma tam pelna ramke przerwania
 * i wroci przez iretq, a watek, ktory sam oddal procesor, wroci
 * ze zwyklego wywolania.
 *
 *   rdi = gdzie zapisac RSP starego watku
 *   rsi = RSP watku, ktory ma ruszyc
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

    movq %rsp, (%rdi)            /* zapamietaj stos starego watku */
    movq %rsi, %rsp              /* i przejdz na stos nowego */

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

.size context_switch64, . - context_switch64
.section .note.GNU-stack, "", @progbits
