/* ThreadOS: the way back from a signal handler. The kernel puts this
   address under the handler as its return address; SYS64_SIGRETURN is
   ThreadOS call number 10. */
.global __restore_rt
.hidden __restore_rt
.type __restore_rt,@function
__restore_rt:
	mov $10, %rax
	syscall
	ud2
.size __restore_rt,.-__restore_rt
