// ThreadOS: the way back from a signal handler. SYS64_SIGRETURN is
// ThreadOS call number 10, on the ARM64 kernel as on the PC.
.global __restore
.hidden __restore
.type __restore,%function
__restore:
.global __restore_rt
.hidden __restore_rt
.type __restore_rt,%function
__restore_rt:
	mov x8,#10
	svc #0
	brk #0
