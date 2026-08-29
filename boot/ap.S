/* Trampolina rozruchowa dla pozostalych procesorow
 *
 * Procesor obudzony sygnalem SIPI zaczyna prace tak, jak zaczynal
 * kazdy pecet w 1981 roku: w trybie rzeczywistym, szesnastobitowo,
 * pod adresem (wektor << 12). Zeby dolaczyc do systemu, ktory dawno
 * juz jest w trybie chronionym, musi przejsc te sama droge, co
 * procesor rozruchowy - tyle ze sam i bez pomocy BIOS-u.
 *
 * Ten kod jest linkowany razem z jadrem, ale WYKONUJE sie pod
 * adresem 0x8000, dokad kopiuje go smp.c. Dlatego kazdy adres, ktory
 * tu pada, liczy sie jako AP_BASE plus odleglosc od poczatku bloku:
 * odleglosci sa znane przy linkowaniu, a poczatek znamy z gory.
 * Adresy trafiaja najpierw do rejestru, a dopiero potem w uzycie -
 * asembler ma wtedy jedno oczywiste znaczenie do wyboru.
 */

#define AP_BASE 0x8000
#define OFF(x)  (AP_BASE + ((x) - ap_trampolina))

.section .text
.globl ap_trampolina
.globl ap_trampolina_koniec
.globl ap_licznik
.globl ap_stos
.globl ap_numer

.code16
ap_trampolina:
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    /* Tablica deskryptorow lezy w tym samym bloku, wiec i jej adres
       trzeba przeliczyc na miejsce docelowe */
    movw $OFF(ap_gdtr), %si
    lgdtl (%si)

    movl %cr0, %eax
    orl $1, %eax                  /* tryb chroniony */
    movl %eax, %cr0

    /* Skok dalekomorski przelacza deskryptor kodu na 32-bitowy */
    ljmpl $0x08, $OFF(ap_chroniony)

.code32
ap_chroniony:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    /* Stos podaje procesor rozruchowy, po jednym na kazdy obudzony:
       tutaj nie ma jeszcze zadnego sposobu, zeby go sobie przydzielic */
    movl $OFF(ap_stos), %eax
    movl (%eax), %esp

    /* Zglos sie. Licznik rosnie niepodzielnie, bo procesorow moze
       ruszac kilka i kazdy pisze w to samo slowo. */
    movl $OFF(ap_licznik), %eax
    lock incl (%eax)

    /* I do roboty. Numer rdzenia idzie na stos jako argument, a sama
       petla robocza siedzi juz w jadrze - tam, gdzie kod pisze sie
       w C, a nie w adresach liczonych na piechote.

       Ten rdzen nie ma tablicy wyjatkow ani przerwan, wiec funkcja
       po drugiej stronie nie moze sie mylic i nie wraca. */
    movl $OFF(ap_numer), %eax
    pushl (%eax)
    /* Adres funkcji przez rejestr, nie skokiem wzglednym: ten kod
       wykonuje sie pod 0x8000, a przesuniecie policzone przy
       linkowaniu opisuje odleglosc od miejsca, w ktorym LEZY
       w obrazie jadra. Skok wzgledny trafilby wiec w prozne. */
    movl $smp_ap_glowna, %eax
    call *%eax

1:  hlt
    jmp 1b

.align 8
ap_gdt:
    .quad 0x0000000000000000      /* deskryptor zerowy */
    .quad 0x00CF9A000000FFFF      /* kod 32-bitowy, caly zakres */
    .quad 0x00CF92000000FFFF      /* dane 32-bitowe, caly zakres */
ap_gdt_koniec:

ap_gdtr:
    .word ap_gdt_koniec - ap_gdt - 1
    .long OFF(ap_gdt)

/* Wypelniane przez procesor rozruchowy przed kazdym SIPI */
.align 4
ap_stos:
    .long 0
ap_numer:
    .long 0
ap_licznik:
    .long 0

ap_trampolina_koniec:

/* Stos tego bloku nie jest wykonywalny - linker chce to slyszec */
.section .note.GNU-stack,"",@progbits
