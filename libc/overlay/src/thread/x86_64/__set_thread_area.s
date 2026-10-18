/* ThreadOS: the thread pointer is the FS base, as on Linux - only the
   call that sets it is ThreadOS's own (arch_prctl goes through the same
   translation as everything else, see src/threados/syscall.c) */
.text
.global __set_thread_area
.hidden __set_thread_area
.type __set_thread_area,@function
__set_thread_area:
	mov %rdi,%rdx           /* __tos_syscall(SYS_arch_prctl, */
	movl $0x1002,%esi       /*   ARCH_SET_FS, */
	movl $158,%edi          /*   pointer) */
	jmp __tos_syscall
