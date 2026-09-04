/* ProtonOS - kernel entry point (Multiboot 1)
 *
 * GRUB loads the kernel and jumps to _start in 32-bit protected mode.
 * Here we set up a stack and call kmain() written in C.
 */

/* Multiboot header constants */
.set MB_ALIGN,    1 << 0             /* align loaded modules on page boundaries */
.set MB_MEMINFO,  1 << 1             /* hand over the memory map */
.set MB_FLAGS,    MB_ALIGN | MB_MEMINFO
.set MB_MAGIC,    0x1BADB002
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

/* Multiboot header - must be within the first 8 KiB of the image */
.section .multiboot, "a", @progbits
.align 4
.long MB_MAGIC
.long MB_FLAGS
.long MB_CHECKSUM

/* Multiboot 2 header
 *
 * The image carries both headers at once, and that is deliberate: QEMU
 * started with -kernel understands only the older one, while GRUB
 * prefers the newer. The loader picks the one it knows and passes a
 * different magic in eax - the kernel tells from it which form the
 * machine description arrived in.
 *
 * The newer one is the road to UEFI: there is neither a BIOS nor old
 * Multiboot there, and the pointer to the ACPI tables comes from the
 * loader and nowhere else.
 */
.set MB2_MAGIC, 0xE85250D6
.set MB2_ARCH,  0                    /* i386 protected mode */

.align 8
mb2_header:
.long MB2_MAGIC
.long MB2_ARCH
.long mb2_header_end - mb2_header
.long -(MB2_MAGIC + MB2_ARCH + (mb2_header_end - mb2_header))

    /* What we ask the loader for: command line, modules, memory map
       and a pointer to the ACPI tables (under UEFI there is nowhere
       else to look for it) */
    .align 8
    .short 1                         /* tag: information request */
    .short 0
    .long 8 + 5 * 4
    .long 1                          /* command line */
    .long 3                          /* modules */
    .long 6                          /* memory map */
    .long 14                         /* RSDP (ACPI 1.0) */
    .long 15                         /* RSDP (ACPI 2.0) */

    /* Modules aligned on page boundaries - same as in Multiboot 1 */
    .align 8
    .short 6
    .short 0
    .long 8

    /* A request for a graphics mode with a linear framebuffer.
       Under UEFI there is no VGA text mode - the character buffer at
       0xB8000 simply does not exist - so the only way to show anything
       is the framebuffer handed over by the loader. We then draw the
       characters ourselves.

       Zero means "no preference", which is not laziness: a number here
       WINS over whatever the loader was told to use, so asking for a
       thousand and twenty four by seven hundred and sixty eight is
       asking a laptop with a screen twice that size to use a quarter
       of it. Under UEFI it is worse than that - the choice is whatever
       the firmware happens to expose through GOP, and a mode outside
       that list leaves the loader setting up NOTHING, on a machine
       with no text mode to fall back to.

       So the mode is the loader's to pick, which in practice means
       gfxpayload, which in practice means the panel's own resolution.
       The long-mode header has said this for a while; this one had
       not, and that was the whole of why the desktop came up small. */
    .align 8
    .short 5                         /* tag: video mode */
    .short 0
    .long 20
    .long 0                          /* width - any */
    .long 0                          /* height - any */
    .long 0                          /* depth - any */

    .align 8
    .short 0                         /* end tag */
    .short 0
    .long 8
mb2_header_end:

/* Kernel stack - 16 KiB, aligned to 16 bytes (System V ABI rule) */
.section .bss
.align 16
stack_bottom:
.skip 16384
.global stack_top
stack_top:

.section .text
.global _start
.type _start, @function
_start:
    movl $stack_top, %esp

    /* Arguments for kmain: magic (eax) and the Multiboot info pointer (ebx) */
    pushl %ebx
    pushl %eax
    call kmain

    /* kmain should never return - if it does, stop the CPU */
    cli
1:  hlt
    jmp 1b

.size _start, . - _start

/* Mark this file as not needing an executable stack */
.section .note.GNU-stack, "", @progbits
