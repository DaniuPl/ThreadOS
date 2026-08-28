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
    movq %rsp, syscall64_user_rsp(%rip)      /* zapamietaj stos programu */
    movq syscall64_kernel_rsp(%rip), %rsp    /* i przesiadz sie na swoj */

    pushq %rcx                   /* adres powrotu zostawiony przez syscall */
    pushq %r11                   /* flagi zostawione przez syscall */

    /* Rejestry, ktore niszczy funkcja C - odkladamy je i oddajemy
       programowi nietkniete.
     *
     * Bez tego umowa z programem brzmialaby "syscall psuje polowe
     * rejestrow" i kazdy program musialby o tym pamietac. Kompilator
     * o niczym nie wie: trzyma sobie stala w RDX przez cale wywolanie
     * i uzywa jej potem jako numeru nastepnego wywolania - a tam lezy
     * juz smiec po jadrze. Taniej jest raz odlozyc szesc rejestrow
     * niz w kazdym programie pamietac o szesciu pulapkach. */
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %r8
    pushq %r9
    pushq %r10

    /* Nasza konwencja: numer w rax, argumenty w rdi/rsi. Funkcja C
       oczekuje ich w rdi/rsi/rdx, wiec przesuwamy o jedno w prawo. */
    movq %rsi, %rdx
    movq %rdi, %rsi
    movq %rax, %rdi
    call syscall64_handler

    popq %r10
    popq %r9
    popq %r8
    popq %rdx
    popq %rsi
    popq %rdi
    popq %r11
    popq %rcx
    movq syscall64_user_rsp(%rip), %rsp
    sysretq

.size syscall64_entry, . - syscall64_entry

/* Wejscie w ring 3: iretq ze spreparowana ramka. Nie wraca.
     rdi = adres pierwszej instrukcji, rsi = wierzcholek stosu programu */
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
    iretq

.size enter_ring3_64, . - enter_ring3_64

.section .data
.align 8
.global syscall64_kernel_rsp
.global syscall64_user_rsp
syscall64_kernel_rsp: .quad 0
syscall64_user_rsp:   .quad 0

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
