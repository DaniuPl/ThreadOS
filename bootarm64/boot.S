/* ThreadOS on ARM64 - the first instructions
 *
 * QEMU's virt machine loads the kernel's ELF image where it was linked
 * and jumps to _start with the MMU off, on every core it starts. What
 * there is to do before C can run is short, but each step has a reason:
 *
 *   1. one core only. With -smp the others are normally held off by
 *      the firmware's PSCI until somebody asks for them, but nothing
 *      promises that on other boards, and two cores sharing one stack
 *      is a crash nobody can read. So all but core 0 wait.
 *   2. down to EL1. The kernel runs where an operating system runs. A
 *      machine with virtualization on starts us one level higher, in
 *      the hypervisor's EL2, and that level has to be left on purpose:
 *      say that EL1 is 64-bit, let it read the timer, and return into
 *      it with eret as if from an exception.
 *   3. a stack, and a zeroed .bss - C assumes both.
 *
 * x0 may hold the address of the device tree, the board's description
 * of itself. It is kept for kmain, which also looks at the start of
 * RAM, where QEMU puts the tree for a kernel that is not Linux.
 */

/* From EL2, if that is where this core is, down to EL1; from EL1,
   nothing. Every core goes through it, the first and the ones PSCI
   starts later. Uses x1 only. */
.macro to_el1
    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    cmp     x1, #2
    b.ne    9f
    mov     x1, #(1 << 31)              /* HCR_EL2.RW: EL1 runs AArch64 */
    msr     hcr_el2, x1
    mov     x1, #3                      /* EL1 may read the counter and timer */
    msr     cnthctl_el2, x1
    msr     cntvoff_el2, xzr
    ldr     x1, =0x30D00800             /* SCTLR_EL1: the RES1 bits, MMU off */
    msr     sctlr_el1, x1
    mov     x1, #0x3C5                  /* EL1h, every exception masked */
    msr     spsr_el2, x1
    adr     x1, 9f
    msr     elr_el2, x1
    eret
9:
.endm

.section .text.boot, "ax"
.global _start
_start:
    mrs     x1, mpidr_el1
    and     x1, x1, #0xFF               /* Aff0: the core within its cluster */
    cbz     x1, 1f
0:  wfe                                 /* not core 0: wait for good */
    b       0b

1:  mov     x19, x0                     /* the device tree, perhaps */
    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    adrp    x2, boot_el
    str     w1, [x2, :lo12:boot_el]
    to_el1

2:  ldr     x1, =stack_top
    mov     sp, x1
    /* The FP/SIMD registers trap until allowed. The kernel is built
       not to use them, but a trap here would be silent and fatal. */
    mov     x1, #(3 << 20)
    msr     cpacr_el1, x1
    isb

    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
3:  cmp     x1, x2
    b.hs    4f
    str     xzr, [x1], #8
    b       3b

4:  mov     x0, x19
    bl      kmain
5:  wfi
    b       5b

/* Where PSCI starts every further core, MMU off, with its index in x0.
   Its stack was put in secondary_stacks[index] by the first core. */
.global secondary_entry
secondary_entry:
    mov     x19, x0
    to_el1
    adrp    x1, secondary_stacks
    add     x1, x1, :lo12:secondary_stacks
    ldr     x2, [x1, x19, lsl #3]
    mov     sp, x2
    mov     x1, #(3 << 20)              /* FP/SIMD not trapped, as on core 0 */
    msr     cpacr_el1, x1
    isb
    mov     x0, x19
    bl      secondary_main
6:  wfi
    b       6b

/* Where we were started. In .data, not .bss: it is written before the
   .bss is cleared. */
.section .data
.global boot_el
.align 2
boot_el:
    .word 0

.section .bss
.align 4
stack_bottom:
    .space 16384
stack_top:
