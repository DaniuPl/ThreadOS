/* Wejscie do jadra przez instrukcje syscall
 *
 * W trybie dlugim nie uzywa sie juz bramki przerwania do wolania jadra.
 * Instrukcja syscall jest szybsza, bo nie siega do pamieci: procesor
 * bierze adres procedury wprost z rejestru modelu (LSTAR), a adres
 * powrotu i flagi zostawia w rejestrach RCX i R11.
 *
 * Ma to jednak cene: syscall NIE przelacza stosu. Wchodzimy do jadra
 * ze stosem programu i pierwsze, co trzeba zrobic, to przesiasc sie na
 * wlasny - inaczej caly kod jadra dzialalby na pamieci, ktora proces
 * moze sobie dowolnie podmienic.
 */

.section .text
.code64

.global syscall64_entry
.type syscall64_entry, @function
syscall64_entry:
    /* Stos programu zapamietujemy w jednej komorce na krzyz, bo w tej
       chwili nie mamy ani jednego wolnego rejestru: RAX niesie numer
       wywolania, RCX adres powrotu, R11 flagi, a reszta to argumenty
       albo rejestry programu. Przerwania sa tu zgaszone (SFMASK) i az
       do przelozenia wartosci na wlasny stos nie ma zadnego wywolania,
       wiec nikt tej komorki miedzy nami nie podmieni.

       Wczesniej ta komorka trzymala stos programu przez CALE wywolanie
       i to bylo bledem: wystarczylo, ze wywolanie uspilo zadanie,
       a drugi proces wszedl do jadra w tym samym czasie - i pierwszy
       wracal do ring 3 na cudzy stos. Teraz wartosc ladu je na stosie
       jadra tego konkretnego zadania i nikt inny jej nie widzi. */
    movq %rsp, syscall64_scratch(%rip)
    movq syscall64_kernel_rsp(%rip), %rsp

    /* Odtwarzamy DOKLADNIE taka ramke, jaka odklada przerwanie.
     *
     * Instrukcja syscall nie odklada niczego - adres powrotu zostawia
     * w RCX, flagi w R11 - wiec te piec slow dopisujemy sami. Kosztuje
     * to kilkanascie instrukcji i powrot przez iretq zamiast sysretq,
     * a daje jedna rzecz: jadro widzi wywolanie systemowe i przerwanie
     * tak samo. Bez tego fork nie mialby skad wziac pelnego stanu
     * programu, ktory ma powielic. */
    pushq $0x1B                  /* ss */
    pushq syscall64_scratch(%rip) /* rsp programu */
    pushq %r11                   /* rflags zostawione przez syscall */
    pushq $0x23                  /* cs: kod uzytkownika, RPL 3 */
    pushq %rcx                   /* rip: adres powrotu */
    pushq $0                     /* err_code - wywolanie go nie ma */
    pushq $0                     /* int_no  - ani numeru wektora */

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
    call syscall64_handler

    jmp return_to_user_64_frame

.size syscall64_entry, . - syscall64_entry

/* Powrot do ring 3 z ramki lezacej pod RDI.
 *
 * Uzywa tego dziecko fork-a: dostaje kopie ramki rodzica i wraca nia
 * do ring 3 tak, jakby samo wlasnie wykonalo wywolanie systemowe -
 * tyle ze z zerem w RAX. */
.global return_to_user_64
.type return_to_user_64, @function
return_to_user_64:
    movq %rdi, %rsp
return_to_user_64_frame:
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

.size return_to_user_64, . - return_to_user_64

/* Wejscie w ring 3: iretq ze spreparowana ramka. Nie wraca.
     rdi = adres pierwszej instrukcji, rsi = wierzcholek stosu programu,
     rdx = wartosc, ktora program zastanie w RDI (jego argument) */
.global enter_ring3_64
.type enter_ring3_64, @function
enter_ring3_64:
    movq $0x1B, %rax             /* dane uzytkownika, RPL 3 */
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    pushq $0x1B                  /* ss */
    pushq %rsi                   /* rsp */
    pushq $0x202                 /* rflags z wlaczonymi przerwaniami */
    pushq $0x23                  /* cs: kod uzytkownika, RPL 3 */
    pushq %rdi                   /* rip */

    movq %rdx, %rdi              /* argument programu */

    /* Reszta rejestrow na zero. Program nie ma powodu ogladac tego,
       co jadro w nich zostawilo - a zostawia tam adresy wlasnych
       struktur, ktorych ring 3 nie powinien nawet znac. */
    xorl %eax, %eax
    xorl %ebx, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    xorl %esi, %esi
    xorl %ebp, %ebp
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    xorl %r10d, %r10d
    xorl %r11d, %r11d
    xorl %r12d, %r12d
    xorl %r13d, %r13d
    xorl %r14d, %r14d
    xorl %r15d, %r15d
    iretq

.size enter_ring3_64, . - enter_ring3_64

.section .data
.align 8
.global syscall64_kernel_rsp
.global syscall64_scratch
syscall64_kernel_rsp: .quad 0
syscall64_scratch:    .quad 0

/* Kod, ktory ma zadzialac w ring 3.
 *
 * Lezy w obrazie jadra tylko po to, zeby go stamtad skopiowac do ramki
 * oznaczonej jako dostepna dla uzytkownika. Nie ma w nim ani jednego
 * adresu bezwzglednego, wiec zadziala pod dowolnym adresem.
 *
 * Najpierw pyta procesor, w ktorym pierscieniu naprawde jest - selektor
 * CS zna tylko on - i mowi to jadru. Potem prosi o zakonczenie. */
.section .rodata
.align 16
.global user_payload_start
.global user_payload_end
user_payload_start:
    movw %cs, %ax                /* selektor kodu: dwa najnizsze bity
                                    to poziom uprawnien */
    movzwq %ax, %rdi
    movq $3, %rax                /* 3 = powiedz, w ktorym pierscieniu jestem */
    syscall
    movq $2, %rax                /* 2 = koniec programu */
    syscall
1:  jmp 1b                       /* jadro tu nie wroci */
user_payload_end:

.section .note.GNU-stack, "", @progbits
