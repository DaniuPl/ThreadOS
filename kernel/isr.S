/* Stuby przerwań
 *
 * CPU przy przerwaniu odkłada na stos eflags/cs/eip (+ kod błędu dla
 * niektórych wyjątków). Stub dokłada numer wektora (i zerowy kod błędu
 * tam, gdzie CPU go nie daje), zapisuje rejestry i woła isr_handler()
 * z kodu C, przekazując wskaźnik na strukturę regs.
 */

/* Wyjątek bez kodu błędu od CPU - dokładamy zerowy dla spójności ramki */
.macro ISR_NOERR n
.global isr\n
isr\n:
    pushl $0
    pushl $\n
    jmp isr_common
.endm

/* Wyjątek, przy którym CPU sam odkłada kod błędu */
.macro ISR_ERR n
.global isr\n
isr\n:
    pushl $\n
    jmp isr_common
.endm

.section .text

/* Wyjątki CPU 0-31. Kod błędu odkładają: 8, 10-14, 17 */
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

/* IRQ 0-15 po remapowaniu PIC trafiają na wektory 32-47 */
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

isr_common:
    pushal
    pushl %ds
    pushl %es
    pushl %fs
    pushl %gs

    /* Załaduj segmenty danych jądra (istotne, gdy dojdzie ring 3) */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    pushl %esp          /* argument: wskaźnik na struct regs */
    call isr_handler
    addl $4, %esp

    popl %gs
    popl %fs
    popl %es
    popl %ds
    popal
    addl $8, %esp       /* zdejmij int_no i err_code */
    iret

.section .note.GNU-stack, "", @progbits
