/* Kernel entry through the syscall instruction
 *
 * In long mode an interrupt gate is no longer used to call the kernel.
 * The syscall instruction is faster because it touches no memory: the
 * processor takes the handler address straight from a model-specific
 * register (LSTAR) and leaves the return address and flags in RCX and
 * R11.
 *
 * There is a price, though: syscall does NOT switch stacks. We enter
 * the kernel on the program's stack, and the first thing to do is move
 * onto our own - otherwise all kernel code would run on memory the
 * process can swap out from under it at will.
 */

.section .text
.code64

.global syscall64_entry
.type syscall64_entry, @function
syscall64_entry:
    /* The program's stack is parked in a single scratch cell, because
       at this moment there is not one free register: RAX carries the
       call number, RCX the return address, R11 the flags, and the rest
       are arguments or the program's own registers. Interrupts are off
       here (SFMASK) and there is no call until the value is moved onto
       our own stack, so nobody can swap that cell out from under us.

       Earlier this cell held the program's stack for the WHOLE call,
       and that was a bug: it was enough for a call to put the task to
       sleep while another process entered the kernel at the same time -
       and the first one returned to ring 3 on somebody else's stack.
       Now the value lands on the kernel stack of that particular task
       and nobody else sees it. */
    /* The kernel stack and the scratch cell belong to the CORE, not to
       the whole machine: two cores really can enter here at the same
       time, and one shared cell would mean the second returns to
       ring 3 on somebody else's stack.
       The swapgs instruction exchanges GS_BASE with KERNEL_GS_BASE -
       every core has a pointer to ITS OWN block in the latter, set at
       startup. This is the only way to get that pointer without a
       single free register and without touching the program's stack,
       which we do not trust. */
    swapgs
    movq %rsp, %gs:8                  /* park the program's stack */
    movq %gs:0, %rsp                  /* this core's own kernel stack */

    /* We rebuild EXACTLY the frame an interrupt would have pushed.
     *
     * The syscall instruction pushes nothing - it leaves the return
     * address in RCX and the flags in R11 - so we write those five
     * words ourselves. It costs a dozen or so instructions and a return
     * through iretq rather than sysretq, and it buys one thing: the
     * kernel sees a system call and an interrupt the same way. Without
     * it fork would have nowhere to take the full program state it is
     * supposed to duplicate. */
    pushq $0x1B                  /* ss */
    pushq %gs:8                  /* the program's rsp */
    swapgs                       /* GS goes back to the program's */
    pushq %r11                   /* rflags as left by syscall */
    pushq $0x23                  /* cs: user code, RPL 3 */
    pushq %rcx                   /* rip: return address */
    pushq $0                     /* err_code - a call has none */
    pushq $0                     /* int_no  - nor a vector number */

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

    movq %rsp, %rdi              /* first argument: pointer to the frame */
    call syscall64_handler

    jmp return_to_user_64_frame

.size syscall64_entry, . - syscall64_entry

/* Return to ring 3 from the frame pointed to by RDI.
 *
 * This is what a fork child uses: it gets a copy of the parent's frame
 * and returns through it into ring 3 as if it had just made the system
 * call itself - only with zero in RAX. */
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

    addq $16, %rsp               /* vector number and error code */
    iretq

.size return_to_user_64, . - return_to_user_64

/* Entering ring 3: iretq with a hand-built frame. Never returns.
     rdi = address of the first instruction, rsi = top of the program
     stack, rdx and rcx = the values the program will find in RDI and
     RSI (that is argc and argv - the first two arguments of the
     calling convention) */
.global enter_ring3_64
.type enter_ring3_64, @function
enter_ring3_64:
    movq $0x1B, %rax             /* user data, RPL 3 */
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    pushq $0x1B                  /* ss */
    pushq %rsi                   /* rsp */
    pushq $0x202                 /* rflags with interrupts enabled */
    pushq $0x23                  /* cs: user code, RPL 3 */
    pushq %rdi                   /* rip */

    movq %rdx, %rdi              /* argc */
    movq %rcx, %rsi              /* argv */

    /* The remaining registers go to zero. The program has no reason to
       see what the kernel left in them - and what it leaves there are
       addresses of its own structures, which ring 3 should not even
       know about. */
    xorl %eax, %eax
    xorl %ebx, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
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
/* Per-core block: the kernel stack (offset 0) and the scratch cell for
   the program's stack (offset 8). The layout must match struct
   core_syscall in kernel64/syscall64.c - a _Static_assert there
   watches over it. */
.global syscall64_blocks
syscall64_blocks:
    .fill 8 * 16, 1, 0

/* Code meant to run in ring 3.
 *
 * It sits in the kernel image only so that it can be copied out of
 * there into a frame marked as user accessible. It contains not one
 * absolute address, so it works wherever it lands.
 *
 * First it asks the processor which ring it is really in - only the
 * processor knows the CS selector - and tells the kernel. Then it asks
 * to be terminated. */
.section .rodata
.align 16
.global user_payload_start
.global user_payload_end
user_payload_start:
    movw %cs, %ax                /* code selector: the two lowest bits
                                    are the privilege level */
    movzwq %ax, %rdi
    movq $3, %rax                /* 3 = tell me which ring I am in */
    syscall
    movq $2, %rax                /* 2 = end of program */
    syscall
1:  jmp 1b                       /* the kernel never returns here */
user_payload_end:

.section .note.GNU-stack, "", @progbits
