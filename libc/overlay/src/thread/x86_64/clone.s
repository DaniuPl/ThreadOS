/* ThreadOS: no clone - a new thread is SYS64_THREAD, which does not
   take a stack or a TLS block from the caller. Until that is bridged,
   pthread_create and posix_spawn get ENOSYS rather than a Linux system
   call number landing on some unrelated ThreadOS one. */
.text
.global __clone
.hidden __clone
.type   __clone,@function
__clone:
	mov $-38,%rax           /* -ENOSYS */
	ret
