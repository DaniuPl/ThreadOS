/* ThreadOS: a thread is SYS64_CLONE (83), which takes a struct
   (kernel64/proc64.h) rather than Linux's six registers:

     entry, stack, tls, parent_tid, clear_tid

   musl calls __clone(func, stack, flags, arg, ptid, tls, ctid). The
   flags are always "a thread of this process", which is the only
   thing SYS64_CLONE makes. func and arg go on the new stack, and the
   new thread starts at 1: below, where it takes them off and calls
   func(arg); what func returns is the thread's exit code (SYS64_EXIT,
   2, which ends only the calling thread). */
.text
.global __clone
.hidden __clone
.type   __clone,@function
__clone:
	and $-16,%rsi
	sub $16,%rsi
	mov %rdi,(%rsi)          /* func */
	mov %rcx,8(%rsi)         /* arg */
	mov 8(%rsp),%rax         /* ctid, the seventh argument */
	sub $40,%rsp
	lea 1f(%rip),%rcx
	mov %rcx,(%rsp)          /* entry */
	mov %rsi,8(%rsp)         /* stack */
	mov %r9,16(%rsp)         /* tls */
	mov %r8,24(%rsp)         /* parent_tid */
	mov %rax,32(%rsp)        /* clear_tid */
	mov %rsp,%rdi
	mov $83,%eax
	syscall
	add $40,%rsp
	test %rax,%rax
	jns 2f
	mov $-11,%rax            /* -EAGAIN: musl turns every failure into it */
2:	ret
1:	xor %ebp,%ebp
	pop %rax                 /* func */
	pop %rdi                 /* arg; the stack is sixteen-aligned again */
	call *%rax
	mov %eax,%edi
	mov $2,%eax
	syscall
	hlt
