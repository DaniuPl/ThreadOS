/* Stuby przerwan dla trybu dlugiego
 *
 * Procesor odklada przy przerwaniu rip, cs, rflags, rsp i ss - i robi to
 * ZAWSZE, takze bez zmiany poziomu uprawnien. To roznica wobec trybu
 * 32-bitowego, gdzie stos byl odkladany tylko przy przejsciu z ring 3.
 *
 * Drugiej roznicy nie widac na pierwszy rzut oka: w trybie dlugim nie
 * ma juz instrukcji pushal. Szesnascie rejestrow trzeba odlozyc recznie
 * i w tej samej kolejnosci, w jakiej opisuje je struct regs.
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

/* Wyjatki 0-31; kod bledu odkladaja: 8, 10-14, 17, 21 */
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

/* Przerwania sprzetowe: IRQ 0-15 na wektorach 32-47 */
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

isr64_common:
    /* Kolejnosc odwrotna do pol struct regs - stos rosnie w dol */
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

    movq %rsp, %rdi              /* pierwszy argument: wskaznik na ramke */
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

    addq $16, %rsp               /* numer wektora i kod bledu */
    iretq

.section .note.GNU-stack, "", @progbits
