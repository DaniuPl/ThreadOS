/* Boot trampoline for secondary cores - long mode
 *
 * A processor woken by a SIPI starts in real mode, and it makes no
 * difference that the rest of the system has long been running in long
 * mode. It has to walk the whole road on its own: sixteen bits, then
 * thirty-two, then sixty-four - the same road the boot processor took
 * at startup, only without any help from the loader.
 *
 * It does not build page tables: it takes the ready ones, the very
 * tables the kernel uses. Their address is handed to it by the boot
 * processor in the ap_cr3 field.
 *
 * The block is linked with the kernel but executes at address 0x8000 -
 * hence every address being recomputed for its destination.
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

    /* Physical address extension: without it there is no long mode,
       because page table entries are eight bytes wide there */
    movl %cr4, %eax
    orl $CR4_PAE, %eax
    movl %eax, %cr4

    /* We take the page tables ready-made - the same ones the kernel
       walks. A secondary core has nothing to add to the mapping. */
    movl $OFF(ap_cr3), %eax
    movl (%eax), %eax
    movl %eax, %cr3

    /* Consent to long mode (bit LME) and to no-execute (bit NXE).
     *
     * NXE is not decoration. The kernel uses that bit to map everything
     * that is not meant to be code - among others the hardware
     * registers glued onto the identity mapping. With NXE off, bit 63
     * of a page table entry is RESERVED rather than "no execute": every
     * access through such an entry ends in a page fault. This core
     * walks the same tables as the boot one, so it must have the same
     * EFER - otherwise the first touch of the local APIC kills the
     * machine. */
    movl $MSR_EFER, %ecx
    rdmsr
    orl $EFER_LME | EFER_NXE, %eax
    wrmsr

    /* Turning paging on switches the processor into long mode - but
       only a far jump to a 64-bit descriptor makes it start executing
       code as 64-bit */
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

    /* And to work. The core number goes in RDI - as the long mode
       calling convention demands - while the work loop itself already
       lives in the kernel, where code is written in C.

       The function address goes through a register: a relative jump
       reaches two gigabytes at most, and the kernel lies beyond that
       range from where this code executes. */
    movq $OFF(ap_number), %rax
    movl (%rax), %edi
    movabsq $smp_ap_main, %rax
    call *%rax

1:  hlt
    jmp 1b

.align 16
ap_gdt:
    .quad 0x0000000000000000      /* null descriptor */
    .quad 0x00CF9A000000FFFF      /* 0x08: 32-bit code */
    .quad 0x00CF92000000FFFF      /* 0x10: 32-bit data */
    .quad 0x00AF9A000000FFFF      /* 0x18: 64-bit code (L bit) */
    .quad 0x00CF92000000FFFF      /* 0x20: data */
ap_gdt_end:

ap_gdtr:
    .word ap_gdt_end - ap_gdt - 1
    .long OFF(ap_gdt)

/* Filled in by the boot processor before every SIPI */
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

/* This block's stack is not executable - the linker wants to hear it */
.section .note.GNU-stack,"",@progbits
