/* ThreadOS: a cancellation point is a system call like any other,
   through __tos_syscall. The labels have to stay - pthread_cancel.c
   looks at them to tell whether a thread stopped inside the call. */
.text
.global __cp_begin
.hidden __cp_begin
.global __cp_end
.hidden __cp_end
.global __cp_cancel
.hidden __cp_cancel
.hidden __cancel
.global __syscall_cp_asm
.hidden __syscall_cp_asm
.type   __syscall_cp_asm,@function
__syscall_cp_asm:

__cp_begin:
	mov (%rdi),%eax
	test %eax,%eax
	jnz __cp_cancel
	mov %rsi,%rdi           /* the number */
	mov %rdx,%rsi           /* and six arguments, one register down */
	mov %rcx,%rdx
	mov %r8,%rcx
	mov %r9,%r8
	mov 8(%rsp),%r9
	mov 16(%rsp),%rax
	push %rax               /* the seventh goes on the stack */
	call __tos_syscall
	add $8,%rsp
__cp_end:
	ret
__cp_cancel:
	jmp __cancel
