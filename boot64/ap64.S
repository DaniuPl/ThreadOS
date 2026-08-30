/* Trampolina rozruchowa rdzeni wtornych - tryb dlugi
 *
 * Procesor obudzony sygnalem SIPI zaczyna w trybie rzeczywistym, i nie
 * ma znaczenia, ze reszta systemu pracuje od dawna w trybie dlugim.
 * Musi przejsc cala droge sam: szesnascie bitow, potem trzydziesci
 * dwa, potem szescdziesiat cztery - te sama, ktora przy starcie
 * przeszedl procesor rozruchowy, tylko bez pomocy loadera.
 *
 * Tablic stron nie buduje: bierze gotowe, te same, ktorych uzywa
 * jadro. Ich adres podaje mu procesor rozruchowy w polu ap_cr3.
 *
 * Blok jest linkowany z jadrem, ale wykonuje sie pod adresem 0x8000-
 * stad przeliczanie kazdego adresu na miejsce docelowe.
 */

#define AP_BASE 0x8000
#define OFF(x)  (AP_BASE + ((x) - ap_trampoline))

#define CR4_PAE   (1 << 5)
#define CR0_PG    (1 << 31)
#define CR0_PE    (1 << 0)
#define MSR_EFER  0xC0000080
#define EFER_LME  (1 << 8)
#define EFER_NXE  (1 << 11)

.section .text
.globl ap_trampoline
.globl ap_trampoline_end
.globl ap_counter
.globl ap_stack
.globl ap_cr3
.globl ap_number

.code16
ap_trampoline:
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    movw $OFF(ap_gdtr), %si
    lgdtl (%si)

    movl %cr0, %eax
    orl $CR0_PE, %eax
    movl %eax, %cr0
    ljmpl $0x08, $OFF(ap_protected)

.code32
ap_protected:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    /* Adresowanie fizyczne z rozszerzeniem: bez tego nie ma trybu
       dlugiego, bo wpisy tablic stron maja tam po osiem bajtow */
    movl %cr4, %eax
    orl $CR4_PAE, %eax
    movl %eax, %cr4

    /* Tablice stron biezemy gotowe - te same, po ktorych chodzi
       jadro. Rdzen wtorny nie ma czego dokladac do mapowania. */
    movl $OFF(ap_cr3), %eax
    movl (%eax), %eax
    movl %eax, %cr3

    /* Zgoda na tryb dlugi (bit LME) i na zakaz wykonywania (bit NXE).
     *
     * NXE nie jest ozdoba. Jadro mapuje tym bitem wszystko, co nie ma
     * byc kodem - miedzy innymi rejestry sprzetu doklejone do mapowania
     * identycznosciowego. Gdy NXE jest wylaczone, bit 63 wpisu tablicy
     * stron jest ZAREZERWOWANY, a nie "zakaz wykonywania": kazdy dostep
     * przez taki wpis konczy sie bledem strony. Ten rdzen chodzi po tych
     * samych tablicach, co rozruchowy, wiec musi miec ten sam EFER -
     * inaczej pierwsze siegniecie po lokalny APIC zabija maszyne. */
    movl $MSR_EFER, %ecx
    rdmsr
    orl $EFER_LME | EFER_NXE, %eax
    wrmsr

    /* Wlaczenie stronicowania przelacza procesor w tryb dlugi -
       ale dopiero skok dalekomorski na deskryptor 64-bitowy sprawia,
       ze zaczyna wykonywac kod jako 64-bitowy */
    movl %cr0, %eax
    orl $CR0_PG, %eax
    movl %eax, %cr0

    ljmpl $0x18, $OFF(ap_long)

.code64
ap_long:
    movw $0x20, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    movq $OFF(ap_stack), %rax
    movq (%rax), %rsp

    movq $OFF(ap_counter), %rax
    lock incl (%rax)

    /* I do roboty. Numer rdzenia w RDI - tak, jak kaze konwencja
       wywolan trybu dlugiego - a sama petla robocza siedzi juz
       w jadrze, gdzie kod pisze sie w C.

       Adres funkcji przez rejestr: skok wzgledny siegnie najwyzej
       dwa gigabajty, a jadro lezy poza tym zasiegiem od miejsca,
       w ktorym ten kod sie wykonuje. */
    movq $OFF(ap_number), %rax
    movl (%rax), %edi
    movabsq $smp_ap_main, %rax
    call *%rax

1:  hlt
    jmp 1b

.align 16
ap_gdt:
    .quad 0x0000000000000000      /* deskryptor zerowy */
    .quad 0x00CF9A000000FFFF      /* 0x08: kod 32-bitowy */
    .quad 0x00CF92000000FFFF      /* 0x10: dane 32-bitowe */
    .quad 0x00AF9A000000FFFF      /* 0x18: kod 64-bitowy (bit L) */
    .quad 0x00CF92000000FFFF      /* 0x20: dane */
ap_gdt_end:

ap_gdtr:
    .word ap_gdt_end - ap_gdt - 1
    .long OFF(ap_gdt)

/* Wypelniane przez procesor rozruchowy przed kazdym SIPI */
.align 8
ap_stack:
    .quad 0
ap_cr3:
    .long 0
ap_number:
    .long 0
ap_counter:
    .long 0

ap_trampoline_end:

/* Stos tego bloku nie jest wykonywalny - linker chce to slyszec */
.section .note.GNU-stack,"",@progbits
