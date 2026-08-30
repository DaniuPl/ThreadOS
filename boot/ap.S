/* Boot trampoline for the remaining CPUs
 *
 * A processor woken by a SIPI starts the way every PC started in 1981:
 * in real mode, sixteen bits wide, at address (vector << 12). To join a
 * system that has long been in protected mode it has to walk the same
 * road as the boot processor - only on its own and without the BIOS.
 *
 * Protected mode alone is not enough. The kernel runs on page tables
 * and it is they that give addresses their meaning - a core without
 * paging enabled reads, at a kernel address, whatever lies there
 * PHYSICALLY. For the kernel itself that comes to the same thing,
 * because low memory is mapped one to one, but a user program lives
 * where without page tables there is nothing at all. So the secondary
 * core turns paging on using the very tables the kernel walks - their
 * address is handed to it by the boot processor in the ap_cr3 field.
 *
 * This code is linked together with the kernel but EXECUTES at address
 * 0x8000, where smp.c copies it. That is why every address appearing
 * here is computed as AP_BASE plus the distance from the start of the
 * block: the distances are known at link time and the start is known
 * up front. Addresses go into a register first and only then into use -
 * the assembler then has exactly one obvious meaning to choose from.
 */

#define AP_BASE 0x8000
#define OFF(x)  (AP_BASE + ((x) - ap_trampoline))

.section .text
.globl ap_trampoline
.globl ap_trampoline_end
.globl ap_counter
.globl ap_stack
.globl ap_number
.globl ap_cr3

.code16
ap_trampoline:
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    /* The descriptor table lies in the same block, so its address has
       to be recomputed for the destination too */
    movw $OFF(ap_gdtr), %si
    lgdtl (%si)

    movl %cr0, %eax
    orl $1, %eax                  /* protected mode */
    movl %eax, %cr0

    /* A far jump switches the code descriptor to the 32-bit one */
    ljmpl $0x08, $OFF(ap_protected)

.code32
ap_protected:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    /* Paging - the same tables the kernel has. Bit PG turns address
       translation on, bit WP makes ring 0 obey write protection too;
       both must be set exactly as on the boot core, otherwise
       copy-on-write behaves differently on every core. The trampoline
       code lies in low memory, mapped one to one, so the next
       instruction is at the same address it was a moment ago. */
    movl $OFF(ap_cr3), %eax
    movl (%eax), %eax
    movl %eax, %cr3
    movl %cr0, %eax
    orl $0x80010000, %eax
    movl %eax, %cr0
    jmp 1f
1:

    /* The stack is supplied by the boot processor, one per woken core:
       there is no way yet to allocate one here */
    movl $OFF(ap_stack), %eax
    movl (%eax), %esp

    /* Report in. The counter is incremented atomically, because several
       processors may be starting and each writes the same word. */
    movl $OFF(ap_counter), %eax
    lock incl (%eax)

    /* And to work. The core number goes on the stack as an argument;
       the work loop itself already lives in the kernel - where code is
       written in C rather than in addresses computed by hand.

       This core has no exception or interrupt table yet, so the
       function on the other side may not make mistakes, and it never
       returns. */
    movl $OFF(ap_number), %eax
    pushl (%eax)
    /* The function address goes through a register, not a relative
       jump: this code executes at 0x8000, while the displacement
       computed at link time describes the distance from where it SITS
       in the kernel image. A relative jump would land in the void. */
    movl $smp_ap_main, %eax
    call *%eax

1:  hlt
    jmp 1b

.align 8
ap_gdt:
    .quad 0x0000000000000000      /* null descriptor */
    .quad 0x00CF9A000000FFFF      /* 32-bit code, full range */
    .quad 0x00CF92000000FFFF      /* 32-bit data, full range */
ap_gdt_end:

ap_gdtr:
    .word ap_gdt_end - ap_gdt - 1
    .long OFF(ap_gdt)

/* Filled in by the boot processor before every SIPI */
.align 4
ap_stack:
    .long 0
ap_number:
    .long 0
ap_counter:
    .long 0
ap_cr3:
    .long 0

ap_trampoline_end:

/* This block's stack is not executable - the linker wants to hear it */
.section .note.GNU-stack,"",@progbits
