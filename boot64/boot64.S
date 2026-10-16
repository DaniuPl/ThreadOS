/* ThreadOS 64 - entering long mode
 *
 * GRUB leaves us in 32-bit protected mode, because that is what
 * Multiboot 2 prescribes. Getting into long mode takes several steps
 * in a strictly fixed order:
 *
 *   1. page tables - in long mode paging is MANDATORY, so they must be
 *      ready before it is turned on,
 *   2. PAE in CR4 - without it entries are 32-bit and long mode will
 *      not start,
 *   3. the LME bit in the EFER register - consent to long mode,
 *   4. PG in CR0 - paging; the processor then enters compatibility
 *      mode, not full long mode yet,
 *   5. a far jump to a segment with the L bit - only that really
 *      switches the processor over.
 *
 * We identity-map the first four gigabytes with 2 MiB pages. This is
 * not about memory - there is not that much of it - but about the fact
 * that device registers and the framebuffer sit high, just below four
 * gigabytes. Four tables of 512 entries describe all of it and fit
 * inside the kernel image, without any memory allocation, which we do
 * not know how to do at this stage yet.
 */

.set MB2_MAGIC, 0xE85250D6
.set MB2_ARCH,  0

.section .multiboot, "a", @progbits
.align 8
mb2_header:
.long MB2_MAGIC
.long MB2_ARCH
.long mb2_header_end - mb2_header
.long -(MB2_MAGIC + MB2_ARCH + (mb2_header_end - mb2_header))

    .align 8
    .short 1                         /* information request */
    .short 0
    .long 8 + 5 * 4
    .long 1                          /* command line */
    .long 3                          /* modules */
    .long 6                          /* memory map */
    .long 14                         /* RSDP (ACPI 1.0) */
    .long 15                         /* RSDP (ACPI 2.0) */

    .align 8
    .short 6                         /* modules aligned on pages */
    .short 0
    .long 8

    /* Console: the kernel can write into EGA character memory, not only
       into a framebuffer. This is how the loader learns it may leave
       text mode alone - otherwise, seeing the video mode request
       below, it would always switch to graphics. */
    .align 8
    .short 4                         /* console flags */
    .short 0
    .long 12
    .long 2                          /* EGA text mode support */

    /* Video mode: zero means "no preference". Asking for a particular
       resolution makes sense under a BIOS, where the card knows the
       whole list of VESA modes. Under UEFI the choice is whatever the
       firmware happens to expose through GOP - and when we ask for
       something outside that list, the loader sets up NOTHING. And
       under UEFI there is no text mode to fall back to: the system
       would be left without a picture. */
    .align 8
    .short 5                         /* video mode */
    .short 0
    .long 20
    .long 0                          /* width - any */
    .long 0                          /* height - any */
    .long 0                          /* depth - any */

    .align 8
    .short 0
    .short 0
    .long 8
mb2_header_end:

/* Page tables: one frame per level. The 4 KiB alignment is required -
   the processor takes only the upper address bits from an entry. */
.section .bss
.align 4096
.global boot_pml4
boot_pml4:  .skip 4096
boot_pdpt:  .skip 4096
boot_pd:    .skip 4 * 4096          /* four tables = four gigabytes */

.align 16
stack_bottom:
.skip 16384
.global stack_top
stack_top:

.section .rodata
.align 8
/* The long mode descriptor table. In long mode the base and limit no
   longer mean anything - what counts is the L bit (64-bit code) and
   the privilege level. The data descriptor exists only so that there
   is something to load into the segment registers. */
gdt64:
    .quad 0                                  /* null entry */
gdt64_code:
    .quad (1<<43) | (1<<44) | (1<<47) | (1<<53)   /* code, present, 64-bit */
gdt64_data:
    .quad (1<<44) | (1<<47) | (1<<41)             /* data, present, writable */
gdt64_end:
gdt64_ptr:
    .short gdt64_end - gdt64 - 1
    .long gdt64

.section .text
.code32
.global _start
.type _start, @function
_start:
    movl $stack_top, %esp
    movl %eax, %edi                  /* magic - to be passed on */
    movl %ebx, %esi                  /* pointer to the loader's information */

    /* --- 1. Page tables: the first gigabyte in 2 MiB pages --- */
    movl $boot_pdpt, %eax
    orl  $0x03, %eax                 /* present + writable */
    movl %eax, boot_pml4

    /* Four entries of the top-level directory - one per gigabyte */
    movl $boot_pd, %eax
    orl  $0x03, %eax
    movl $0, %ecx
2:
    movl %eax, boot_pdpt(, %ecx, 8)
    movl $0, boot_pdpt + 4(, %ecx, 8)
    addl $4096, %eax
    incl %ecx
    cmpl $4, %ecx
    jne 2b

    /* 2048 pages of 2 MiB: the address of each is its number shifted by
       21 bits - the highest comes out at 0xFFE00000, so the upper half
       of the entry stays zero */
    movl $0, %ecx
1:
    movl %ecx, %eax
    shll $21, %eax
    orl  $0x83, %eax                 /* present + writable + HUGE PAGE */
    movl %eax, boot_pd(, %ecx, 8)
    movl $0, boot_pd + 4(, %ecx, 8)
    incl %ecx
    cmpl $2048, %ecx
    jne 1b

    movl $boot_pml4, %eax
    movl %eax, %cr3

    /* --- 2. PAE --- */
    movl %cr4, %eax
    orl  $(1 << 5), %eax
    movl %eax, %cr4

    /* --- 3. The LME bit in EFER (model register 0xC0000080) --- */
    movl $0xC0000080, %ecx
    rdmsr
    orl  $(1 << 8), %eax
    wrmsr

    /* --- 4. Paging: the processor enters compatibility mode --- */
    /* Together with paging (bit 31) we turn on write protection
       (bit 16). Without it the kernel may write over pages marked
       read-only, and that kills copy-on-write: a page shared by two
       processes would be quietly overwritten instead of being split. */
    movl %cr0, %eax
    orl  $((1 << 31) | (1 << 16)), %eax
    movl %eax, %cr0

    /* --- 5. Only a far jump really switches it over --- */
    lgdt gdt64_ptr
    ljmp $0x08, $long_start

.code64
long_start:
    /* In long mode the segment registers mean almost nothing, but
       they still have to point at a valid descriptor */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    movq $stack_top, %rsp
    /* the magic and the pointer already wait in edi/esi - we clear the
       upper halves, because in 32-bit mode they did not exist */
    movl %edi, %edi
    movl %esi, %esi
    call kmain64

    cli
2:  hlt
    jmp 2b

.size _start, . - _start
.section .note.GNU-stack, "", @progbits
